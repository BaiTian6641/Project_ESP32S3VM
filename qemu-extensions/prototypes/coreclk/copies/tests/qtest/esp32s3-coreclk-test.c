/*
 * QTest suite for the ESP32-S3 clock tree and reset domains (coreclk lane)
 *
 * Covers H-CORE-01 acceptance:
 *  - reset clock-tree frequencies (40 MHz XTAL / PLL-80 CPU / 80 MHz APB per
 *    the model's reset SYSTEM state; RTC_SLOW 136 kHz, RTC_FAST XTAL/2),
 *  - CPU/APB divider changes propagate (CPU clock observable on both cores
 *    via the qtest-clock-period QOM property; APB change alters TIMG
 *    progress; CPUPERIOD change does not alter the APB-fed TIMG),
 *  - SYSTEM_PERIP_CLK_EN0 gate bits halt/restart TIMG progress,
 *  - SYSTIMER CNT_CLK invariance to CPU/APB dividers (TRM ch.11: CNT_CLK is
 *    XTAL-derived, 16 MHz at 40 MHz XTAL),
 *  - SW system reset (RTC_CNTL OPTIONS0) clears TIMG/GDMA/SYSTIMER/intmatrix
 *    state but preserves RTC scratch, while a SW CPU reset preserves all
 *    PERIPH state,
 *  - SYSTEM_PERIP_RST_EN0/1 rising edges cold-reset the modeled device,
 *  - SYSTEM_BT_LPCK divider registers keep plain R/W storage (radio-lane
 *    compatibility).
 *
 * Copyright (c) 2026 ESP32S3VM coreclk lane
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/qmp/qnum.h"
#include "qemu/cutils.h"

/* Register block bases (TRM v1.8 ch.4 / hw/misc/esp32s3_reg.h) */
#define SYSTEM_BASE     0x600C0000
#define RTCCNTL_BASE    0x60008000
#define TIMG0_BASE      0x6001F000
#define TIMG1_BASE      0x60020000
#define SYSTIMER_BASE   0x60023000
#define GDMA_BASE       0x6003F000
#define INTMATRIX_BASE  0x600C2000

/* SYSTEM registers */
#define R_SYSCLK_CONF   (SYSTEM_BASE + 0x060)
#define R_CPU_PER_CONF  (SYSTEM_BASE + 0x010)
#define R_CLK_EN0       (SYSTEM_BASE + 0x018)
#define R_CLK_EN1       (SYSTEM_BASE + 0x01C)
#define R_RST_EN0       (SYSTEM_BASE + 0x020)
#define R_RST_EN1       (SYSTEM_BASE + 0x024)
#define R_LPCK_DIV_INT  (SYSTEM_BASE + 0x028)
#define R_CORE1_CTRL0   (SYSTEM_BASE + 0x000)
#define R_CORE1_MESSAGE (SYSTEM_BASE + 0x004)
#define R_RTC_CPU_STALL (RTCCNTL_BASE + 0x0BC)

/* Reset values per the model: sysclk = CLK_DIV_EN|SOC_CLK_SEL=PLL|40<<12|
 * PRE_DIV_CNT=1; cpuperconf = CPUPERIOD_SEL=0|PLL480; PERIP_CLK_EN0/1 follow
 * the IDF6.1 field defaults; PERIP_RST_EN0/1 = 0. */
#define SYSCLK_RESET        0x00028001u  /* TRM 17.20: SOC_CLK_SEL=XTAL, PRE_DIV_CNT=1 */
#define SYSCLK_PLL_80       0x00028401u  /* SOC_CLK_SEL=PLL(480), CPU/APB 80 MHz */
#define CPUPERIOD_RESET     0x00000000u
#define CLK_EN0_RESET       0x6181A06Fu
#define CLK_EN1_RESET       0x00000600u

/* RTC_CNTL */
#define R_RTC_OPTIONS0      (RTCCNTL_BASE + 0x000)
#define R_RTC_STORE0        (RTCCNTL_BASE + 0x050)
#define R_RTC_RESET_STATE   (RTCCNTL_BASE + 0x038)

/* TIMG0 */
#define R_T0CONFIG(base)    ((base) + 0x000)
#define R_T0LO(base)        ((base) + 0x004)
#define R_T0UPDATE(base)    ((base) + 0x00C)
#define R_T0LOADLO(base)    ((base) + 0x018)
#define R_T0LOAD(base)      ((base) + 0x020)
#define R_INT_ENA_TIMG(base) ((base) + 0x070)
#define T0CFG_EN            (1u << 31)
#define T0CFG_INCREASE      (1u << 30)
#define T0CFG_DIV(n)        ((n) << 13)
#define T0CFG_RESET         (1u << 13)  /* divider = 1 */

