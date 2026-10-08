/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native IDF 6.1 fff9895c82d744c7237be8847347bdd1b07c6643 fixture.
 * TRM v1.8 Register 17.1 (p829), section 17.3.6 (p826): CLKGATE
 * controls the CPU1 clock; RUNSTALL stalls execution; RESETING resets CPU1.
 * Primary PDF SHA256 4484bf8a69035ec42a731c58c64ada6fbd1f1618c5559409f134d9ea083f444f
 * https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf
 * esp_cpu.h documents CCOUNT incrementing every CPU clock cycle (not every
 * instruction). core-macros.h issues real RSR/WSR CCOUNT and CCOMPARE.
 * Timer1 is internal interrupt 15, independently of FreeRTOS's timer0.
 * The host writes COMMAND only; all clock changes and measurements are guest
 * instructions. No executable injection, synthetic counter, or PC exception.
 */
#include <stdint.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/system_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/interrupts.h"
#include "xtensa/core-macros.h"

/* Word-indexed ABI consumed by the native lifecycle Python harness. */
enum {
    MAGIC, STAGE, COMMAND, CPU1_REQUEST, CPU1_ACK, CPU0_COUNT, CPU0_LOOPS,
    CPU0_TIME_LO, CPU0_TIME_HI, CPU1_LOOPS, SEED, DELTA, ARMED, COMPARE,
    RESUMED, TIMER_IRQS, TIMER_AT, EXTERNAL_IRQS, EXTERNAL_AT,
    GATE_CPU0_START, GATE_CPU0_END, GATE_TIME_START, GATE_TIME_END,
    GATE_LOOPS_START, GATE_LOOPS_END, GATE_CPU1_LOOPS, GATE_TIMER_IRQS,
    GATE_EXTERNAL_IRQS, GATE_RESUMED, GATE_TIMER_AT, GATE_EXTERNAL_AT,
    STALL_CPU0_START, STALL_CPU0_END, STALL_CPU1_LOOPS, STALL_TIMER_IRQS,
    STALL_RESUMED, STALL_TIMER_AT, RESET_CPU0_START, RESET_CPU0_END,
    CPU_HZ, EXTERNAL_INTNO, WORDS
};
DRAM_ATTR volatile uint32_t counter_state[WORDS];

static void IRAM_ATTR timer_isr(void *arg)
{
    (void)arg;
    counter_state[TIMER_AT] = esp_cpu_get_cycle_count();
    counter_state[TIMER_IRQS]++;
    /* Only the guest compare write acknowledges the timer interrupt. */
    XTHAL_SET_CCOMPARE(1, counter_state[TIMER_AT] + 0x7fffffffU);
}

static void IRAM_ATTR external_isr(void *arg)
{
    (void)arg;
    counter_state[EXTERNAL_AT] = esp_cpu_get_cycle_count();
    counter_state[EXTERNAL_IRQS]++;
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 0);
}

static void IRAM_ATTR cpu1_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_INTERNAL_TIMER1_INTR_SOURCE,
                                  ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                                  timer_isr, NULL, NULL));
    intr_handle_t external;
    /* Leave the FreeRTOS cross-core interrupt sources 0/1 untouched. */
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_FROM_CPU_INTR3_SOURCE,
                                  ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
                                  external_isr, NULL, &external));
    counter_state[EXTERNAL_INTNO] = esp_intr_get_intno(external);
    /* A private spin task must not schedule or take FreeRTOS timer0 after
     * changing CCOUNT. Keep the two actual test IRQs enabled. */
    esp_cpu_intr_disable(~((1U << 15) | (1U << counter_state[EXTERNAL_INTNO])));
    uint32_t previous = 0;
    for (;;) {
        uint32_t request = counter_state[CPU1_REQUEST];
        if (request != previous) {
            esp_cpu_set_cycle_count(counter_state[SEED]);
            counter_state[ARMED] = esp_cpu_get_cycle_count();
            counter_state[COMPARE] = counter_state[ARMED] + counter_state[DELTA];
            XTHAL_SET_CCOMPARE(1, counter_state[COMPARE]);
            counter_state[RESUMED] = 0;
            counter_state[TIMER_IRQS] = 0;
            counter_state[EXTERNAL_IRQS] = 0;
            previous = request;
            counter_state[CPU1_ACK] = request;
            /* CPU0 publishes command 2/4 before restoring the clock/stall.
             * This is the first guest RSR after release, not a cached HMP SR. */
            uint32_t release = request == 1 ? 2 : 4;
            while (counter_state[COMMAND] != release) {
                counter_state[CPU1_LOOPS]++;
            }
            counter_state[RESUMED] = esp_cpu_get_cycle_count();
        }
        counter_state[CPU1_LOOPS]++;
    }
}

