/*
 * ESP32-S3 SENS / APB_SARADC register-level qtest (ADC-01 oneshot profile)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the RTC ADC controller against the TRM v1.8 chapter 39 and
 * pinned IDF v6.1.0 register semantics:
 *   - reset defaults (SENS / APB_SARADC),
 *   - fail-closed behavior with NO sample provider (no fabricated data),
 *   - divider voltage -> raw quantization at the exact acquisition instant,
 *     with a scheduled rail step before and after the aperture (provider
 *     solves an explicit 3.3 V / 10k / 10k circuit via the vendored net-dc
 *     primitive),
 *   - ground and clipped valid rails, per-channel attenuation and unit
 *     selection (ADC1/ADC2, GPIO1..10 / GPIO11..20 mapping),
 *   - busy/FSM state, start-while-busy restart, RTC-side reset, clock gate,
 *     analog power gate, ULP/dig ownership,
 *   - stale completion (DONE persists until next start), INT_RAW W1C/ENA/ST,
 *   - ADC2 arbiter deny carrying the TRM 2'b01 data flag,
 *   - floating / digital-owned / unpowered sample diagnostics (no DONE, no
 *     consumable data, diagnostic counters visible as QOM properties).
 *
 * Timing model constants (documented profile): RTC_FAST 20 MHz fallback /
 * SAR1_CLK_DIV 2 -> 10 MHz RTCADC_SARCLK (100 ns per cycle); phases XPD 8 +
 * aperture 8 + SAR 12 + RSTB 8 + STANDBY 255 = 291 cycles; acquisition at
 * start + 1600 ns; DONE at start + 29100 ns.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest-single.h"

#define SENS_BASE   0x60008800ull
#define APB_BASE    0x60040000ull

#define R_READER1_CTRL      0x00
#define R_MEAS1_CTRL2       0x0C
#define R_MEAS1_MUX         0x10
#define R_ATTEN1            0x14
#define R_READER2_CTRL      0x24
#define R_MEAS2_CTRL1       0x2C
#define R_MEAS2_CTRL2       0x30
#define R_ATTEN2            0x38
#define R_POWER_XPD_SAR     0x3C
#define R_SLAVE_ADDR1       0x40
#define R_PERI_CLK_GATE     0x104
#define R_PERI_RESET        0x108

#define A_CTRL              0x00
#define A_FSM_WAIT          0x0C
#define A_ARB_CTRL          0x38
#define A_DATA_STATUS1      0x40
#define A_INT_ENA           0x5C
#define A_INT_RAW           0x60
#define A_INT_ST            0x64
#define A_INT_CLR           0x68

#define CTRL2_DONE          BIT(16)
#define CTRL2_START         BIT(17)
#define CTRL2_START_FORCE   BIT(18)
#define CTRL2_ENPAD_S       19
#define CTRL2_ENPAD_FORCE   BIT(31)
#define MEAS_STATUS_S       22

#define CYCLE_NS            100ull          /* 20 MHz / div 2 */
#define XPD_CYCLES          8
#define APERTURE_CYCLES     (4 * 2)         /* sample_cycle 2 */
#define CONVERT_CYCLES      12
#define RSTB_CYCLES         8
#define STANDBY_CYCLES      255
#define ACQ_NS              ((XPD_CYCLES + APERTURE_CYCLES) * CYCLE_NS)
#define DONE_NS             ((XPD_CYCLES + APERTURE_CYCLES + CONVERT_CYCLES + \
                              RSTB_CYCLES + STANDBY_CYCLES) * CYCLE_NS)

#define SENS_PATH           "/machine/soc/sens"

static QTestState *s;

static uint32_t sens_r(uint32_t off)
{
    return qtest_readl(s, SENS_BASE + off);
}

static void sens_w(uint32_t off, uint32_t val)
{
    qtest_writel(s, SENS_BASE + off, val);
}

static uint32_t apb_r(uint32_t off)
{
    return qtest_readl(s, APB_BASE + off);
}

static void apb_w(uint32_t off, uint32_t val)
{
    qtest_writel(s, APB_BASE + off, val);
}

/* Program an IDF-shaped unit1 oneshot: ANALOG_CLOCK_ENABLE + channel + SW
 * force bits.  Attenuation and SAR1_CLK_DIV stay at reset (div 2). */
static void unit1_setup(unsigned ch)
{
    sens_w(R_PERI_CLK_GATE, sens_r(R_PERI_CLK_GATE) | BIT(30));
    sens_w(R_MEAS1_CTRL2, CTRL2_START_FORCE | CTRL2_ENPAD_FORCE |
                          (1u << (CTRL2_ENPAD_S + ch)));
}