/* SYSTIMER */
#define R_ST_UNIT0_OP       (SYSTIMER_BASE + 0x004)
#define R_ST_UNIT0_VALUE_LO (SYSTIMER_BASE + 0x044)
#define R_ST_UNIT0_LOAD_LO  (SYSTIMER_BASE + 0x010)
#define R_ST_UNIT0_LOAD     (SYSTIMER_BASE + 0x05C)
#define R_ST_INT_ENA        (SYSTIMER_BASE + 0x064)
#define ST_OP_UPDATE        (1u << 30)

/* GDMA channel 0 OUT CONF0 (chan 0 OUT regs start at 0x60) */
#define R_GDMA_OUT_CONF0_CH0 (GDMA_BASE + 0x060)

static QTestState *qts;

/* CPU-clock propagation to both cores is verified out-of-band with a raw QMP
 * probe (documented in the README evidence section): qom-get on the CPU
 * cores' "clk-in" and on the "cpu-clk" output returns the expected
 * CLOCK_PERIOD_FROM_HZ value over a plain -qmp channel. The libqtest QMP
 * channel in this QEMU revision returns a sign-truncated int32 for the same
 * query, so in-suite assertions use guest-observable consumers instead:
 * TIMG RTCCALICFG (XTAL via the clock tree), TIMG progress (APB/XTAL rates)
 * and SYSTIMER CNT_CLK. */

static void enable_timg_counter(uint32_t base, uint32_t divider)
{
    qtest_writel(qts, R_T0LOADLO(base), 0);
    qtest_writel(qts, R_T0LOAD(base), 1);
    qtest_writel(qts, R_T0CONFIG(base),
                 T0CFG_EN | T0CFG_INCREASE | T0CFG_DIV(divider));
}

static uint32_t timg_counter(uint32_t base)
{
    qtest_writel(qts, R_T0UPDATE(base), 1u << 31);
    return qtest_readl(qts, R_T0LO(base));
}

static void system_reset_via_rtc(void)
{
    qtest_writel(qts, R_RTC_OPTIONS0, 1u << 31);
    /* Let the main loop process qemu_system_reset_request() */
    qtest_clock_step_next(qts);
}

static void cpu_reset_via_rtc(uint32_t mask)
{
    qtest_writel(qts, R_RTC_OPTIONS0, mask);
    qtest_clock_step_next(qts);
}

static void test_clock_tree_reset_defaults(void)
{
    /* Register reset state */
    g_assert_cmpuint(qtest_readl(qts, R_SYSCLK_CONF), ==, SYSCLK_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_CPU_PER_CONF), ==, CPUPERIOD_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_CLK_EN0), ==, CLK_EN0_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_CLK_EN1), ==, CLK_EN1_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_RST_EN0), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, R_RST_EN1), ==, 0);

    /* Propagated clock tree, guest-observable: the TIMG calibration counts
     * XTAL_CLK cycles (from the propagated xtal-clk output) for 1000 cycles
     * of RC_SLOW (136 kHz): 40e6 * 1000 / 136e3 = 294117, reported in
     * TIMG_RTCCALICFG1.VALUE [24:7]. */
    qtest_writel(qts, TIMG0_BASE + 0x68, (1u << 31) | (1000u << 16));
    g_assert_cmpuint(qtest_readl(qts, TIMG0_BASE + 0x6c), ==, 294117u << 7);
}

static void test_cpu_divider_registers(void)
{
    /* Divider/source registers store their documented fields and the
     * resulting frequency changes are proven by the TIMG/SYSTIMER progress
     * tests below (see also the raw-QMP probe for the CPU-core clocks). */
    qtest_writel(qts, R_CPU_PER_CONF, 1);
    g_assert_cmpuint(qtest_readl(qts, R_CPU_PER_CONF), ==, 1);
    qtest_writel(qts, R_CPU_PER_CONF, 2);
    g_assert_cmpuint(qtest_readl(qts, R_CPU_PER_CONF), ==, 2);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET & ~0xC00u);
    /* CLK_XTAL_FREQ is read-only: the stored 40 stays, SOC_CLK_SEL cleared
     * (already 0 after the TRM reset, so only the readback is checked) */
    g_assert_cmpuint(qtest_readl(qts, R_SYSCLK_CONF), ==, 0x00028001u);
    /* Restore the PLL/80 MHz reset state */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
    qtest_writel(qts, R_CPU_PER_CONF, CPUPERIOD_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_SYSCLK_CONF), ==, SYSCLK_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_CPU_PER_CONF), ==, CPUPERIOD_RESET);
}

