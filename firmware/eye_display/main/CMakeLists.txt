idf_component_register(
    SRCS "main.c" "fall_detector.c" "care_store.c" "care_portal.c" "care_audio.c" "care_imu.c" "care_indicator.c" "care_ble.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_adc lvgl esp32-camera esp_jpeg
             nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_driver_i2s esp_driver_i2c bt
    EMBED_TXTFILES "dashboard.html"
)
