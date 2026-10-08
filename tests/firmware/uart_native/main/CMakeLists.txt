idf_component_register(SRCS "uart_native.c" "uart_uhci_native.c" INCLUDE_DIRS "." REQUIRES esp_driver_uart esp_driver_gpio esp_timer esp_rom)