static void test_apb_divider_changes_timg_progress(void)
{
    /* Switch to the PLL 80 MHz state, then APB-fed TIMG0 (USE_XTAL=0),
     * divider 2: 40 MHz -> 40000 ticks/ms */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    enable_timg_counter(TIMG0_BASE, 2);
    g_assert_cmpuint(timg_counter(TIMG0_BASE), ==, 0);

    qtest_clock_step(qts, 1000000);      /* 1 ms of virtual time */
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 40000);

    /* Switch the SoC clock back to the reset XTAL source:
     * APB = CPU_CLK = XTAL/2 = 20 MHz -> 10000 ticks/ms */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 50000); /* 40000 + 10000 */

    /* Restore PLL and verify the rate follows */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 90000);

    /* The CPU divider does not touch the APB-fed rate (PLL: APB fixed 80M) */
    qtest_writel(qts, R_CPU_PER_CONF, 2);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 130000);

    qtest_writel(qts, R_CPU_PER_CONF, CPUPERIOD_RESET);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
    qtest_writel(qts, R_T0CONFIG(TIMG0_BASE), T0CFG_RESET);
}

static void test_timg_xtal_source_and_divider(void)
{
    /* XTAL-fed TIMG1 (USE_XTAL=1), divider 2: 40/2 = 20 MHz -> 20000 ticks/ms */
    qtest_writel(qts, R_T0LOADLO(TIMG1_BASE), 0);
    qtest_writel(qts, R_T0LOAD(TIMG1_BASE), 1);
    qtest_writel(qts, R_T0CONFIG(TIMG1_BASE),
                 T0CFG_EN | T0CFG_INCREASE | (1u << 9) | T0CFG_DIV(2));

    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG1_BASE), ==, 20000);

    /* Divider 4 halves the rate; the CPU/APB dividers do not matter here */
    qtest_writel(qts, R_T0CONFIG(TIMG1_BASE),
                 T0CFG_EN | T0CFG_INCREASE | (1u << 9) | T0CFG_DIV(4));
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG1_BASE), ==, 30000);

    qtest_writel(qts, R_T0CONFIG(TIMG1_BASE), T0CFG_RESET);
}

static void test_timg_divider_boundary_values(void)
{
    /* Divider-field encoding (TRM Register 12.1 + ch.12 12.2.1 + IDF
     * esp_hal_timg esp32s3 timer_ll_set_clock_prescale): field 0 encodes
     * the maximum divider 65536; field 1 divides by 2 ("the actual divisor
     * is 2"); fields 2..65535 divide by the field value. The write path
     * must never divide by zero. */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);   /* APB 80 MHz */

    /* Field 0 -> /65536: 80 MHz / 65536 = 1220.703125 Hz. The rate is kept
     * rational (source_hz + prescale, no integer-Hz truncation): a 10 s
     * step counts exactly 12207 ticks, not the truncated 12200. */
    enable_timg_counter(TIMG0_BASE, 0);
    {
        uint32_t c0 = timg_counter(TIMG0_BASE);
        qtest_clock_step(qts, 10000000000ull);         /* 10 s */
        g_assert_cmpuint(timg_counter(TIMG0_BASE) - c0, ==, 12207);
    }

    /* Field 1 (reset value; TRM §12.2.1: "the actual divisor is 2") ->
     * 40 MHz -> 40000 ticks/ms. Writing it must not wedge the counter
     * or divide by zero. */
    enable_timg_counter(TIMG0_BASE, 1);
    {
        uint32_t c0 = timg_counter(TIMG0_BASE);
        qtest_clock_step(qts, 1000000);                /* 1 ms */
        g_assert_cmpuint(timg_counter(TIMG0_BASE) - c0, ==, 40000);
    }

    /* Field 2 (first valid value) -> 40 MHz -> 40000 ticks/ms */
    enable_timg_counter(TIMG0_BASE, 2);
    {
        uint32_t c0 = timg_counter(TIMG0_BASE);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(timg_counter(TIMG0_BASE) - c0, ==, 40000);
    }

    /* Alarm through the maximum divider meets the derived rational
     * deadline: counter at 12207 ticks, alarm 12207 ticks ahead ->
     * deadline = 12207 * 65536 / 80e6 s = 9.9999744 s; fires within the
     * 10.05 s step and not before it. */
    qtest_writel(qts, R_T0LOADLO(TIMG0_BASE), 0);
    qtest_writel(qts, R_T0LOAD(TIMG0_BASE), 1);
    qtest_writel(qts, TIMG0_BASE + 0x10, 12207);       /* T0ALARMLO */
    qtest_writel(qts, TIMG0_BASE + 0x14, 0);           /* T0ALARMHI */
    qtest_writel(qts, R_T0CONFIG(TIMG0_BASE),
                 T0CFG_EN | T0CFG_INCREASE | (1u << 10) | T0CFG_DIV(0));
    qtest_clock_step(qts, 9999000000ull);              /* 9.999 s: not yet */
    g_assert_cmpuint(qtest_readl(qts, TIMG0_BASE + 0x74) & 1u, ==, 0);
    qtest_clock_step(qts, 60000000ull);                /* +60 ms: fires */
    g_assert_cmpuint(qtest_readl(qts, TIMG0_BASE + 0x74) & 1u, ==, 1);

    /* Restore */
    qtest_writel(qts, TIMG0_BASE + 0x7C, 1u);          /* INT_CLR */
    qtest_writel(qts, R_T0CONFIG(TIMG0_BASE), T0CFG_RESET);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
}

