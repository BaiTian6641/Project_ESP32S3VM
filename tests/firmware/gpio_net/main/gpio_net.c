/*
 * gpio_net — H-NET-02 firmware fixture (real IDF gpio driver).
 *
 * 1. Configures GPIO4 as a push-pull output with gpio_config, toggles it
 *    and reads the resolved pad level back through gpio_get_level
 *    (GPIO_IN) — electrical loopback of the same pad.
 * 2. Configures GPIO5 as an input with a rising-edge interrupt registered
 *    through gpio_isr_register, which routes ETS_GPIO_INTR_SOURCE through
 *    the interrupt matrix to the CPU.
 *
 * The QEMU side proves the ISR path: an external qtest client drives the
 * model's "gpio-in" line for pad 5 (script run-gpio-net-fixture.sh in
 * build-runtime-state/gpio-2026-10-07).  The mechanism is documented:
 * no firmware hook, the ISR must fire because the model raises the GPIO
 * interrupt.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#define OUT_PIN GPIO_NUM_4
#define IN_PIN  GPIO_NUM_5

static volatile unsigned s_isr_count;
static volatile unsigned s_isr_level;

static void gpio_net_isr(void *arg)
{
    (void)arg;
    /* Low-level global ISR (gpio_isr_register): the caller owns the
     * status ack.  Write the hardware W1TC register — the GPIO model
     * lowers its matrix source line on this exact write. */
    REG_WRITE(GPIO_STATUS_W1TC_REG, BIT(IN_PIN));
    s_isr_count++;
    s_isr_level = gpio_get_level(IN_PIN);
}

void app_main(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << OUT_PIN,
        /* INPUT_OUTPUT: the driver enables the input buffer (FUN_IE) so
         * gpio_get_level can read the pad back while it drives — the
         * documented IDF mode for output readback/loopback. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << IN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };

    printf("GPIO_NET_BOOT\n");
    fflush(stdout);

    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
    ESP_ERROR_CHECK(gpio_isr_register(gpio_net_isr, (void *)IN_PIN, 0, NULL));

    /* Toggle the output and read the resolved pad back via GPIO_IN */
    for (int level = 1; level >= 0; level--) {
        ESP_ERROR_CHECK(gpio_set_level(OUT_PIN, level));
        vTaskDelay(pdMS_TO_TICKS(20));
        int read = gpio_get_level(OUT_PIN);
        printf("GPIO_NET_OUT_LOOPBACK set=%d read=%d %s\n",
               level, read, read == level ? "ok" : "MISMATCH");
        fflush(stdout);
    }

    printf("GPIO_NET_READY isr_count=%u\n", s_isr_count);
    fflush(stdout);

    unsigned last = 0;
    for (unsigned tick = 0;; ++tick) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_isr_count != last) {
            printf("GPIO_NET_ISR_SEEN cnt=%u level=%u\n",
                   s_isr_count, s_isr_level);
            fflush(stdout);
            last = s_isr_count;
        }
        if ((tick % 10) == 0) {
            printf("GPIO_NET_CNT n=%u in=%d\n", tick, gpio_get_level(IN_PIN));
            fflush(stdout);
        }
    }
}
