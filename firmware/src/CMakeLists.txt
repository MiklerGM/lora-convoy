idf_component_register(
    SRCS "main.c" "run.c" "port_esp.c" "sx1262.c" "phy.c" "codec.c" "nmea.c" "node.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_uart esp_driver_usb_serial_jtag esp_timer esp_hw_support
)
