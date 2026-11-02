/*
 * ESP32 Bluetooth audio receiver: A2DP sink -> PT8211 -> LM4881 headphones.
 * See docs/esp_bt_audio_receiver.md for the hardware.
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "app.h"

static const char *TAG = "main";

void app_main(void)
{
    /* Before anything slow: the user may let go of the power button any moment. */
    power_hold();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(bt_init());
    ESP_LOGI(TAG, "running, battery %lu mV", (unsigned long)power_battery_mv());
}