/* adc_oneshot_ll_start: field-assign write of 0 then 1 -> rising edge. */
static void unit1_start(void)
{
    sens_w(R_MEAS1_CTRL2, sens_r(R_MEAS1_CTRL2) & ~CTRL2_START);
    sens_w(R_MEAS1_CTRL2, sens_r(R_MEAS1_CTRL2) | CTRL2_START);
}

static uint64_t diag_get(const char *prop)
{
    QDict *r = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, 'property': %s}}",
        SENS_PATH, prop);
    uint64_t v = qdict_get_try_int(r, "return", UINT64_MAX);

    g_assert_cmpuint(v, !=, UINT64_MAX);
    qobject_unref(r);
    return v;
}

/* libqtest's %s interpolates escaped, *quoted* JSON strings, so commands
 * that need raw JSON sub-objects are composed here and sent with
 * qtest_qmp_send_raw. */
static QDict *qmp_cmd(const char *cmd)
{
    qtest_qmp_send_raw(s, "%s\n", cmd);
    return qtest_qmp_receive_dict(s);
}

static void qmp_ok(char *cmd)
{
    QDict *r = qmp_cmd(cmd);

    g_assert(!qdict_haskey(r, "error"));
    qobject_unref(r);
    g_free(cmd);
}

static void provider_set_str(const char *id, const char *prop,
                             const char *val)
{
    /* The value is inserted verbatim: bool properties take quoted strings,
     * uint properties need JSON numbers — callers pass the right literal. */
    const char *objects = g_str_has_prefix(id, "prov") ? "/objects" :
                          "/machine/peripheral";
    qmp_ok(g_strdup_printf(
        "{'execute': 'qom-set', 'arguments': {'path': '%s/%s',"
        " 'property': '%s', 'value': %s}}", objects, id, prop, val));
}

/* ------------------------------------------------------------------ */

static void test_reset_defaults(void)
{
    /* SENS reset defaults (TRM/IDF): div 2, clk gated, int enabled. */
    g_assert_cmpuint(sens_r(R_READER1_CTRL), ==, BIT(29) | BIT(18) | 2);
    g_assert_cmpuint(sens_r(R_READER2_CTRL), ==, BIT(30) | BIT(18) | 2);
    g_assert_cmpuint(sens_r(R_MEAS2_CTRL1), ==,
                     (7u << 24) | (2u << 16) | (2u << 8));
    g_assert_cmpuint(sens_r(0x1FC), ==, 0x02101180u);
    /* APB_SARADC reset defaults. */
    g_assert_cmpuint(apb_r(A_CTRL), ==,
                     (1u << 30) | (15u << 19) | (15u << 15) | (4u << 7) | BIT(6));
    g_assert_cmpuint(apb_r(0x04), ==, (10u << 12) | (255u << 1));
    g_assert_cmpuint(apb_r(A_FSM_WAIT), ==, (255u << 16) | (8u << 8) | 8u);
    g_assert_cmpuint(apb_r(A_ARB_CTRL), ==, (2u << 10) | (1u << 8));
    g_assert_cmpuint(apb_r(0x3FC), ==, 0x02101180u);
    /* Live FSM state idle. */
    g_assert_cmpuint(sens_r(0x04), ==, 0);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
}

static void test_unmatched_channel_fails_closed(void)
{
    uint64_t before = diag_get("diag-unknown-sample");

    /* Channel 9 has no provider in the chain: the tail reports UNKNOWN and
     * the SENS fails the conversion closed (same contract as an absent
     * provider). */
    unit1_setup(9);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 1000);
    /* No DONE, no data, FSM back to idle, diagnostic raised. */
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 0);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
    g_assert_cmpuint(diag_get("diag-unknown-sample"), ==, before + 1);
    g_assert_cmpuint(diag_get("diag-invalid-sample"), ==, 1);
}

