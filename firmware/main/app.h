/*
 * Internal interface between the Bluetooth glue (bt.c), the I2S output (audio.c),
 * the buttons/LED (ui.c) and power management (power.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BT_NAME_MAX 64

/* A2DP connection state, mirrors esp_a2d_connection_state_t */
enum { BT_CONN_DISCONNECTED = 0, BT_CONN_CONNECTING, BT_CONN_CONNECTED, BT_CONN_DISCONNECTING };

typedef enum {
    BT_KEY_PLAYPAUSE, BT_KEY_NEXT, BT_KEY_PREV,
} bt_key_t;

typedef struct {
    bool inited;
    int conn;               /* BT_CONN_* */
    bool audio_started;
    bool avrc_connected;
    int play_status;        /* esp_avrc_playback_stat_t, 0xFF = unknown */
    uint32_t sample_rate;
    uint8_t volume;         /* 0..127 (AVRCP absolute volume) */
    uint8_t peer[6];
    bool has_peer;
} bt_status_t;

/* --- bt.c --- */
esp_err_t bt_init(void);
void bt_deinit(void);       /* disconnects and waits for the link to drop */
void bt_get_status(bt_status_t *out);
void bt_set_pairing(bool on);   /* discoverable for CONFIG_BTR_PAIRING_TIMEOUT */
bool bt_is_pairing(void);
void bt_tick(void);             /* ends the pairing window; call periodically */
esp_err_t bt_key(bt_key_t key);

/* --- audio.c --- */
esp_err_t audio_init(void);
void audio_set_rate(uint32_t hz);
void audio_set_volume(uint8_t vol);
void audio_stream_event(bool started);
void audio_feed(const uint8_t *buf, uint32_t len);  /* from the A2DP data callback */
void audio_amp_off(void);

/* --- ui.c --- */
esp_err_t ui_init(void);

/* --- power.c --- */
void power_hold(void);          /* first thing at boot: keep the LDO on */
esp_err_t power_init(void);
uint32_t power_battery_mv(void);  /* 0 until the first reading */
bool power_battery_low(void);
void power_off(void) __attribute__((noreturn));
