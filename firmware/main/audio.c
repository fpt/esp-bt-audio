/*
 * Decoded A2DP PCM -> I2S -> PT8211, and the LM4881 SHUTDOWN line.
 *
 * The A2DP sink data callback pushes PCM into a byte ring buffer without
 * blocking; audio_task applies the AVRCP volume and writes to I2S. The DMA
 * sends zeros (mid-scale on the PT8211) whenever the buffer runs dry.
 *
 * PT8211 is LSB-justified 16 bit. With 16-bit slots that is the same waveform as
 * MSB-justified, so the standard driver's MSB mode is used (BCK = 32 fs, no MCLK).
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
#ifdef CONFIG_BTR_SWAP_LR
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
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BTR_I2S_BCK_GPIO,
            .ws = CONFIG_BTR_I2S_WS_GPIO,
            .dout = CONFIG_BTR_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s std");
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
    s_streaming = started;
    if (started) {
        s_stop_us = 0;
    } else {
        s_stop_us = esp_timer_get_time();
        drain_ringbuf();
    }
}

void audio_feed(const uint8_t *buf, uint32_t len)
{
    if (!s_streaming) {
        return;
    }
    if (xRingbufferSend(s_rb, buf, len & ~3u, 0) != pdTRUE) {
        s_dropped += len;
    }
}

void audio_amp_off(void)
{
    s_streaming = false;
    s_stop_us = 0;
    amp_set(false);
}
