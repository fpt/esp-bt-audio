/*
 * Buttons and the WS2812B status LED.
 *
 * Power button (SW5, GPIO34 through a 22k/100k divider, high = pressed):
 *   long press (>= 2 s)  power off. The LED turns red once the threshold is reached;
 *                        the power actually drops when the button is released.
 *   The press that powered the board on is ignored until it is released.
 *
 * BT button (SW6, GPIO13, low = pressed):
 *   short press          play / pause
 *   long press (>= 3 s)  pairing mode (drops the current phone, discoverable)
 *
 * LED:
 *   blue, fast blink     pairing (discoverable) or connecting
 *   blue, short blip     waiting for the last phone
 *   blue, steady         connected
 *   green, steady        connected and playing
 *   red blink (overlay)  battery low
 *   red, steady          powering off
 *
 * One task polls both buttons every 20 ms and redraws the LED only when its color
 * changes. Adapted from rp-esp-wifi-bta components/at_btaudio/src/bta_ui.c.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "sdkconfig.h"
#include "app.h"

#define TICK_MS          20
#define DEBOUNCE_TICKS   3        /* 60 ms stable level */
#define PWR_LONG_MS      2000
#define BT_LONG_MS       3000
#define RMT_RES_HZ       10000000 /* 0.1 us per tick */

static const char *TAG = "ui";

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_enc;
static uint32_t s_led_rgb = 0xFFFFFFFF;   /* force the first write */

typedef struct {
    int gpio;
    bool active_high;
    int long_ms;
    bool pressed;
    bool long_fired;
    bool armed;          /* false until first seen released (power button at boot) */
    int stable;
    int64_t press_t;
} button_t;

enum { BTN_NONE, BTN_SHORT, BTN_LONG };

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ------------------------------------------------------------------ LED */

static esp_err_t led_init(int gpio)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &s_led_chan), TAG, "rmt channel");
    /* WS2812B-V6: T0H 0.22-0.38 us, T1H 0.58-1 us, TxL 0.58-1 us. MSB first. */
    rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 7, .level1 = 0, .duration1 = 6 },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc, &s_led_enc), TAG, "rmt encoder");
    return rmt_enable(s_led_chan);
}

static void led_set(uint32_t rgb)
{
    if (!s_led_chan || rgb == s_led_rgb) {
        return;
    }
    s_led_rgb = rgb;
    const uint32_t b = CONFIG_BTR_LED_BRIGHTNESS;
    /* Wire order is G, R, B. */
    static uint8_t grb[3];
    grb[0] = ((rgb >> 8) & 0xFF) * b / 255;
    grb[1] = ((rgb >> 16) & 0xFF) * b / 255;
    grb[2] = (rgb & 0xFF) * b / 255;
    rmt_transmit_config_t tx = { .loop_count = 0 };
    /* The line idles low between updates (20 ms >> 280 us reset), which latches the color. */
    rmt_tx_wait_all_done(s_led_chan, 10);
    rmt_transmit(s_led_chan, s_led_enc, grb, sizeof(grb), &tx);
}

#define RGB_OFF   0x000000
#define RGB_BLUE  0x0000FF
#define RGB_GREEN 0x00FF00
#define RGB_RED   0xFF0000

static uint32_t led_pattern(const bt_status_t *st, int64_t t)
{
    if (power_battery_low() && (t % 2000) < 150) {
        return RGB_RED;
    }
    if (st->conn == BT_CONN_CONNECTED) {
        return st->audio_started ? RGB_GREEN : RGB_BLUE;
    }
    if (st->conn == BT_CONN_CONNECTING || bt_is_pairing()) {
        return (t / 250) % 2 ? RGB_BLUE : RGB_OFF;          /* 2 Hz blink */
    }
    return (t % 2000) < 100 ? RGB_BLUE : RGB_OFF;           /* blip every 2 s */
}

/* ------------------------------------------------------------------ buttons */

static int button_poll(button_t *b, int64_t t)
{
    bool level_pressed = gpio_get_level(b->gpio) == (b->active_high ? 1 : 0);
    int ev = BTN_NONE;
    b->stable = level_pressed == b->pressed ? 0 : b->stable + 1;
    if (b->stable >= DEBOUNCE_TICKS) {
        b->stable = 0;
        b->pressed = level_pressed;
        if (b->pressed) {
            b->press_t = t;
            b->long_fired = false;
        } else if (!b->armed) {
            b->armed = true;
        } else if (!b->long_fired) {
            ev = BTN_SHORT;
        }
    }
    if (b->armed && b->pressed && !b->long_fired && t - b->press_t >= b->long_ms) {
        b->long_fired = true;
        ev = BTN_LONG;
    }
    return ev;
}

static void ui_task(void *arg)
{
    /* The power button is still held from switching on: start "pressed", unarmed. */
    button_t pwr = { .gpio = CONFIG_BTR_PWR_BTN_GPIO, .active_high = true, .long_ms = PWR_LONG_MS,
                     .pressed = true };
    button_t bt = { .gpio = CONFIG_BTR_BT_BTN_GPIO, .active_high = false, .long_ms = BT_LONG_MS,
                    .armed = true };

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t t = now_ms();

        if (button_poll(&pwr, t) == BTN_LONG) {
            led_set(RGB_RED);
            power_off();
        }
        switch (button_poll(&bt, t)) {
        case BTN_SHORT:
            bt_key(BT_KEY_PLAYPAUSE);
            break;
        case BTN_LONG:
            bt_set_pairing(true);
            break;
        default:
            break;
        }

        bt_tick();
        bt_status_t st;
        bt_get_status(&st);
        led_set(led_pattern(&st, t));
    }
}

esp_err_t ui_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BTR_PWR_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,   /* GPIO34: input only, external 100k pull-down */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pwr button");
    io.pin_bit_mask = 1ULL << CONFIG_BTR_BT_BTN_GPIO;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "bt button");

    if (led_init(CONFIG_BTR_LED_GPIO) != ESP_OK) {
        ESP_LOGW(TAG, "LED disabled");
        s_led_chan = NULL;
    }
    led_set(RGB_OFF);
    if (xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