static void IRAM_ATTR progress(void)
{
    uint64_t now = esp_timer_get_time();
    counter_state[CPU0_COUNT] = esp_cpu_get_cycle_count();
    counter_state[CPU0_LOOPS]++;
    counter_state[CPU0_TIME_LO] = now;
    counter_state[CPU0_TIME_HI] = now >> 32;
}

static void IRAM_ATTR wait_command(uint32_t command)
{
    while (counter_state[COMMAND] != command) {
        progress();
    }
}

static void IRAM_ATTR arm(uint32_t request)
{
    counter_state[SEED] = request == 1 ? 0x13572468U : 0x24681357U;
    counter_state[DELTA] = counter_state[CPU_HZ] / 5; /* 200 ms of CPU cycles */
    counter_state[CPU1_REQUEST] = request;
    while (counter_state[CPU1_ACK] != request) {
        progress();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(cpu1_task, "counter1", 4096,
                    NULL, configMAX_PRIORITIES - 1, NULL, 1) == pdPASS
                    ? ESP_OK : ESP_FAIL);
    counter_state[MAGIC] = 0xc001cafe;
    counter_state[CPU_HZ] = esp_clk_cpu_freq();
    counter_state[STAGE] = 1;
    wait_command(1);
    arm(1);
    REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, 0);
    counter_state[GATE_CPU0_START] = esp_cpu_get_cycle_count();
    counter_state[GATE_TIME_START] = esp_timer_get_time();
    counter_state[GATE_LOOPS_START] = counter_state[CPU0_LOOPS];
    counter_state[GATE_CPU1_LOOPS] = counter_state[CPU1_LOOPS];
    counter_state[STAGE] = 2;
    /* Deliberately outlast the compare deadline while CPU0 executes. */
    while ((uint32_t)(esp_cpu_get_cycle_count() -
                      counter_state[GATE_CPU0_START]) < counter_state[DELTA] * 3) {
        progress();
    }
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_3_REG, 1);
    counter_state[GATE_CPU0_END] = esp_cpu_get_cycle_count();
    counter_state[GATE_TIME_END] = esp_timer_get_time();
    counter_state[GATE_LOOPS_END] = counter_state[CPU0_LOOPS];
    counter_state[GATE_TIMER_IRQS] = counter_state[TIMER_IRQS];
    counter_state[GATE_EXTERNAL_IRQS] = counter_state[EXTERNAL_IRQS];
    counter_state[STAGE] = 3;
    wait_command(2);
    REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
    while (!counter_state[TIMER_IRQS] || !counter_state[EXTERNAL_IRQS] ||
           !counter_state[RESUMED]) {
        progress();
    }
    counter_state[GATE_RESUMED] = counter_state[RESUMED];
    counter_state[GATE_TIMER_AT] = counter_state[TIMER_AT];
    counter_state[GATE_EXTERNAL_AT] = counter_state[EXTERNAL_AT];
    counter_state[STAGE] = 4;
    wait_command(3);
    arm(2);
    REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG,
              SYSTEM_CONTROL_CORE_1_CLKGATE_EN | SYSTEM_CONTROL_CORE_1_RUNSTALL);
    counter_state[STALL_CPU0_START] = esp_cpu_get_cycle_count();
    counter_state[STALL_CPU1_LOOPS] = counter_state[CPU1_LOOPS];
    counter_state[STAGE] = 5;
    while ((uint32_t)(esp_cpu_get_cycle_count() -
                      counter_state[STALL_CPU0_START]) < counter_state[DELTA] * 3) {
        progress();
    }
    counter_state[STALL_CPU0_END] = esp_cpu_get_cycle_count();
    counter_state[STALL_TIMER_IRQS] = counter_state[TIMER_IRQS];
    counter_state[STAGE] = 6;
    wait_command(4);
    REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
    while (!counter_state[TIMER_IRQS] || !counter_state[RESUMED]) {
        progress();
    }
    counter_state[STALL_RESUMED] = counter_state[RESUMED];
    counter_state[STALL_TIMER_AT] = counter_state[TIMER_AT];
    counter_state[STAGE] = 7;
    wait_command(5);
    REG_WRITE(RTC_CNTL_STORE0_REG, 0xc001cafe);
    counter_state[RESET_CPU0_START] = esp_cpu_get_cycle_count();
    REG_WRITE(SYSTEM_CORE_1_CONTROL_0_REG,
              SYSTEM_CONTROL_CORE_1_CLKGATE_EN | SYSTEM_CONTROL_CORE_1_RESETING);
    counter_state[STAGE] = 8;
    for (;;) {
        progress();
        counter_state[RESET_CPU0_END] = esp_cpu_get_cycle_count();
    }
}