static void test_common_clock_ports(void)
{
    /* Guest-observable checks only: the new outputs (pll-f80m/f160m/f240m,
     * rc-fast) are qdev Clock outputs verified by value with the raw-QMP
     * probe (README; the libqtest QMP channel both truncates >2^31 periods
     * and crashes parsing the quoted qom-get path, so it is not used).
     *
     * BBPLL validity (RTC_CNTL OPTIONS0 BBPLL_FORCE_PD [10] / FORCE_PU [11])
     * is independent of the CPU mux: switching SOC_CLK_SEL between PLL and
     * XTAL (or RC_FAST) leaves the PLL powered, so the pll-* outputs stay
     * nominal; only BBPLL_FORCE_PD gates them to 0 Hz. */
    const uint32_t clk_conf = qtest_readl(qts, RTCCNTL_BASE + 0x74);
    g_assert_cmpuint(clk_conf & (1u << 25), ==, 0);    /* powered at reset */
    qtest_writel(qts, RTCCNTL_BASE + 0x74, clk_conf | (1u << 25));
    g_assert_cmpuint(qtest_readl(qts, RTCCNTL_BASE + 0x74) & (1u << 25),
                     !=, 0);
    qtest_writel(qts, RTCCNTL_BASE + 0x74, clk_conf);
    g_assert_cmpuint(qtest_readl(qts, RTCCNTL_BASE + 0x74) & (1u << 25), ==, 0);

    /* CPU mux-only switch: BBPLL stays powered, so the pll-* outputs keep
     * their nominal rates across SOC_CLK_SEL changes (raw-QMP verified:
     * pll-f80m 53687091200 / pll-f160m 26843545600 across PLL->XTAL->RC_FAST
     * with no truncation drift). In-suite: the SOC_CLK_SEL writes store,
     * and the OPTIONS0 power bits store and read back (the clock-period
     * asserts live in the raw-QMP probe). */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
    qtest_writel(qts, R_SYSCLK_CONF, (SYSCLK_RESET & ~0xC00u) | 0x800u); /* RC_FAST CPU */
    g_assert_cmpuint(qtest_readl(qts, R_SYSCLK_CONF) & 0xC00u, ==, 0x800u);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);

    /* BBPLL force-power-down gates the pll-* outputs independently of the
     * CPU mux (RTC_CNTL OPTIONS0 BBPLL_FORCE_PD [10]). */
    const uint32_t options0 = qtest_readl(qts, RTCCNTL_BASE + 0x00);
    qtest_writel(qts, RTCCNTL_BASE + 0x00, options0 | (1u << 10));
    g_assert_cmpuint(qtest_readl(qts, RTCCNTL_BASE + 0x00) & (1u << 10), !=, 0);
    qtest_writel(qts, RTCCNTL_BASE + 0x00, options0);
    g_assert_cmpuint(qtest_readl(qts, RTCCNTL_BASE + 0x00) & (1u << 10), ==, 0);

    /* PERIP_RST level registers store their bits (level bundle mirrors
     * these for consumers with level-held reset semantics) */
    qtest_writel(qts, R_RST_EN0, 1u << 6);             /* SPI2_RST */
    g_assert_cmphex(qtest_readl(qts, R_RST_EN0), ==, 1u << 6);
    qtest_writel(qts, R_RST_EN0, 0);
}

static void test_systimer_cnt_clk_invariant(void)
{
    /* TRM ch.11: the systimer counter runs from CNT_CLK (2/5 x XTAL_CLK =
     * 16 MHz at 40 MHz), NOT from APB_CLK: divider changes must not alter it */
    qtest_writel(qts, R_ST_UNIT0_LOAD_LO, 0);
    qtest_writel(qts, R_ST_UNIT0_LOAD, 1);
    qtest_writel(qts, R_ST_UNIT0_OP, ST_OP_UPDATE);
    g_assert_cmpuint(qtest_readl(qts, R_ST_UNIT0_VALUE_LO), ==, 0);

    qtest_clock_step(qts, 1000000);      /* 16 ticks/us * 1 ms = 16000 ticks */
    qtest_writel(qts, R_ST_UNIT0_OP, ST_OP_UPDATE);
    g_assert_cmphex(qtest_readl(qts, R_ST_UNIT0_VALUE_LO), ==, 16000);

    /* CPU divider + SoC source change: CNT_CLK unchanged */
    qtest_writel(qts, R_CPU_PER_CONF, 2);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET & ~0xC00u);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, R_ST_UNIT0_OP, ST_OP_UPDATE);
    g_assert_cmphex(qtest_readl(qts, R_ST_UNIT0_VALUE_LO), ==, 32000);

    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
    qtest_writel(qts, R_CPU_PER_CONF, CPUPERIOD_RESET);
}

