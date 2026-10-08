/*
 * SPDX-FileCopyrightText: 2026 ESP32S3VM project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP32-S3 interrupt matrix native fixture (CORE-03 / H-CORE-02 firmware
 * evidence).  Exercises the interrupt matrix OR semantics of the reworked
 * QEMU model with normal IDF interrupt allocation on BOTH cores.
 *
 * Source pair: TIMG0 timer0 and TIMG1 timer0 level interrupts
 * (ETS_TG0_T0_LEVEL_INTR_SOURCE / ETS_TG1_T0_LEVEL_INTR_SOURCE).  These are
 * software-schedulable (alarm programmed to fire immediately) and unclaimed
 * by the IDF 6.1 default configuration (task WDT uses TG0_WDT, esp_timer and
 * the FreeRTOS tick use the SYSTIMER counters/alarms 0/1/2, and the FreeRTOS
 * IPC-ISR uses the FROM_CPU_2/3 sources).  FROM_CPU2/3 were the originally
 * requested pair, but on IDF 6.1 they are the esp_ipc_isr channels
 * (components/esp_system/port/arch/xtensa/esp_ipc_isr_port.c routes
 * FROM_CPU_2/3 to ETS_IPC_ISR_INUM at startup): routing application ISRs to
 * them desynchronizes the cross-core IPC protocol (observed: Core 1
 * panic'ed InstrFetchProhibited at PC=0 inside esp_ipc_isr_handler), so
 * they cannot carry a test fixture.
 *
 * What it proves, per pinned core (CPU0 then CPU1, sequential so shared
 * source clearing is never a cross-core race):
 *  - two esp_intr_alloc_intrstatus(..., ESP_INTR_FLAG_SHARED, statusreg =
 *    TIMGn_INT_ST_TIMERS, own bit, ...) allocations land on ONE CPU
 *    interrupt number (esp_intr_get_intno) and both ISRs run, each clearing
 *    only its own timer's INT_CLR bit and recording the core it ran on;
 *  - pattern "basic": both alarms pending together, both ISRs serviced;
 *  - pattern "cross": ISR A arms timer B to fire immediately and then
 *    clears A's raw status while B's level is still asserted.  With a
 *    wired-OR matrix the CPU line stays asserted and ISR B runs; a
 *    direct per-source line model would drop the line on A's clear and
 *    starve B (bounded timeout).
 * Unlike the FROM_CPU pair, nothing else chains on these sources, so this
 * pattern is a genuine firmware-level OR-persistence discriminator.
 *
 * Bounded failure and reset/reboot repeat: one full pass on CPU0 then CPU1
 * per boot; on success esp_restart() repeats the sequence for
 * INTMATRIX_ROUNDS boots using RTC-noinit state (not reloaded by the boot
 * process); any failure prints a FAIL marker and restarts, and the boot
 * budget aborts with OVERALL FAIL.
 *
 * Markers (parseable lines on the UART console):
 *   INTMATRIX boot=N ... / INTMATRIX PASS|FAIL ... / INTMATRIX OVERALL PASS|FAIL
 */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_intr_alloc.h"
#include "esp_cpu.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "soc/interrupts.h"
#include "soc/timer_group_reg.h"

#define INTMATRIX_ROUNDS 2
#define INTMATRIX_WAIT_TICKS 4000 /* bounded wait per expectation */

/* Source pair: TIMG0 timer0 and TIMG1 timer0 (ESP32-S3 TRM Table 9.3-1
 * sources 50 and 53). */
#define SRC_A ETS_TG0_T0_LEVEL_INTR_SOURCE
#define SRC_B ETS_TG1_T0_LEVEL_INTR_SOURCE

/* Register bases and fields per soc/timer_group_reg.h */
#define TIMG0_BASE 0
#define TIMG1_BASE 1

#define ST_REG(g)        TIMG_INT_ST_TIMERS_REG(g)
#define CLR_REG(g)       TIMG_INT_CLR_TIMERS_REG(g)
#define ENA_REG(g)       TIMG_INT_ENA_TIMERS_REG(g)
#define CFG_REG(g)       TIMG_T0CONFIG_REG(g)
#define ALARMLO_REG(g)   TIMG_T0ALARMLO_REG(g)
/* Both groups drive their TIMER0: the T0 interrupt bit is BIT(0) of each
 * group's own INT_ENA/INT_ST/INT_CLR register (T1 is BIT(1) of the same
 * group, unused here). */
