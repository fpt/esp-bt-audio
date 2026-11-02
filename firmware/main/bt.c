/*
 * Bluetooth Classic glue: GAP (pairing), A2DP sink, AVRCP controller + target.
 *
 * Callbacks run in the Bluedroid BTC task (A2DP data in the sink decode task).
 * Shared state is guarded by s_lock.
 *
 * Adapted from rp-esp-wifi-bta components/at_btaudio/src/bta_bt.c, without the AT
 * layer, Wi-Fi switching and metadata (no display on this board).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "sdkconfig.h"
#include "app.h"

#define NVS_NS        "btaudio"
#define NVS_KEY_PEER  "peer"
#define NVS_KEY_VOL   "vol"

#define BDA_FMT       "%02x:%02x:%02x:%02x:%02x:%02x"
#define BDA_ARG(b)    (b)[0], (b)[1], (b)[2], (b)[3], (b)[4], (b)[5]

/* Transaction labels: AVRCP allows 0..15, one outstanding command per label. */
enum { TL_RN_CAPS = 0, TL_RN_PLAY, TL_PT };

static const char *TAG = "bt";

static SemaphoreHandle_t s_lock;
static bt_status_t s_st = { .play_status = 0xFF, .sample_rate = 44100, .volume = 64 };
static int s_scan_mode = -1;     /* 0 none, 1 connectable, 2 connectable + discoverable */
static int64_t s_pair_deadline_us;
static bool s_vol_ntf_pending;   /* peer registered for ESP_AVRC_RN_VOLUME_CHANGE */
static esp_avrc_rn_evt_cap_mask_t s_peer_rn_caps;
static uint8_t s_pt_tl;
static volatile bool s_deiniting;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static esp_err_t set_scan_mode(int mode);

/* ------------------------------------------------------------------ NVS */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_st.peer);
    s_st.has_peer = nvs_get_blob(h, NVS_KEY_PEER, s_st.peer, &len) == ESP_OK && len == sizeof(s_st.peer);
    uint8_t vol;
    if (nvs_get_u8(h, NVS_KEY_VOL, &vol) == ESP_OK && vol <= 127) {
        s_st.volume = vol;
    }
    nvs_close(h);
}

static void nvs_save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, data, len);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------ GAP */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "auth failed: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* IO capability is NoInputNoOutput, so this is Just Works. */
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey %06lu", (unsigned long)param->key_notif.passkey);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        size_t n = strlen(CONFIG_BTR_PIN_CODE);
        if (n > ESP_BT_PIN_CODE_LEN) {
            n = ESP_BT_PIN_CODE_LEN;
        }
        memcpy(pin, CONFIG_BTR_PIN_CODE, n);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, n, pin);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ A2DP */

static uint32_t sbc_rate(const esp_a2d_mcc_t *mcc)
{
    uint8_t oct0 = mcc->cie.sbc[0];
    if (oct0 & (1 << 6)) {
        return 32000;
    } else if (oct0 & (1 << 5)) {
        return 44100;
    } else if (oct0 & (1 << 4)) {
        return 48000;
    }
    return 16000;
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = param->conn_stat.remote_bda;
        int state = param->conn_stat.state;
        LOCK();
        s_st.conn = state;
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            memcpy(s_st.peer, bda, 6);
            s_st.has_peer = true;
        }
        if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_st.audio_started = false;
            s_st.play_status = 0xFF;
        }
        UNLOCK();
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            nvs_save_blob(NVS_KEY_PEER, bda, 6);
            /* Stop advertising while connected; restore on disconnect. */
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            audio_stream_event(false);
            if (s_scan_mode >= 0 && !s_deiniting) {
                set_scan_mode(s_scan_mode);
            }
        }
        ESP_LOGI(TAG, "A2DP state %d " BDA_FMT, state, BDA_ARG(bda));
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        LOCK();
        s_st.audio_started = started;
        UNLOCK();
        audio_stream_event(started);
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint32_t rate = sbc_rate(&param->audio_cfg.mcc);
            LOCK();
            s_st.sample_rate = rate;
            UNLOCK();
            audio_set_rate(rate);
            ESP_LOGI(TAG, "SBC %lu Hz", (unsigned long)rate);
        }
        break;
    default:
        break;
    }
}

static void a2d_data_cb(const uint8_t *buf, uint32_t len)
{
    audio_feed(buf, len);
}

/* ------------------------------------------------------------------ AVRCP CT */