static void test_divider_quantization_and_interrupt(void)
{
    /* Explicit circuit: 3.3 V rail -> 10k -> tap -> 10k -> ref.  Tap 1.65 V,
     * atten 2 (6 dB, full scale 2.1948 V): code round(1.65/2.1948*4095) =
     * 3079.  Acquisition 1600 ns after start; DONE 29100 ns after start. */
    sens_w(R_ATTEN1, 2u << (2 * 2)); /* ch2 atten 6 dB, others 0 dB */

    unit1_setup(2);
    unit1_start();
    /* Busy during the measurement: FSM XPD (0..800 ns), then SAMPLE
     * aperture (800..1600 ns); no early DONE. */
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 1);
    qtest_clock_step(s, 800);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 2);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    qtest_clock_step(s, DONE_NS - 800 + 10);

    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 3079);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
    /* APB data-status mirror + done event (SAR1_INT_EN reset = 1). */
    g_assert_cmphex(apb_r(A_DATA_STATUS1) & 0x1ffff, ==, 3079);
    g_assert_cmphex(apb_r(A_INT_RAW) & BIT(31), !=, 0);
    /* INT_ST gated by ENA; W1C; W1S on RAW. */
    g_assert_cmphex(apb_r(A_INT_ST), ==, 0);
    apb_w(A_INT_ENA, BIT(31));
    g_assert_cmphex(apb_r(A_INT_ST), ==, BIT(31));
    apb_w(A_INT_CLR, BIT(31));
    g_assert_cmphex(apb_r(A_INT_RAW) & BIT(31), ==, 0);
    g_assert_cmphex(apb_r(A_INT_ST), ==, 0);
    apb_w(A_INT_RAW, BIT(31)); /* software W1S */
    g_assert_cmphex(apb_r(A_INT_RAW) & BIT(31), !=, 0);
    apb_w(A_INT_CLR, BIT(31));
    apb_w(A_INT_ENA, 0);
}

static void test_stale_completion(void)
{
    /* DONE persists until the next start; time passing alone clears nothing */
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
    qtest_clock_step(s, 1000000);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
    /* A new start clears the previous completion. */
    unit1_start();
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
}

static void test_source_edge_vs_aperture(void)
{
    /* prov1 models the same 3.3 V / 10k / 10k divider starting from a 0 V
     * rail; the physical source step is applied by a qom-set between
     * virtual-time steps and the acquisition samples the rail LIVE at the
     * aperture instant (start + 1600 ns). */

    /* Change BEFORE the aperture (T+1000 < T+1600): post-change rail ->
     * 1.65 V tap -> 3079. */
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, 1000);
    provider_set_str("prov1", "rail-mv", "3300");
    qtest_clock_step(s, DONE_NS - 1000 + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 3079);

    /* Change AFTER the aperture (T+2000 > T+1600): pre-change rail 0 V ->
     * code 0. */
    provider_set_str("prov1", "rail-mv", "0");
    unit1_start();
    qtest_clock_step(s, 2000);
    provider_set_str("prov1", "rail-mv", "3300");
    qtest_clock_step(s, DONE_NS - 2000 + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 0);
    g_assert_cmphex(apb_r(A_DATA_STATUS1) & 0x1ffff, ==, 0);
}

static void test_ground_and_clipped_rail(void)
{
    /* Grounded tap is a valid rail: exact 0. */
    provider_set_str("prov1", "rail-mv", "0");
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 0);

    /* Clipped valid rail: tap 1.65 V at 0 dB (full scale 1.1 V) saturates
     * the 12-bit code at 4095. */
    sens_w(R_ATTEN1, 0);
    provider_set_str("prov1", "rail-mv", "3300");
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 4095);
    sens_w(R_ATTEN1, 2u << (2 * 2));
}

static void test_channel_atten_selection(void)
{
    /* Distinct attenuation per channel: ch3 at 0 dB from a 1.8 V rail
     * (tap 0.9 V -> 3350), ch2 still at 6 dB (3079). */
    sens_w(R_ATTEN1, 2u << (2 * 2)); /* ch2 = 6 dB, ch3 = 0 dB */

    unit1_setup(3);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 3350);

    /* Back to the ch2 divider circuit: same attenuation register, different
     * channel -> 6 dB full scale applies (3079, not the ch3 result). */
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 3079);
}

static void test_start_while_busy_restarts(void)
{
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, 1000); /* mid-aperture */
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, !=, 0);

    /* Restart: the previous measurement is cancelled and re-armed; DONE
     * arrives one full conversion time after the SECOND start. */
    unit1_start();
    qtest_clock_step(s, DONE_NS - 1000 - 100);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    /* DONE lands one full conversion after the SECOND start (T0+30100). */
    qtest_clock_step(s, 1100);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 3079);
}

static void test_rtc_side_reset(void)
{
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, 400);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, !=, 0);

    sens_w(R_PERI_RESET, BIT(30));
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    sens_w(R_PERI_RESET, 0);
}

