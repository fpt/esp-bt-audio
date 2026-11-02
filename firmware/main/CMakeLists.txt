idf_component_register(
    SRCS main.c bt.c audio.c ui.c power.c
    INCLUDE_DIRS .
    PRIV_REQUIRES bt nvs_flash esp_driver_i2s esp_driver_gpio esp_driver_rmt esp_adc esp_ringbuf esp_timer freertos)