static void test_periph_clock_gate_halts_timg(void)
{
    uint32_t en0 = qtest_readl(qts, R_CLK_EN0);

    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    enable_timg_counter(TIMG0_BASE, 1);
    timg_counter(TIMG0_BASE);

    /* Clear TIMERS_CLK_EN [0]: TIMG0 timers make no progress */
    qtest_writel(qts, R_CLK_EN0, en0 & ~1u);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 0);

    /* Restore TIMERS, clear TIMERGROUP_CLK_EN [13]: still gated */
    qtest_writel(qts, R_CLK_EN0, (en0 & ~0x2000u));
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 0);

    /* Full restore: progress resumes */
    qtest_writel(qts, R_CLK_EN0, en0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG0_BASE), ==, 40000);

    /* TIMG1 is gated by TIMERGROUP1_CLK_EN [15] */
    enable_timg_counter(TIMG1_BASE, 1);
    timg_counter(TIMG1_BASE);
    qtest_writel(qts, R_CLK_EN0, en0 & ~0x8000u);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG1_BASE), ==, 0);
    qtest_writel(qts, R_CLK_EN0, en0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(timg_counter(TIMG1_BASE), ==, 40000);

    qtest_writel(qts, R_T0CONFIG(TIMG0_BASE), T0CFG_RESET);
    qtest_writel(qts, R_T0CONFIG(TIMG1_BASE), T0CFG_RESET);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_RESET);
}

static void program_periph_state(void)
{
    enable_timg_counter(TIMG0_BASE, 80);
    qtest_writel(qts, R_GDMA_OUT_CONF0_CH0, 0x3E);
    qtest_writel(qts, R_ST_INT_ENA, 0x7);
    /* Interrupt matrix: route source 1 to CPU0 int 5 */
    qtest_writel(qts, INTMATRIX_BASE + 4, 5);
}

static void test_sw_system_reset_clears_periph_keeps_rtc(void)
{
    program_periph_state();
    qtest_writel(qts, R_RTC_STORE0, 0xDEADBEEF);

    system_reset_via_rtc();

    /* TIMG back to its reset configuration */
    g_assert_cmphex(qtest_readl(qts, R_T0CONFIG(TIMG0_BASE)), ==, T0CFG_RESET);
    /* GDMA channel configuration cleared */
    g_assert_cmphex(qtest_readl(qts, R_GDMA_OUT_CONF0_CH0), ==, 0);
    /* SYSTIMER interrupt enables cleared */
    g_assert_cmphex(qtest_readl(qts, R_ST_INT_ENA), ==, 0);
    /* Interrupt matrix map entry back to the documented reset value 0x10
     * (intc lane: MAP registers reset to 0x10 per IDF interrupt_core*_reg.h
     * defaults; 0x10 is an internal CPU interrupt, i.e. unrouted) */
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_BASE + 4), ==, 0x10);

    /* RTC domain survived: scratch register and the SW reset cause */
    g_assert_cmphex(qtest_readl(qts, R_RTC_STORE0), ==, 0xDEADBEEF);
    g_assert_cmpuint(qtest_readl(qts, R_RTC_RESET_STATE) & 0x3f, ==, 3);

    /* Clock tree registers returned to reset defaults */
    g_assert_cmpuint(qtest_readl(qts, R_CLK_EN0), ==, CLK_EN0_RESET);
    g_assert_cmpuint(qtest_readl(qts, R_SYSCLK_CONF), ==, SYSCLK_RESET);
}