static void test_clock_gate_blocks(void)
{
    uint64_t before = diag_get("diag-gate-blocked");

    sens_w(R_PERI_CLK_GATE, sens_r(R_PERI_CLK_GATE) & ~BIT(30));
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
    g_assert_cmpuint(diag_get("diag-gate-blocked"), ==, before + 1);
    sens_w(R_PERI_CLK_GATE, sens_r(R_PERI_CLK_GATE) | BIT(30));
}

static void test_analog_power_gate(void)
{
    uint64_t before = diag_get("diag-unpowered-sample");

    /* FORCE_XPD_SAR = 2'b10: SAR forced unpowered. */
    sens_w(R_POWER_XPD_SAR, 2u << 29);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(diag_get("diag-unpowered-sample"), ==, before + 1);

    /* Guest programs the real internal-ground mux through REGI2C. It is
     * still an analog acquisition and must obey FORCE_XPD_SAR. */
    qtest_writel(s, 0x6000e000, BIT(26) | BIT(24) | (BIT(5) << 16) |
                               (7u << 8) | 0x69);
    /* Reset wire profile: 37 bits, APB80MHz / divider100. */
    qtest_clock_step(s, 46250);
    g_assert_cmphex(qtest_readl(s, 0x6000e000) & BIT(25), ==, 0);
    unit1_setup(2);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(diag_get("diag-unpowered-sample"), ==, before + 2);
    sens_w(R_POWER_XPD_SAR, 0);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, !=, 0);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & 0xffff, ==, 0);
    qtest_writel(s, 0x6000e000, BIT(26) | BIT(24) | (7u << 8) | 0x69);
    qtest_clock_step(s, 46250);
    g_assert_cmphex(qtest_readl(s, 0x6000e000) & BIT(25), ==, 0);
}

static void test_ownership_gates(void)
{
    uint64_t ulp_before = diag_get("diag-ulp-ignored");
    uint64_t dig_before = diag_get("diag-dig-force-ignored");

    /* ULP-owned controller (START_FORCE = 0): SW start ignored. */
    sens_w(R_MEAS1_CTRL2, CTRL2_ENPAD_FORCE | (1u << (CTRL2_ENPAD_S + 2)));
    sens_w(R_MEAS1_CTRL2, sens_r(R_MEAS1_CTRL2) | CTRL2_START);
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(diag_get("diag-ulp-ignored"), ==, ulp_before + 1);

    /* ADC1 handed to the DIG controller: RTC start ignored. */
    unit1_setup(2);
    sens_w(R_MEAS1_MUX, BIT(31));
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(diag_get("diag-dig-force-ignored"), ==, dig_before + 1);
    sens_w(R_MEAS1_MUX, 0);
}

static void test_adc2_and_arbiter(void)
{
    /* ADC2 channel 0 (GPIO11) at 0 dB: tap 0.9 V -> code 3350, flag 2'b00. */
    sens_w(R_ATTEN2, 0);
    sens_w(R_PERI_CLK_GATE, sens_r(R_PERI_CLK_GATE) | BIT(30));

    sens_w(R_MEAS2_CTRL2, CTRL2_START_FORCE | CTRL2_ENPAD_FORCE |
                          (1u << (CTRL2_ENPAD_S + 0)));
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) & ~CTRL2_START);
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) | CTRL2_START);
    qtest_clock_step(s, DONE_NS + 10);
    /* Valid conversion, TRM data flag 2'b00 in [15:14]. */
    g_assert_cmphex(sens_r(R_MEAS2_CTRL2) & 0xffff, ==, 3350);
    g_assert_cmphex(sens_r(R_MEAS2_CTRL2) & (0x3u << 14), ==, 0);
    g_assert_cmphex(apb_r(0x78) & 0x1ffff, ==, 3350);

    /* Arbiter masked against the RTC controller: conversion completes with
     * the TRM "not started" flag 2'b01. */
    apb_w(A_ARB_CTRL, (2u << 10) | (1u << 8) | BIT(5) | BIT(4));
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) & ~CTRL2_START);
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) | CTRL2_START);
    qtest_clock_step(s, DONE_NS + 10);
    /* The conversion is flagged "not started" (2'b01); the data field is
     * stale/meaningless and the flag is what firmware checks. */
    g_assert_cmphex(sens_r(R_MEAS2_CTRL2) & (0x3u << 14), ==, BIT(14));
    g_assert_cmpuint(diag_get("diag-arbiter-denied"), ==, 1);
    apb_w(A_ARB_CTRL, (2u << 10) | (1u << 8));

    /* ADC2 done event mirrors into INT_RAW bit 30 (SAR2_INT_EN reset = 1). */
    apb_w(A_INT_CLR, BIT(30));
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) & ~CTRL2_START);
    sens_w(R_MEAS2_CTRL2, sens_r(R_MEAS2_CTRL2) | CTRL2_START);
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(apb_r(A_INT_RAW) & BIT(30), !=, 0);
    apb_w(A_INT_CLR, BIT(30));
}

