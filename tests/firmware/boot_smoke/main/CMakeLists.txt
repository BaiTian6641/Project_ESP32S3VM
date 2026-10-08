idf_component_register(SRCS "boot_smoke.c" INCLUDE_DIRS "." REQUIRES esp_driver_gpio esp_timer esp_system spi_flash nvs_flash)