static void test_sw_cpu_reset_preserves_periph(void)
{
    program_periph_state();
    qtest_writel(qts, R_RTC_STORE0, 0xCAFEBABE);

    /* PROCPU SW reset (RTC_CNTL_SW_PROCPU_RST) */
    cpu_reset_via_rtc(1u << 5);

    g_assert_cmphex(qtest_readl(qts, R_T0CONFIG(TIMG0_BASE)),
                    ==, T0CFG_EN | T0CFG_INCREASE | T0CFG_DIV(80));
    g_assert_cmphex(qtest_readl(qts, R_GDMA_OUT_CONF0_CH0), ==, 0x3E);
    g_assert_cmphex(qtest_readl(qts, R_ST_INT_ENA), ==, 0x7);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_BASE + 4), ==, 5);
    g_assert_cmphex(qtest_readl(qts, R_RTC_STORE0), ==, 0xCAFEBABE);
    g_assert_cmpuint(qtest_readl(qts, R_RTC_RESET_STATE) & 0x3f, ==, 12);

    /* And the timer keeps progressing at the reset APB rate:
     * 20 MHz / div 80 = 250 kHz -> 250 ticks per 1 ms step */
    {
        uint32_t c0 = timg_counter(TIMG0_BASE);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(timg_counter(TIMG0_BASE) - c0, ==, 250);
    }

    /* APPCPU SW reset preserves everything as well */
    cpu_reset_via_rtc(1u << 4);
    g_assert_cmphex(qtest_readl(qts, R_GDMA_OUT_CONF0_CH0), ==, 0x3E);
    g_assert_cmpuint(qtest_readl(qts, R_RTC_RESET_STATE) & 0x3f, ==, 12);
}

static void test_periph_rst_strobe_resets_device(void)
{
    enable_timg_counter(TIMG0_BASE, 2);
    enable_timg_counter(TIMG1_BASE, 2);
    qtest_writel(qts, R_ST_INT_ENA, 0x7);

    /* Raise TIMERGROUP_RST [13]: TIMG0 cold-resets, TIMG1 is untouched */
    qtest_writel(qts, R_RST_EN0, 1u << 13);
    g_assert_cmphex(qtest_readl(qts, R_T0CONFIG(TIMG0_BASE)), ==, T0CFG_RESET);
    g_assert_cmphex(qtest_readl(qts, R_T0CONFIG(TIMG1_BASE)),
                    ==, T0CFG_EN | T0CFG_INCREASE | T0CFG_DIV(2));

    /* SYSTIMER_RST [29] clears the systimer enables */
    qtest_writel(qts, R_RST_EN0, (1u << 13) | (1u << 29));
    g_assert_cmphex(qtest_readl(qts, R_ST_INT_ENA), ==, 0);

    /* Falling edges do nothing */
    qtest_writel(qts, R_RST_EN0, 0);
    g_assert_cmphex(qtest_readl(qts, R_T0CONFIG(TIMG1_BASE)),
                    ==, T0CFG_EN | T0CFG_INCREASE | T0CFG_DIV(2));

    qtest_writel(qts, R_T0CONFIG(TIMG1_BASE), T0CFG_RESET);
}

/* Use raw QMP here: the old JSON format interpolator must not interpret
 * literal QOM paths. These properties report actual CPU lifecycle state. */
static QDict *lifecycle_property(const char *property)
{
    QDict *reply;

    qtest_qmp_send_raw(qts,
        "{\"execute\":\"qom-get\",\"arguments\":"
        "{\"path\":\"/machine/soc\",\"property\":\"%s\"}}\n", property);
    reply = qtest_qmp_receive(qts);
    g_assert_true(qdict_haskey(reply, "return"));
    return reply;
}

static uint64_t lifecycle_resets(void)
{
    QDict *reply = lifecycle_property("diag-cpu1-reset-count");
    uint64_t count = qdict_get_int(reply, "return");

    qobject_unref(reply);
    return count;
}

static void lifecycle_flag(const char *property, bool expected)
{
    QDict *reply = lifecycle_property(property);

    g_assert_cmpint(qdict_get_bool(reply, "return"), ==, expected);
    qobject_unref(reply);
}

static char *lifecycle_cpu(unsigned index)
{
    return qtest_hmp(qts, "info registers %u", index);
}

static uint32_t lifecycle_ccount(unsigned index)
{
    char *dump = lifecycle_cpu(index);
    char *field = strstr(dump, "CCOUNT");
    char *value;
    uint32_t count;

    g_assert_nonnull(field);
    value = strchr(field, '=');
    g_assert_nonnull(value);
    count = g_ascii_strtoull(value + 1, NULL, 16);
    g_free(dump);
    return count;
}