#define ST_BIT_A         TIMG_T0_INT_ST /* BIT(0) of TIMG0 */
#define ST_BIT_B         TIMG_T0_INT_ST /* BIT(0) of TIMG1 */
#define CLR_BIT_A        TIMG_T0_INT_CLR /* BIT(0) of TIMG0 */
#define CLR_BIT_B        TIMG_T0_INT_CLR /* BIT(0) of TIMG1 */

/* T0CONFIG fields (soc/timer_group_reg.h) */
#define CFG_DIV(c)       (((c) << TIMG_T0_DIVIDER_S) & TIMG_T0_DIVIDER)
#define CFG_EN           TIMG_T0_EN       /* BIT(31) */
#define CFG_INCREASE     TIMG_T0_INCREASE /* BIT(30) */
#define CFG_ALARM_EN     TIMG_T0_ALARM_EN /* BIT(10) */

#define ALLOC_FLAGS (ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED)

typedef struct {
    intr_handle_t handle;
    volatile uint32_t count;   /* ISR invocations */
    volatile int isr_core;     /* core the ISR last ran on, -1 = none */
} intr_slot_t;

typedef struct {
    intr_slot_t a;             /* TIMG0 T0 */
    intr_slot_t b;             /* TIMG1 T0 */
    int alloc_core;
    int intno_a;
    int intno_b;
    volatile int arm_b_in_isr; /* cross pattern: ISR A raises B before clear */
} core_state_t;

/* RTC-noinit state survives esp_restart() and is not reloaded by the boot
 * process; magic guards the cold-boot garbage case. */
#define INTMATRIX_MAGIC 0x1a7c4a03u
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_count;
static RTC_NOINIT_ATTR uint32_t s_failed;

static core_state_t s_core_state[portNUM_PROCESSORS];
static volatile int s_done_flags[portNUM_PROCESSORS];

/* raw status of timer A (TIMG0 T0) as seen by the matrix status register */
static inline uint32_t st_bit_a(void)
{
    return REG_READ(ST_REG(TIMG0_BASE)) & ST_BIT_A;
}

static inline uint32_t st_bit_b(void)
{
    return REG_READ(ST_REG(TIMG1_BASE)) & ST_BIT_B;
}

/* Program TIMGn timer0: APB/80 base, counting up, counter at 0.  When
 * arm_alarm is true the alarm is set to fire immediately (alarm <= counter
 * triggers at once). */
static void timer_setup(int g, bool arm_alarm)
{
    uint32_t own = (g == TIMG0_BASE) ? CLR_BIT_A : CLR_BIT_B;

    REG_WRITE(CFG_REG(g), 0); /* disable + disarm while configuring */
    /* Enable only this timer's interrupt; keep other bits (e.g. the
     * task-watchdog WDT bit on TIMG0) untouched. */
    REG_WRITE(ENA_REG(g), REG_READ(ENA_REG(g)) | own);
    REG_WRITE(CLR_REG(g), own); /* clear raw */
    REG_WRITE(ALARMLO_REG(g), 0);
    /* divider=80, increase, counter enabled, alarm armed only on request */
    REG_WRITE(CFG_REG(g), CFG_DIV(80) | CFG_INCREASE | CFG_EN |
                           (arm_alarm ? CFG_ALARM_EN : 0));
}

static void timer_arm_fire_now(int g)
{
    REG_WRITE(ALARMLO_REG(g), 0);      /* alarm <= counter: fires at once */
    REG_WRITE(CFG_REG(g),
              REG_READ(CFG_REG(g)) | CFG_ALARM_EN);
}

static void timer_disarm(int g)
{
    REG_WRITE(CFG_REG(g), REG_READ(CFG_REG(g)) & ~CFG_ALARM_EN);
    REG_WRITE(CLR_REG(g), (g == TIMG0_BASE) ? CLR_BIT_A : CLR_BIT_B);
}

