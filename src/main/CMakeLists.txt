idf_component_register(
    SRCS "main.c" "display.c" "splash_asset.c" "keyboard.c" "eeprom.c" "storage.c" "image.c" "image_timezones.c" "image_crypto.c" "donor.c" "sound.c" "sound_wave.c" "battery.c" "battery_model.c" "settings.c" "wifi_manager.c" "web_files.c" "operation_guard.c"
    EMBED_TXTFILES "index.html"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_sdspi esp_lcd fatfs sdmmc esp_hw_support bootloader_support esp_driver_i2s nvs_flash esp_adc esp_driver_ledc esp_wifi esp_netif esp_event esp_http_server
)