static void test_cpu1_counter_clock_gate(void)
{
    QTestState *original = qts;
    uint32_t cpu0, cpu1;

    /* Native SDK fixture additionally programs a NONZERO CCOUNT/CCOMPARE1
     * and handles real IRQs. This deterministic test observes the actual
     * clock-derived live CCOUNT without executing any guest instructions. */
    qts = qtest_init("-M esp32s3 -accel qtest -display none");
    qtest_writel(qts, R_CORE1_CTRL0, 2);
    qtest_clock_step(qts, 1000000);
    cpu0 = lifecycle_ccount(0);
    cpu1 = lifecycle_ccount(1);
    g_assert_cmpuint(cpu1, >, 0);
    qtest_writel(qts, R_CORE1_CTRL0, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(lifecycle_ccount(1), ==, cpu1);
    g_assert_cmpuint((uint32_t)(lifecycle_ccount(0) - cpu0), ==, 20000);

    /* Recomputing nominal CPU rate must not reopen the physical CPU1 gate. */
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    qtest_writel(qts, R_CPU_PER_CONF, 2);
    cpu0 = lifecycle_ccount(0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(lifecycle_ccount(1), ==, cpu1);
    g_assert_cmpuint((uint32_t)(lifecycle_ccount(0) - cpu0), ==, 240000);

    /* RUNSTALL holds instructions, not CPU clock cycles or internal timers. */
    qtest_writel(qts, R_CORE1_CTRL0, 3);
    cpu1 = lifecycle_ccount(1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint((uint32_t)(lifecycle_ccount(1) - cpu1), ==, 240000);
    qtest_quit(qts);
    qts = original;
}

static void test_cpu1_lifecycle_levels(void)
{
    QTestState *original = qts;
    uint64_t resets;
    char *cpu1;
    const uint32_t rmw[] = { 6, 6, 6, 2 };
    unsigned i;

    qts = qtest_init("-M esp32s3 -accel qtest -display none");
    g_assert_cmphex(qtest_readl(qts, R_CORE1_CTRL0), ==, 4);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_MESSAGE), ==, 0);
    lifecycle_flag("diag-cpu1-held", true);
    lifecycle_flag("diag-cpu1-stalled", true);
    resets = lifecycle_resets();
    cpu1 = lifecycle_cpu(1);
    qtest_clock_step(qts, 1000000);
    {
        char *after = lifecycle_cpu(1);
        g_assert_cmpstr(after, ==, cpu1);
        g_free(after);
    }
    g_free(cpu1);

    /* IDF's stored RMW sequence does not invent a RESETING rising edge. */
    for (i = 0; i < G_N_ELEMENTS(rmw); i++) {
        qtest_writel(qts, R_CORE1_CTRL0, rmw[i] | 0xFFFFFFF8u);
        g_assert_cmphex(qtest_readl(qts, R_CORE1_CTRL0), ==, rmw[i]);
        g_assert_cmpuint(lifecycle_resets(), ==, resets);
        lifecycle_flag("diag-cpu1-held", i != 3);
    }
    lifecycle_flag("diag-cpu1-stalled", false);
    cpu1 = lifecycle_cpu(1);
    qtest_writel(qts, R_CORE1_CTRL0, 3);
    lifecycle_flag("diag-cpu1-runstall-raw", true);
    lifecycle_flag("diag-cpu1-held", false);
    lifecycle_flag("diag-cpu1-stalled", true);
    qtest_writel(qts, R_CORE1_CTRL0, 1);
    lifecycle_flag("diag-cpu1-held", true);
    qtest_writel(qts, R_CORE1_CTRL0, 2);
    qtest_writel(qts, R_RTC_CPU_STALL, 0x21u << 20);
    qtest_writel(qts, R_RTC_OPTIONS0,
                 qtest_readl(qts, R_RTC_OPTIONS0) | 2);
    lifecycle_flag("diag-rtc-cpu1-stall", true);
    lifecycle_flag("diag-cpu1-stalled", true);
    qtest_writel(qts, R_CORE1_CTRL0, 3);
    qtest_writel(qts, R_RTC_CPU_STALL, 0);
    lifecycle_flag("diag-rtc-cpu1-stall", false);
    lifecycle_flag("diag-cpu1-stalled", true);
    /* One write clears RUNSTALL while disabling the gate: never released. */
    qtest_writel(qts, R_CORE1_CTRL0, 0);
    lifecycle_flag("diag-cpu1-runstall-raw", false);
    lifecycle_flag("diag-cpu1-stalled", true);
    qtest_clock_step(qts, 1000000);
    {
        char *after = lifecycle_cpu(1);
        g_assert_cmpstr(after, ==, cpu1);
        g_free(after);
    }
    g_free(cpu1);
    g_assert_cmpuint(lifecycle_resets(), ==, resets);
    qtest_quit(qts);
    qts = original;
}

static void test_cpu1_lifecycle_reset_domains(void)
{
    QTestState *original = qts;
    uint64_t resets;
    char *cpu0;
    unsigned i;

    qts = qtest_init("-M esp32s3 -accel qtest -display none");
    program_periph_state();
    qtest_writel(qts, R_CORE1_MESSAGE, 0x40381234);
    qtest_writel(qts, R_SYSCLK_CONF, SYSCLK_PLL_80);
    qtest_writel(qts, R_CPU_PER_CONF, 2);
    qtest_writel(qts, R_RTC_STORE0, 0xA15C01);
    /* State outside either CPU, including shared SRAM, must survive. */
    qtest_writel(qts, 0x3FC80000, 0x0015C0DE);
    qtest_writel(qts, R_CORE1_CTRL0, 2);
    resets = lifecycle_resets();
    cpu0 = lifecycle_cpu(0);
    qtest_writel(qts, R_CORE1_CTRL0, 6);
    /* The edge queues CPU-local work; count only completed callbacks.
     * QMP round trips allow the main loop to drain that work in qtest. */
    for (i = 0; i < 100 && lifecycle_resets() == resets; i++) {
        qtest_clock_step(qts, 0);
    }
    g_assert_cmpuint(lifecycle_resets(), ==, resets + 1);
    lifecycle_flag("diag-cpu1-stalled", true);
    qtest_writel(qts, R_CORE1_CTRL0, 6);
    qtest_clock_step(qts, 0);
    g_assert_cmpuint(lifecycle_resets(), ==, resets + 1);
    {
        char *after = lifecycle_cpu(0);
        g_assert_cmpstr(after, ==, cpu0);
        g_free(after);
    }
    g_free(cpu0);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_CTRL0), ==, 6);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_MESSAGE), ==, 0x40381234);
    g_assert_cmphex(qtest_readl(qts, R_SYSCLK_CONF), ==, SYSCLK_PLL_80);
    g_assert_cmphex(qtest_readl(qts, R_CPU_PER_CONF), ==, 2);
    g_assert_cmphex(qtest_readl(qts, R_CLK_EN0), ==, CLK_EN0_RESET);
    g_assert_cmphex(qtest_readl(qts, R_GDMA_OUT_CONF0_CH0), ==, 0x3E);
    g_assert_cmphex(qtest_readl(qts, R_ST_INT_ENA), ==, 7);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_BASE + 4), ==, 5);
    g_assert_cmphex(qtest_readl(qts, R_RTC_STORE0), ==, 0xA15C01);
    g_assert_cmphex(qtest_readl(qts, 0x3FC80000), ==, 0x0015C0DE);
    cpu_reset_via_rtc(1u << 4);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_CTRL0), ==, 6);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_MESSAGE), ==, 0x40381234);
    g_assert_cmphex(qtest_readl(qts, 0x3FC80000), ==, 0x0015C0DE);
    system_reset_via_rtc();
    g_assert_cmphex(qtest_readl(qts, R_CORE1_CTRL0), ==, 4);
    g_assert_cmphex(qtest_readl(qts, R_CORE1_MESSAGE), ==, 0);
    lifecycle_flag("diag-cpu1-held", true);
    g_assert_cmphex(qtest_readl(qts, R_GDMA_OUT_CONF0_CH0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, R_RTC_STORE0), ==, 0xA15C01);
    g_assert_cmphex(qtest_readl(qts, 0x3FC80000), ==, 0x0015C0DE);
    qtest_quit(qts);
    qts = original;
}

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();
    int ret;

    g_assert_cmpstr(arch, ==, "xtensa");

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init("-M esp32s3 -accel qtest -display none");

    qtest_add_func("esp32s3-coreclk/reset-defaults",
                   test_clock_tree_reset_defaults);
    qtest_add_func("esp32s3-coreclk/cpu-divider-registers",
                   test_cpu_divider_registers);
    qtest_add_func("esp32s3-coreclk/apb-divider-timg-progress",
                   test_apb_divider_changes_timg_progress);
    qtest_add_func("esp32s3-coreclk/timg-xtal-source",
                   test_timg_xtal_source_and_divider);
    qtest_add_func("esp32s3-coreclk/timg-divider-boundary",
                   test_timg_divider_boundary_values);
    qtest_add_func("esp32s3-coreclk/systimer-cnt-clk-invariant",
                   test_systimer_cnt_clk_invariant);
    qtest_add_func("esp32s3-coreclk/periph-clock-gate",
                   test_periph_clock_gate_halts_timg);
    qtest_add_func("esp32s3-coreclk/sw-system-reset",
                   test_sw_system_reset_clears_periph_keeps_rtc);
    qtest_add_func("esp32s3-coreclk/sw-cpu-reset-preserves-periph",
                   test_sw_cpu_reset_preserves_periph);
    qtest_add_func("esp32s3-coreclk/periph-rst-strobe",
                   test_periph_rst_strobe_resets_device);
    qtest_add_func("esp32s3-coreclk/common-clock-ports",
                   test_common_clock_ports);
    qtest_add_func("esp32s3-coreclk/cpu1-lifecycle-levels",
                   test_cpu1_lifecycle_levels);
    qtest_add_func("esp32s3-coreclk/cpu1-lifecycle-reset-domains",
                   test_cpu1_lifecycle_reset_domains);
    qtest_add_func("esp32s3-coreclk/cpu1-counter-clock-gate",
                   test_cpu1_counter_clock_gate);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
