idf_component_register(SRCS "pulse_native.c" INCLUDE_DIRS "." REQUIRES esp_driver_ledc esp_driver_pcnt esp_driver_mcpwm esp_driver_sdm esp_driver_gpio esp_adc esp_timer esp_hw_support)
