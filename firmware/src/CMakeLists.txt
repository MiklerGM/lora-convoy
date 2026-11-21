idf_component_register(
    SRCS "main.c" "run.c" "ble_nus.c" "port_esp.c" "sx1262.c" "phy.c" "crypto.c" "codec.c" "nmea.c" "node.c" "api.c" "screen.c" "screen_font.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_uart esp_driver_usb_serial_jtag esp_driver_i2c esp_lcd esp_adc esp_timer esp_hw_support bt nvs_flash
)
