idf_component_register(
    SRCS "app_main.cpp"
         "system/power_management.cpp"
         "display/display_driver.cpp"
         "input/brightness_button.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_psram
                  espressif__esp_lcd_co5300
)
