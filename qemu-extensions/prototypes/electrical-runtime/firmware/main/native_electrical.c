/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile unsigned interrupts;
static volatile unsigned irq_level;

static void input_irq(void *arg)
{
    (void)arg;
    ++interrupts;
    irq_level = gpio_get_level(GPIO_NUM_5);
}

void app_main(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << GPIO_NUM_5,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    printf("NATIVE_ELECTRICAL_BOOT\n");
    ESP_ERROR_CHECK(gpio_config(&out));
    ESP_ERROR_CHECK(gpio_config(&in));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_NUM_5, input_irq, NULL));
    printf("NATIVE_ELECTRICAL_READY\n");
    fflush(stdout);
    for (unsigned tick = 0;; ++tick) {
        int output = tick & 1;
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_4, output));
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("NATIVE_ELECTRICAL_OBS tick=%u out=%d input=%d irq=%u irq_level=%u\n",
               tick, output, gpio_get_level(GPIO_NUM_5), interrupts, irq_level);
        fflush(stdout);
    }
}
