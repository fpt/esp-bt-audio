/*
 * Decoded A2DP PCM -> I2S -> DAC, and the LM4881 SHUTDOWN line.
 *
 * The A2DP sink data callback pushes PCM into a byte ring buffer without
 * blocking; audio_task applies the AVRCP volume and writes to I2S. The DMA
 * sends zeros (mid-scale) whenever the buffer runs dry.
 *
 * Both DACs get 16-bit slots (BCK = 32 fs) and no MCLK.
 * - PT8211 is LSB-justified 16 bit. With 16-bit slots that is the same waveform as
 *   MSB-justified, so the standard driver's MSB mode is used. It takes the right
 *   channel in the first slot (WS low), the reverse of I2S, so process() writes R
 *   first. (slot_cfg.ws_pol has no effect on the ESP32.)
 * - A standard I2S DAC (UDA1334A, PCM5102A) gets Philips I2S, left first.
 *
 * The amp turns on when a stream starts and off CONFIG_BTR_AMP_OFF_DELAY_MS after
 * it stops, so short pauses don't click.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "app.h"

#define RINGBUF_SIZE  16384   /* ~90 ms at 44.1 kHz stereo */
#define CHUNK_MAX     2048

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static RingbufHandle_t s_rb;
static volatile uint32_t s_pending_rate;
static volatile uint16_t s_gain_q14 = 1 << 14;
static volatile bool s_streaming;
static volatile int64_t s_stop_us;    /* when the stream stopped, 0 = amp should stay as is */
static bool s_amp_on;
static uint32_t s_dropped;
static volatile uint32_t s_underruns; /* DMA buffers sent as zeros because no data was queued */
static bool s_tone;          /* test tone owns the output; Bluetooth data is ignored */

static void amp_set(bool on)
{
    if (on != s_amp_on) {
        s_amp_on = on;
        gpio_set_level(CONFIG_BTR_AMP_SD_GPIO, on ? 0 : 1);
        ESP_LOGI(TAG, "amp %s", on ? "on" : "off");
    }
}

static void drain_ringbuf(void)
{
    size_t n;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_rb, &n, 0, RINGBUF_SIZE)) != NULL) {
        vRingbufferReturnItem(s_rb, p);
    }
}

static void process(int16_t *s, size_t samples)
{
    uint32_t g = s_gain_q14;
    for (size_t i = 0; i < samples; i += 2) {
        int32_t l = s[i], r = s[i + 1];
        if (g < (1 << 14)) {
            l = (l * (int32_t)g) >> 14;
            r = (r * (int32_t)g) >> 14;
        }
        /* The DAC's slot order (see the top), unless the board swaps the channels. */
#if defined(CONFIG_BTR_DAC_PT8211) != defined(CONFIG_BTR_SWAP_LR)
        s[i] = (int16_t)r;
        s[i + 1] = (int16_t)l;
#else
        s[i] = (int16_t)l;
        s[i + 1] = (int16_t)r;
#endif
    }
}

static void apply_rate(uint32_t hz)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    i2s_channel_disable(s_tx);
    if (i2s_channel_reconfig_std_clock(s_tx, &clk) != ESP_OK) {
        ESP_LOGE(TAG, "rate %lu Hz failed", (unsigned long)hz);
    }
    i2s_channel_enable(s_tx);
    ESP_LOGI(TAG, "I2S %lu Hz", (unsigned long)hz);
}

/* Every 10 s while streaming: what reached I2S, to tell data problems from
 * analog ones. */
static uint32_t s_stat_bytes, s_stat_fed;
static int s_stat_peak;
static int64_t s_stat_t;

static void stats(const int16_t *s, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > s_stat_peak) {
            s_stat_peak = v;
        }
    }
    s_stat_bytes += samples * 2;
    int64_t now = esp_timer_get_time();
    if (now - s_stat_t >= 10000000) {
        ESP_LOGI(TAG, "10 s: in %lu B, out %lu B, dropped %lu, underruns %lu, peak %d, gain %u/16384, first %04x %04x",
                 (unsigned long)s_stat_fed, (unsigned long)s_stat_bytes, (unsigned long)s_dropped,
                 (unsigned long)s_underruns, s_stat_peak, s_gain_q14, (uint16_t)s[0], (uint16_t)s[1]);
        s_underruns = 0;
        s_stat_t = now;
        s_stat_bytes = s_stat_fed = 0;
        s_stat_peak = 0;
    }
}

