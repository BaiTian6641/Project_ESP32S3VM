#include <stdio.h>
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_2));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT));
    printf("ESP32S3VM_BOOT_OK cores=%d flash=%lu\n", chip.cores, (unsigned long)flash_size);
    fflush(stdout);
    for (unsigned tick = 0;; ++tick) {
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_2, tick & 1));
        printf("ESP32S3VM_TICK %u time_us=%lld\n", tick, (long long)esp_timer_get_time());
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