static void IRAM_ATTR isr_a(void *arg)
{
    core_state_t *st = (core_state_t *)arg;

    st->a.isr_core = (int)esp_cpu_get_core_id();
    if (st->arm_b_in_isr) {
        /* Raise source B before clearing A: with a wired-OR matrix the CPU
         * line stays asserted and ISR B must still run after A clears. */
        timer_arm_fire_now(TIMG1_BASE);
    }
    st->a.count++;
    /* Clear only this source's own status bit. */
    REG_WRITE(CLR_REG(TIMG0_BASE), CLR_BIT_A);
}

static void IRAM_ATTR isr_b(void *arg)
{
    core_state_t *st = (core_state_t *)arg;

    st->b.isr_core = (int)esp_cpu_get_core_id();
    st->b.count++;
    /* Clear only this source's own status bit. */
    REG_WRITE(CLR_REG(TIMG1_BASE), CLR_BIT_B);
}

static void wait_counts(core_state_t *st, uint32_t want_a, uint32_t want_b)
{
    uint32_t waited = 0;

    while ((__atomic_load_n(&st->a.count, __ATOMIC_RELAXED) < want_a ||
            __atomic_load_n(&st->b.count, __ATOMIC_RELAXED) < want_b) &&
           waited < INTMATRIX_WAIT_TICKS) {
        vTaskDelay(1);
        waited++;
    }
}

static bool run_core_pass(int core)
{
    core_state_t *st = &s_core_state[core];
    bool pass = true;

    memset(st, 0, sizeof(*st));
    st->a.isr_core = -1;
    st->b.isr_core = -1;
    st->alloc_core = core;

    printf("INTMATRIX boot=%u core=%d allocating shared sources src_a=%d src_b=%d\n",
           (unsigned)s_boot_count, core, (int)SRC_A, (int)SRC_B);

    timer_setup(TIMG0_BASE, false);
    timer_setup(TIMG1_BASE, false);

    /* Two normally-shared allocations through the normal IDF API; both use
     * the same flags and status registers so they must land on ONE shared
     * CPU interrupt number.  Registration order defines the shared-chain
     * order (B first, then A): in the cross pattern ISR A arms B only
     * after the chain has already passed B's slot, so servicing B requires
     * a second dispatch driven purely by the wired-OR line level. */
    ESP_ERROR_CHECK(esp_intr_alloc_intrstatus((int)SRC_B, ALLOC_FLAGS,
                                              (uint32_t)ST_REG(TIMG1_BASE), ST_BIT_B,
                                              isr_b, st, &st->b.handle));
    ESP_ERROR_CHECK(esp_intr_alloc_intrstatus((int)SRC_A, ALLOC_FLAGS,
                                              (uint32_t)ST_REG(TIMG0_BASE), ST_BIT_A,
                                              isr_a, st, &st->a.handle));
    st->intno_a = esp_intr_get_intno(st->a.handle);
    st->intno_b = esp_intr_get_intno(st->b.handle);
    printf("INTMATRIX boot=%u core=%d intno_a=%d intno_b=%d shared=%s\n",
           (unsigned)(unsigned)s_boot_count, core, st->intno_a, st->intno_b,
           st->intno_a == st->intno_b ? "yes" : "NO");
    if (st->intno_a != st->intno_b) {
        printf("INTMATRIX boot=%u core=%d FAIL shared-allocation intno_a=%d intno_b=%d\n",
               (unsigned)(unsigned)s_boot_count, core, st->intno_a, st->intno_b);
        return false;
    }

    /* Pattern "basic": both alarms fire together, both ISRs must run and
     * each clears only its own status bit. */
    timer_arm_fire_now(TIMG0_BASE);
    timer_arm_fire_now(TIMG1_BASE);
    wait_counts(st, 1, 1);
    printf("INTMATRIX boot=%u core=%d pattern=basic count_a=%u count_b=%u isr_core_a=%d isr_core_b=%d\n",
           (unsigned)s_boot_count, core,
           (unsigned)st->a.count, (unsigned)st->b.count,
           st->a.isr_core, st->b.isr_core);
    if (st->a.count < 1 || st->b.count < 1 ||
        st->a.isr_core != core || st->b.isr_core != core) {
        printf("INTMATRIX boot=%u core=%d FAIL basic count_a=%u count_b=%u isr_core_a=%d isr_core_b=%d want_core=%d\n",
               (unsigned)s_boot_count, core, (unsigned)st->a.count,
               (unsigned)st->b.count, st->a.isr_core, st->b.isr_core, core);
        pass = false;
    }

    if (pass) {
        /* Pattern "cross": ISR A arms B to fire immediately, then clears A
         * while B's level is asserted.  The line must stay asserted so ISR
         * B runs; a direct per-source line model starves B here. */
        st->a.count = 0;
        st->b.count = 0;
        st->a.isr_core = -1;
        st->b.isr_core = -1;
        st->arm_b_in_isr = 1;
        timer_arm_fire_now(TIMG0_BASE); /* A fires; its ISR raises B */
        wait_counts(st, 1, 1);
        printf("INTMATRIX boot=%u core=%d pattern=cross count_a=%u count_b=%u\n",
               (unsigned)s_boot_count, core,
               (unsigned)st->a.count, (unsigned)st->b.count);
        if (st->a.count < 1 || st->b.count < 1) {
            printf("INTMATRIX boot=%u core=%d FAIL cross count_a=%u count_b=%u (b starved after a cleared)\n",
                   (unsigned)s_boot_count, core, (unsigned)st->a.count,
                   (unsigned)st->b.count);
            pass = false;
        }
        st->arm_b_in_isr = 0;
    }

    esp_intr_free(st->a.handle);
    esp_intr_free(st->b.handle);
    timer_disarm(TIMG0_BASE);
    timer_disarm(TIMG1_BASE);

    if (pass) {
        printf("INTMATRIX boot=%u core=%d PASS intno=%d\n",
               (unsigned)s_boot_count, core, st->intno_a);
    }
    return pass;
}