static void avrc_register_ntf(uint8_t event_id, uint8_t tl)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_peer_rn_caps, event_id)) {
        esp_avrc_ct_send_register_notification_cmd(tl, event_id, 0);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        LOCK();
        s_st.avrc_connected = param->conn_stat.connected;
        UNLOCK();
        if (param->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(TL_RN_CAPS);
        } else {
            memset(&s_peer_rn_caps, 0, sizeof(s_peer_rn_caps));
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_peer_rn_caps = param->get_rn_caps_rsp.evt_set;
        avrc_register_ntf(ESP_AVRC_RN_PLAY_STATUS_CHANGE, TL_RN_PLAY);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            LOCK();
            s_st.play_status = param->change_ntf.event_parameter.playback;
            UNLOCK();
            avrc_register_ntf(ESP_AVRC_RN_PLAY_STATUS_CHANGE, TL_RN_PLAY);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ AVRCP TG */

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        if (!param->conn_stat.connected) {
            s_vol_ntf_pending = false;
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        /* The phone sends absolute volume and leaves the scaling to us. */
        uint8_t vol = param->set_abs_vol.volume & 0x7F;
        LOCK();
        s_st.volume = vol;
        UNLOCK();
        audio_set_volume(vol);
        nvs_save_u8(NVS_KEY_VOL, vol);
        break;
    }

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            esp_avrc_rn_param_t rv = { .volume = s_st.volume };
            s_vol_ntf_pending = true;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rv);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ public */

static esp_err_t set_scan_mode(int mode)
{
    s_scan_mode = mode;
    /* While a source is connected stay hidden; the mode applies after disconnect. */
    if (s_st.conn == BT_CONN_CONNECTED) {
        return ESP_OK;
    }
    esp_bt_connection_mode_t c = mode >= 1 ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE;
    esp_bt_discovery_mode_t d = mode >= 2 ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE;
    return esp_bt_gap_set_scan_mode(c, d);
}

esp_err_t bt_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_load();
    audio_set_volume(s_st.volume);

    /* Classic BT only: give the BLE controller memory back to the heap. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "controller enable");
    esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bcfg.ssp_en = true;
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bcfg), TAG, "bluedroid init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = {0};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);
    esp_bt_gap_set_device_name(CONFIG_BTR_DEVICE_NAME);

    /* Order follows the IDF a2dp_sink example: AVRCP before A2DP. */
    ESP_RETURN_ON_ERROR(esp_avrc_ct_init(), TAG, "avrc ct");
    esp_avrc_ct_register_callback(avrc_ct_cb);
    ESP_RETURN_ON_ERROR(esp_avrc_tg_init(), TAG, "avrc tg");
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    ESP_RETURN_ON_ERROR(esp_a2d_sink_init(), TAG, "a2dp sink");

    s_st.inited = true;
    bt_set_pairing(true);
    if (s_st.has_peer) {
        esp_a2d_sink_connect(s_st.peer);
    }
    ESP_LOGI(TAG, "ready as \"%s\"", CONFIG_BTR_DEVICE_NAME);
    return ESP_OK;
}

void bt_deinit(void)
{
    if (!s_st.inited) {
        return;
    }
    s_deiniting = true;
    if (s_st.conn == BT_CONN_CONNECTED || s_st.conn == BT_CONN_CONNECTING) {
        esp_a2d_sink_disconnect(s_st.peer);
        /* Let the phone see a clean disconnect before the power goes. */
        for (int i = 0; i < 40 && s_st.conn != BT_CONN_DISCONNECTED; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

void bt_get_status(bt_status_t *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
}

void bt_set_pairing(bool on)
{
    if (!s_st.inited) {
        return;
    }
    if (on) {
        /* Drop the current source so another phone can pair; the discoverable
         * mode is applied on the disconnect event. */
        if (s_st.conn == BT_CONN_CONNECTED) {
            esp_a2d_sink_disconnect(s_st.peer);
        }
        s_pair_deadline_us = CONFIG_BTR_PAIRING_TIMEOUT > 0
            ? esp_timer_get_time() + (int64_t)CONFIG_BTR_PAIRING_TIMEOUT * 1000000 : INT64_MAX;
        set_scan_mode(2);
        ESP_LOGI(TAG, "pairing mode");
    } else {
        s_pair_deadline_us = 0;
        set_scan_mode(1);
    }
}

bool bt_is_pairing(void)
{
    return s_pair_deadline_us != 0;
}

void bt_tick(void)
{
    /* End the pairing window on connect or timeout; stay connectable so the
     * paired phone can come back. */
    if (s_pair_deadline_us &&
        (s_st.conn == BT_CONN_CONNECTED || esp_timer_get_time() >= s_pair_deadline_us)) {
        bt_set_pairing(false);
    }
}

esp_err_t bt_key(bt_key_t key)
{
    if (!s_st.avrc_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t code;
    switch (key) {
    case BT_KEY_PLAYPAUSE:
        code = s_st.play_status == ESP_AVRC_PLAYBACK_PLAYING ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
        break;
    case BT_KEY_NEXT: code = ESP_AVRC_PT_CMD_FORWARD; break;
    case BT_KEY_PREV: code = ESP_AVRC_PT_CMD_BACKWARD; break;
    default:          return ESP_ERR_INVALID_ARG;
    }
    /* Rotate labels so a press/release pair never collides with the notification labels. */
    uint8_t tl = TL_PT + (s_pt_tl++ & 7);
    ESP_RETURN_ON_ERROR(esp_avrc_ct_send_passthrough_cmd(tl, code, ESP_AVRC_PT_CMD_STATE_PRESSED), TAG, "press");
    return esp_avrc_ct_send_passthrough_cmd(tl, code, ESP_AVRC_PT_CMD_STATE_RELEASED);
}
