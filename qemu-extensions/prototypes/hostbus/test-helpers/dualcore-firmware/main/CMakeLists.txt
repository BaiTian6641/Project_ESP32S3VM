idf_component_register(SRCS "dualcore.c" INCLUDE_DIRS "."
                       REQUIRES esp_hw_support spi_flash esp_timer freertos nvs_flash)