static void core_pass_task(void *arg)
{
    int core = (int)(intptr_t)arg;
    bool pass = run_core_pass(core);

    s_done_flags[core] = pass ? 1 : -1;
    vTaskDelete(NULL);
}

static bool run_core_sequential(int core)
{
    s_done_flags[core] = 0;

    if (xTaskCreatePinnedToCore(core_pass_task, "intmatrix", 6144,
                                (void *)(intptr_t)core,
                                tskIDLE_PRIORITY + 5, NULL, core) != pdPASS) {
        printf("INTMATRIX boot=%u FAIL task-create core=%d\n",
               (unsigned)s_boot_count, core);
        return false;
    }
    int waited = 0;
    while (s_done_flags[core] == 0 && waited < INTMATRIX_WAIT_TICKS) {
        vTaskDelay(1);
        waited++;
    }
    return s_done_flags[core] == 1;
}

void app_main(void)
{
    bool pass = true;

    if (s_magic != INTMATRIX_MAGIC) {
        /* Cold boot (or RTC state lost): reset the reboot protocol */
        s_magic = INTMATRIX_MAGIC;
        s_boot_count = 0;
        s_failed = 0;
    }
    s_boot_count++;

    printf("INTMATRIX boot=%u start round=%u of %d\n",
           (unsigned)s_boot_count, (unsigned)s_boot_count, INTMATRIX_ROUNDS);

    if (s_boot_count > INTMATRIX_ROUNDS) {
        printf("INTMATRIX OVERALL FAIL boot=%u exceeded rounds=%d\n",
               (unsigned)s_boot_count, INTMATRIX_ROUNDS);
        return;
    }
    if (s_failed) {
        printf("INTMATRIX OVERALL FAIL boot=%u failed_boot=%u\n",
               (unsigned)s_boot_count, (unsigned)s_failed);
        return;
    }

    /* CPU0 pass, then CPU1 pass — sequential so the shared-source clearing
     * inside one pass is never a cross-core race. */
    if (!run_core_sequential(0)) {
        pass = false;
    }
    if (pass && !run_core_sequential(1)) {
        pass = false;
    }

    if (!pass) {
        s_failed = s_boot_count;
        printf("INTMATRIX boot=%u RESULT FAIL\n", (unsigned)s_boot_count);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    printf("INTMATRIX boot=%u RESULT PASS\n", (unsigned)s_boot_count);
    if (s_boot_count >= INTMATRIX_ROUNDS) {
        printf("INTMATRIX OVERALL PASS boots=%u\n", (unsigned)s_boot_count);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}
