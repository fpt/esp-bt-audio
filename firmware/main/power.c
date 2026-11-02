/*
 * Power latch, battery monitor and automatic power-off.
 *
 * SW5 turns the RT9080 on through D3; GPIO15 keeps it on through D1. Driving
 * GPIO15 low turns the board off once SW5 is released.
 *
 * The battery is read on GPIO35 (ADC1) through a 100k/100k divider. Below
 * CONFIG_BTR_BAT_LOW_MV the LED warns; below CONFIG_BTR_BAT_OFF_MV the board turns
 * itself off (the TP4056 module's protection cuts out much lower, ~2.5 V).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "app.h"

#define POLL_MS        1000
#define BAT_SAMPLES    16
#define OFF_CONFIRM    5         /* consecutive low readings before powering off */

static const char *TAG = "power";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static volatile uint32_t s_bat_mv;

void power_hold(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BTR_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_BTR_POWER_HOLD_GPIO, 1);
    gpio_config(&io);
}

static uint32_t read_battery_mv(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    return n ? (uint32_t)(sum / n) * 2 : 0;   /* 1/2 divider */
}

void power_off(void)
{
    ESP_LOGI(TAG, "power off");
    audio_amp_off();
    bt_deinit();
    gpio_set_level(CONFIG_BTR_POWER_HOLD_GPIO, 0);
    /* Still running while SW5 is held (it feeds the LDO enable directly). */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void power_task(void *arg)
{
    int low_count = 0;
    int64_t idle_since = esp_timer_get_time();
    for (;;) {
        s_bat_mv = read_battery_mv();
        if (s_bat_mv && s_bat_mv < CONFIG_BTR_BAT_OFF_MV) {
            if (++low_count >= OFF_CONFIRM) {
                ESP_LOGW(TAG, "battery empty (%lu mV)", (unsigned long)s_bat_mv);
                power_off();
            }
        } else {
            low_count = 0;
        }

#if CONFIG_BTR_IDLE_OFF_MIN > 0
        bt_status_t st;
        bt_get_status(&st);
        if (st.conn == BT_CONN_CONNECTED) {
            idle_since = esp_timer_get_time();
        } else if (esp_timer_get_time() - idle_since >= (int64_t)CONFIG_BTR_IDLE_OFF_MIN * 60 * 1000000) {
            ESP_LOGI(TAG, "no connection for %d min", CONFIG_BTR_IDLE_OFF_MIN);
            power_off();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t power_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(CONFIG_BTR_BAT_ADC_GPIO, &unit, &s_chan), TAG, "adc pin");
    ESP_RETURN_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG, TAG, "battery pin must be ADC1 (ADC2 is used by the radio)");
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "adc unit");
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &ccfg), TAG, "adc channel");
    adc_cali_line_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_line_fitting(&cal, &s_cali), TAG, "adc cali");

    s_bat_mv = read_battery_mv();
    ESP_LOGI(TAG, "battery %lu mV", (unsigned long)s_bat_mv);
    if (xTaskCreate(power_task, "power", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t power_battery_mv(void)
{
    return s_bat_mv;
}

bool power_battery_low(void)
{
    return s_bat_mv && s_bat_mv < CONFIG_BTR_BAT_LOW_MV;
}