/* TX "send queue overflow": the DMA reached a buffer nobody refilled. */
static IRAM_ATTR bool on_underrun(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    s_underruns++;
    return false;
}

static void audio_task(void *arg)
{
    static uint8_t chunk[CHUNK_MAX];
    for (;;) {
        uint32_t rate = s_pending_rate;
        if (rate) {
            s_pending_rate = 0;
            apply_rate(rate);
        }

        if (s_streaming) {
            amp_set(true);
        } else if (s_stop_us && esp_timer_get_time() - s_stop_us >= (int64_t)CONFIG_BTR_AMP_OFF_DELAY_MS * 1000) {
            s_stop_us = 0;
            amp_set(false);
        }

        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(50), CHUNK_MAX);
        if (!p) {
            continue;
        }
        /* Writes are whole stereo frames and the buffer size is a multiple of 4;
         * mask anyway so L/R never desync. */
        size_t len = n & ~(size_t)3;
        memcpy(chunk, p, len);
        vRingbufferReturnItem(s_rb, p);
        if (len == 0) {
            continue;
        }
        process((int16_t *)chunk, len / 2);
        stats((int16_t *)chunk, len / 2);
        size_t written;
        i2s_channel_write(s_tx, chunk, len, &written, portMAX_DELAY);
    }
}

esp_err_t audio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BTR_AMP_SD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_BTR_AMP_SD_GPIO, 1);   /* amp in shutdown until audio */
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "amp gpio");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;   /* send zeros on underrun instead of repeating old data */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "i2s channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
#if CONFIG_BTR_DAC_PT8211
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
#else
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
#endif
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BTR_I2S_BCK_GPIO,
            .ws = CONFIG_BTR_I2S_WS_GPIO,
            .dout = CONFIG_BTR_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s std");
    i2s_event_callbacks_t cbs = { .on_send_q_ovf = on_underrun };
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, NULL), TAG, "i2s cb");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");

    s_rb = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_rb) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "audio", 3072, NULL, configMAX_PRIORITIES - 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_set_rate(uint32_t hz)
{
    s_pending_rate = hz;
}

void audio_set_volume(uint8_t vol)
{
    if (vol > 127) {
        vol = 127;
    }
    /* Square law: a rough perceptual taper, 127 -> unity. */
    s_gain_q14 = (uint16_t)(((uint32_t)vol * vol * (1 << 14)) / (127 * 127));
}

void audio_stream_event(bool started)
{
    if (s_tone) {
        return;
    }
    s_streaming = started;
    if (started) {
        s_stop_us = 0;
        /* Count only what happens during playback; idle DMA sends zeros on purpose. */
        s_underruns = 0;
        s_stat_t = esp_timer_get_time();
    } else {
        s_stop_us = esp_timer_get_time();
        drain_ringbuf();
    }
}

void audio_feed(const uint8_t *buf, uint32_t len)
{
    if (s_tone) {
        return;
    }
    if (!s_streaming) {
        return;
    }
    s_stat_fed += len;
    if (xRingbufferSend(s_rb, buf, len & ~3u, 0) != pdTRUE) {
        s_dropped += len;
    }
}

#if CONFIG_BTR_TEST_TONE
#include <math.h>

static void tone_task(void *arg)
{
    /* 441 frames = 10 ms at 44.1 kHz: whole cycles of 1 kHz and 500 Hz. */
    static int16_t buf[441 * 2];
    for (int i = 0; i < 441; i++) {
        buf[2 * i]     = (int16_t)(8192 * sinf(2 * (float)M_PI * 1000 * i / 44100));
        buf[2 * i + 1] = (int16_t)(8192 * sinf(2 * (float)M_PI * 500 * i / 44100));
    }
    for (;;) {
        xRingbufferSend(s_rb, buf, sizeof(buf), portMAX_DELAY);
    }
}

void audio_test_tone(void)
{
    ESP_LOGW(TAG, "test tone: L 1 kHz, R 500 Hz, -12 dBFS");
    s_gain_q14 = 1 << 14;
    audio_stream_event(true);
    s_tone = true;
    xTaskCreate(tone_task, "tone", 2048, NULL, 5, NULL);
}
#endif

void audio_amp_off(void)
{
    s_streaming = false;
    s_stop_us = 0;
    amp_set(false);
}