static void test_floating_and_digital_owned(void)
{
    uint64_t flt_before = diag_get("diag-floating-sample");
    uint64_t dig_before = diag_get("diag-digital-owned-sample");


    /* Open divider: the tap is a floating net.  Fail closed: no DONE, no
     * data, previous result stays stale, diagnostic counted. */
    unit1_setup(5);
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(sens_r(R_SLAVE_ADDR1) >> MEAS_STATUS_S, ==, 0);
    g_assert_cmpuint(diag_get("diag-floating-sample"), ==, flt_before + 1);

    /* Digitally-owned pad: same fail-closed treatment. */
    provider_set_str("prov4", "digital-owned", "true");
    unit1_start();
    qtest_clock_step(s, DONE_NS + 10);
    g_assert_cmphex(sens_r(R_MEAS1_CTRL2) & CTRL2_DONE, ==, 0);
    g_assert_cmpuint(diag_get("diag-digital-owned-sample"), ==, dig_before + 1);
    provider_set_str("prov4", "digital-owned", "false");
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    /* Canonical wiring: providers are user-creatable -object instances
     * (pre-machine, under /objects), and the SENS class typed link
     * "sample-provider" resolves them BEFORE realize via -global.  Creation
     * order is reverse-reference (a chain member must exist before the
     * member that names it). Chain: prov1 (ADC1 ch2 divider) -> prov3
     * (ADC1 ch3 divider) -> prov2 (ADC2 ch0 divider) -> prov4 (ADC1 ch5,
     * floating tail). Unmatched channels fall off the tail as UNKNOWN.
     * Internal calibration ground is selected by real REGI2C guest bits,
     * never by an extra grounded GPIO1 provider. */
    s = qtest_initf("-machine esp32s3 "
                    "-object adc-dc-provider,id=prov4,unit=0,channel=5,"
                    "rail-mv=3300,float-net=true "
                    "-object adc-dc-provider,id=prov2,unit=1,channel=0,"
                    "rail-mv=1800,r-top-ohm=10000,r-bot-ohm=10000,"
                    "next=/objects/prov4 "
                    "-object adc-dc-provider,id=prov3,unit=0,channel=3,"
                    "rail-mv=1800,r-top-ohm=10000,r-bot-ohm=10000,"
                    "next=/objects/prov2 "
                    "-object adc-dc-provider,id=prov1,unit=0,channel=2,"
                    "rail-mv=3300,r-top-ohm=10000,r-bot-ohm=10000,"
                    "next=/objects/prov3 "
                    "-global driver=esp32s3.sens,property=sample-provider,"
                    "value=/objects/prov1 ");

    qtest_add_func("esp32s3-adc/reset-defaults", test_reset_defaults);
    qtest_add_func("esp32s3-adc/unmatched-channel-fails-closed",
                   test_unmatched_channel_fails_closed);
    qtest_add_func("esp32s3-adc/divider-quantization",
                   test_divider_quantization_and_interrupt);
    qtest_add_func("esp32s3-adc/stale-completion", test_stale_completion);
    qtest_add_func("esp32s3-adc/edge-vs-aperture", test_source_edge_vs_aperture);
    qtest_add_func("esp32s3-adc/ground-clipped", test_ground_and_clipped_rail);
    qtest_add_func("esp32s3-adc/channel-atten", test_channel_atten_selection);
    qtest_add_func("esp32s3-adc/busy-restart", test_start_while_busy_restarts);
    qtest_add_func("esp32s3-adc/rtc-reset", test_rtc_side_reset);
    qtest_add_func("esp32s3-adc/clock-gate", test_clock_gate_blocks);
    qtest_add_func("esp32s3-adc/power-gate", test_analog_power_gate);
    qtest_add_func("esp32s3-adc/ownership", test_ownership_gates);
    qtest_add_func("esp32s3-adc/adc2-arbiter", test_adc2_and_arbiter);
    qtest_add_func("esp32s3-adc/invalid-samples", test_floating_and_digital_owned);

    ret = g_test_run();
    qtest_quit(s);
    return ret;
}
