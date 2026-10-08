/* SPDX-License-Identifier: MIT */
/* Normal ESP-IDF/FreeRTOS firmware. No QOM, MMIO replacement or simulator hook. */
#include <inttypes.h>
#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static unsigned assignments[] = { 0, 1 };

static void heartbeat(void *argument)
{
    const unsigned assigned = *(const unsigned *)argument;
    for (uint32_t iteration = 1;; ++iteration) {
        const int actual = xPortGetCoreID();
        configASSERT((unsigned)actual == assigned);
        if (iteration <= 3 || iteration % 50 == 0) {
            printf("ESP32S3VM_CORE_EXEC assigned=%u core=%d iteration=%" PRIu32
                   " time_us=%" PRId64 "\n", assigned, actual, iteration,
                   esp_timer_get_time());
            fflush(stdout);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    configASSERT(chip.cores == 2);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    ESP_ERROR_CHECK(nvs_flash_init());
    printf("ESP32S3VM_BOOT_OK cores=%d flash=%" PRIu32 "\n", chip.cores, flash_size);
    fflush(stdout);
    for (unsigned core = 0; core < 2; ++core) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            heartbeat, core ? "hostbus-core1" : "hostbus-core0", 4096,
            (void *)&assignments[core], 5, NULL, core);
        configASSERT(created == pdPASS);
    }
}
