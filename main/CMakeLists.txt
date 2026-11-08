idf_component_register(
    SRCS "app_main.cpp"
         "system/power_management.cpp"
         "input/orientation.cpp"
         "display/display_driver.cpp"
         "display/brightness_settings.cpp"
         "image/jpeg_decoder.cpp"
         "image/png_decoder.cpp"
         "image/bmp_decoder.cpp"
         "image/image_store.cpp"
         "input/brightness_button.cpp"
         "input/noise_level.cpp"
         "input/touch_brightness.cpp"
         "network/network_manager.cpp"
         "network/network_api.cpp"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../web/index.html"
                  "../web/style.css"
                  "../web/script.js"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_lcd esp_psram
                  esp_event esp_netif esp_wifi nvs_flash spi_flash esp_timer esp_http_server espressif__cjson
                  espressif__esp_lcd_co5300 espressif__mdns espressif__esp_jpeg espressif__libpng waveshare__qmi8658
                  espressif__esp_codec_dev waveshare__esp_lcd_touch_cst9217
)
