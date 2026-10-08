/*
 * ESP32-S3 SENS block: RTC ADC1/ADC2 oneshot measurement controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sources: ESP32-S3 TRM v1.8 chapter 39 ("On-Chip Sensors and Analog Signal
 * Processing"), pinned ESP-IDF v6.1.0 (fff9895c) soc/esp32s3 register headers
 * (sens_reg.h / sens_struct.h / apb_saradc_reg.h) and
 * esp_hal_ana_conv/esp32s3/include/hal/adc_ll.h.
 *
 * Modeled behavior (ADC-01 bounded ideal profile):
 *  - RTC controller oneshot conversion FSM with SW start (MEASn_START_SAR
 *    rising edge, gated by MEASn_START_FORCE = SW ownership); conversion
 *    timing from RTCADC_SARCLK = RTC_FAST_CLK / SARn_CLK_DIV (TRM 39.3: the
 *    programmed divider is honored; the <= 5 MHz precision limit is
 *    diagnosed, not coerced).  Phases: xpd -> acquisition aperture (4 *
 *    sample_cycle sar clocks; REGI2C ADC_SAR1_SAMPLE_CYCLE reset default 2)
 *    -> 12-cycle SAR approximation -> rstb -> standby -> DONE.
 *  - The pad voltage is resolved ONLY at the acquisition instant through the
 *    Esp32S3AdcSampleProvider (net solver layer).  Floating, unpowered,
 *    digitally-owned or unknown samples FAIL CLOSED: the FSM returns to
 *    idle, DONE is never set, no data is consumable, a wiring diagnostic
 *    (and counter) is raised.  With "strict-invalid-sample" = true the
 *    simulation pauses with a wiring error instead of continuing.
 *  - Per-channel attenuation (SAR_ATTEN1/2), data invert (READERn_CTRL),
 *    ADC2 arbiter grant/deny with the TRM data flags [15:14]
 *    (2'b01 not-started, 2'b10 interrupted), power (SAR_POWER_XPD_SAR) and
 *    clock (SAR_PERI_CLK_GATE_CONF.SARADC_CLK_EN) gates, RTC-side reset
 *    (SAR_PERI_RESET_CONF.SARADC_RESET) and live FSM state exposure
 *    (SAR_SLAVE_ADDR1.MEAS_STATUS, SAR_READERn_STATUS,
 *    SAR_MEAS2_CTRL1.CNTL_STATE).
 *  - Interrupts: done events propagate into APB_SARADC INT_RAW (ADC1_DONE /
 *    ADC2_DONE) gated by SARn_INT_EN; the APB device owns the level IRQ.
 *
 * Ideal quantization profile (documented, NOT a silicon calibration claim):
 *   code12 = round(4095 * clamp(v, 0, VDD_A) / (Vref * 10^(atten/20)))
 *   with Vref = 1.1 V nominal, VDD_A = 3.3 V; offset/gain/nonlinearity = 0,
 *   noise disabled (exact repeat); seeded noise and calibrated curves are
 *   ADC-02+ packages.  SARn_DATA_INV inverts the 12-bit result field.
 *
 * Unsupported analog surfaces (touch, temperature sensor, ULP/COCPU
 * measurement) store registers with real read/write/W1C semantics but NEVER
 * fabricate results: a start pulse fails closed with a diagnostic (no
 * DONE/READY/data).  The regi2c lane consumes the named QOM outputs
 * "sar2-armed" and "tsens-dump-out" instead of owning SENS ranges; this
 * device is the single owner of 0x60008800..0x600089FF.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "sysemu/runstate.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/misc/esp32s3_sens.h"
#include "hw/misc/esp32s3_regi2c.h"

static void sens_timer_trampoline(void *opaque);

/* ---- register word indices (SENS + 4*i, TRM ch.39 / sens_reg.h) ---- */
#define R_READER1_CTRL          0x00 /* div [7:0], gated [18], num [26:19], inv [28], int_en [29] */
#define R_READER1_STATUS        0x01 /* RO */
#define R_MEAS1_CTRL1           0x02
#define R_MEAS1_CTRL2           0x03 /* data [15:0], done [16], start [17], start_force [18], en_pad [30:19], en_pad_force [31] */
#define R_MEAS1_MUX             0x04 /* sar1_dig_force [31] */
#define R_ATTEN1                0x05
#define R_AMP_CTRL1             0x06
#define R_AMP_CTRL2             0x07
#define R_AMP_CTRL3             0x08
#define R_READER2_CTRL          0x09 /* div [7:0], wait_arb [17:16], gated [18], num [26:19], inv [29], int_en [30] */
#define R_READER2_STATUS        0x0A /* RO */
#define R_MEAS2_CTRL1           0x0B /* cntl_state [2:0] RO, cal ens, en_test [5], rstb_force [7:6], standby [15:8], rstb [23:16], xpd [31:24] */
#define R_MEAS2_CTRL2           0x0C
#define R_MEAS2_MUX             0x0D /* pwdet_cct [30:28], sar2_rtc_force [31] */
#define R_ATTEN2                0x0E
#define R_POWER_XPD_SAR         0x0F /* force_xpd_sar [30:29], sarclk_en [31] */
#define R_SLAVE_ADDR1           0x10 /* meas_status [29:22] RO */
#define R_SLAVE_ADDR2           0x11
#define R_SLAVE_ADDR3           0x12
#define R_SLAVE_ADDR4           0x13
#define R_TSENS_CTRL            0x14 /* out [7:0], ready [8] RO, xpd_wait [11:0], clk_div [21:14], dump_out [24] */
#define R_TOUCH_CONF            0x17 /* 0x5C; touch window stored, never faked */
#define R_TOUCH_CHN_ST          0x27 /* 0x9C scan/done status, RO */
#define R_COCPU_INT_RAW         0x3A /* 0xE8 */
#define R_COCPU_INT_ENA         0x3B /* 0xEC */
#define R_COCPU_INT_ST          0x3C /* 0xF0 RO = raw & ena */
#define R_COCPU_INT_CLR         0x3D /* 0xF4 W1C */
#define R_PERI_CLK_GATE         0x41 /* 0x104: rtc_i2c [27], tsens [29], saradc [30], iomux [31] */
#define R_PERI_RESET            0x42 /* 0x108: reset [25], rtc_i2c [27], tsens [29], saradc [30] */
#define R_COCPU_INT_ENA_W1TS    0x43 /* 0x10C */
#define R_COCPU_INT_ENA_W1TC    0x44 /* 0x110 */
#define R_SARDATE               0x7F /* 0x1FC RO, [27:0] */

/* MEASn_CTRL2 fields */
#define SENS_CTRL2_DATA_M       0x0000ffffu
#define SENS_CTRL2_DONE         BIT(16)
#define SENS_CTRL2_START        BIT(17)
#define SENS_CTRL2_START_FORCE  BIT(18)
#define SENS_CTRL2_EN_PAD_S     19
#define SENS_CTRL2_EN_PAD_M     0xfffu
#define SENS_CTRL2_EN_PAD_FORCE BIT(31)

/* READERn_CTRL fields */
#define READER_DIV_M            0x000000ffu
#define READER1_DIG_FORCE       BIT(31) /* MEAS1_MUX */
#define READER_CLK_GATED        BIT(18)
#define READER1_DATA_INV        BIT(28)
#define READER1_INT_EN          BIT(29)
#define READER2_DATA_INV        BIT(29)
#define READER2_INT_EN          BIT(30)

/* MEAS2_CTRL1 */
#define MEAS2_CNTL_STATE_M      0x7u
#define MEAS2_STANDBY_S         8
#define MEAS2_RSTB_S            16
#define MEAS2_XPD_S             24

/* MEAS2_MUX */
#define MEAS2_MUX_RTC_FORCE     BIT(31)

/* SAR_POWER_XPD_SAR */
#define POWER_FORCE_XPD_S       29
#define POWER_FORCE_XPD_M       0x3u
#define POWER_XPD_SW_OFF        0x2u

/* SAR_PERI_CLK_GATE_CONF / SAR_PERI_RESET_CONF */
#define GATE_SARADC_CLK_EN      BIT(30)
#define RESET_SARADC            BIT(30)

/* SAR_SLAVE_ADDR1.MEAS_STATUS */
#define MEAS_STATUS_S           22

/* SAR_TSENS_CTRL */
#define TSENS_OUT_M             0x000000ffu
#define TSENS_READY             BIT(8)
#define TSENS_DUMP_OUT          BIT(24)

/* REGI2C ADC_SAR1_SAMPLE_CYCLE reset default (regi2c_saradc.h: addr 0x2
 * [2:0], silicon reset value 2; live REGI2C tracking is an ADC-02 item). */
#define SAR1_SAMPLE_CYCLE_DEFAULT 2u

/* Conversion FSM constants (RTCADC_SARCLK cycles). */
#define SAR_SAMPLE_CYCLES_PER_CYCLE 4u
#define SAR_CONVERT_CYCLES          12u

/* TRM data flags for the RTC ADC2 output (MEAS2_DATA [15:14]). */
#define RTC2_FLAG_VALID       0x0u
#define RTC2_FLAG_NOT_STARTED 0x1u
#define RTC2_FLAG_INTERRUPTED 0x2u

const double esp32s3_adc_atten_db[4] = { 0.0, 2.5, 6.0, 12.0 };

uint32_t esp32s3_adc_ideal_quantize(double voltage_v, unsigned atten)
{
    double v_full, v, r;

    if (atten > 3) {
        atten = 3;
    }
    v_full = ESP32S3_ADC_IDEAL_VREF_MV / 1000.0 *
             pow(10.0, esp32s3_adc_atten_db[atten] / 20.0);
    v = CLAMP(voltage_v, 0.0, ESP32S3_ADC_IDEAL_VDD_V);
    r = round(v / v_full * 4095.0);
    return (uint32_t)CLAMP(r, 0.0, 4095.0);
}

/* ------------------------------------------------------------------ */
/* RTC controller conversion FSM                                       */
/* ------------------------------------------------------------------ */

static void sens_phase_advance(ESP32S3SensState *s, unsigned unit);

static uint64_t sens_now_ns(ESP32S3SensState *s)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint32_t sens_meas_ctrl2(ESP32S3SensState *s, unsigned unit)
{
    return s->regs[unit ? R_MEAS2_CTRL2 : R_MEAS1_CTRL2];
}

static uint32_t sens_reader_ctrl(ESP32S3SensState *s, unsigned unit)
{
    return s->regs[unit ? R_READER2_CTRL : R_READER1_CTRL];
}

/* RTCADC_SARCLK = RTC_FAST_CLK / div (TRM 39.3: divider honored; the 5 MHz
 * sampling-precision limit is diagnosed, never silently coerced). */
static uint64_t sens_sar_clk_hz(ESP32S3SensState *s, unsigned unit)
{
    uint32_t div = sens_reader_ctrl(s, unit) & READER_DIV_M;
    uint64_t src;

    if (clock_has_source(s->rtc_fast_clk)) {
        src = clock_get_hz(s->rtc_fast_clk);
    } else {
        src = 20000000ull; /* CoreClockWorker reset profile: XTAL/2 */
    }
    if (div == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: SAR%d_CLK_DIV=0 is undefined, treating"
                      " as 1\n", unit + 1);
        div = 1;
    }
    if (src / div > 5000000ull) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: RTCADC_SARCLK %" PRIu64 " Hz exceeds the"
                      " 5 MHz precision limit (div=%u)\n", src / div, div);
    }
    return src / div;
}

static uint64_t sens_cycles_to_ns(ESP32S3SensState *s, unsigned unit,
                                  uint64_t cycles)
{
    uint64_t hz = sens_sar_clk_hz(s, unit);

    if (hz == 0 || cycles == 0) {
        return 0;
    }
    return muldiv64(cycles, NANOSECONDS_PER_SECOND, hz);
}

/* Phase wait configuration.  SAR1 shares the APB_SARADC FSM_WAIT profile
 * (the SENS block has no SAR1 wait fields); SAR2 uses its native
 * SENS_SAR_MEAS2_CTRL1 wait fields.  Documented profile, see README. */
static uint32_t sens_fsm_wait(ESP32S3SensState *s, unsigned unit, unsigned phase)
{
    uint32_t v;

    if (unit == 0) {
        v = esp32s3_apb_saradc_get_reg(s->apb_saradc, 0x0C / 4);
    } else {
        v = s->regs[R_MEAS2_CTRL1];
    }
    switch (phase) {
    case SAR_FSM_XPD:
        return (v >> (unit ? MEAS2_XPD_S : 0)) & 0xff;
    case SAR_FSM_RSTB:
        return (v >> (unit ? MEAS2_RSTB_S : 8)) & 0xff;
    default:
        return (v >> (unit ? MEAS2_STANDBY_S : 16)) & 0xff;
    }
}

static bool sens_ground_route(ESP32S3SensState *s, unsigned unit, bool *ground)
{
    uint8_t cal = 0;
    bool written = false;

    *ground = false;
    if (!s->regi2c) {
        return true;
    }
    if (!esp32s3_regi2c_get_slave_reg(s->regi2c, 0x69, 0x7,
                                     &cal, &written)) {
        return false;
    }
    *ground = written && (cal & BIT(unit ? 7 : 5));
    return true;
}

static uint64_t sens_source_generation(ESP32S3SensState *s, unsigned unit,
                                       unsigned channel)
{
    return s->provider ?
        ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(s->provider)
            ->source_generation(s->provider, unit, channel) : 0;
}

static void sens_cancel_pause(ESP32S3SensState *s)
{
    ++s->pause_epoch;
    s->pause_pending = 0;
    qemu_bh_cancel(s->pause_bh);
}

/* Timer callbacks hold the timerlist lock. Pause from the main-loop bottom
 * half, like REGI2C, so stopping vCPUs cannot deadlock that lock. */
static void sens_pause_bh(void *opaque)
{
    ESP32S3SensState *s = opaque;
    bool pause = false;
    uint8_t pending = s->pause_pending;

    s->pause_pending = 0;
    if (s->pause_scheduled_epoch != s->pause_epoch) {
        return;
    }
    for (unsigned unit = 0; unit < 2; unit++) {
        bool ground;

        if (!(pending & BIT(unit))) {
            continue;
        }
        if (!s->pause_source_dependent[unit] ||
            !sens_ground_route(s, unit, &ground) ||
            (!ground && s->pause_source_generation[unit] ==
                sens_source_generation(s, unit, s->pause_channel[unit]))) {
            pause = true;
            break;
        }
    }
    /* A repaired/replaced physical source cancels only a stale request.
     * It never completes the failed aperture or resumes a stopped VM. */
    if (pause && s->strict_invalid_sample && runstate_is_running() &&
        vm_stop(RUN_STATE_PAUSED) < 0) {
        error_report("esp32s3.sens: unable to pause VM on invalid sample");
    }
}

static void sens_fail_closed(ESP32S3SensState *s, unsigned unit,
                             Esp32S3AdcSampleValidity why)
{
    static const char *const why_str[] = {
        "valid", "floating", "unpowered", "digital-owned", "unknown",
    };

    s->diag_invalid_sample++;
    switch (why) {
    case ESP32S3_ADC_SAMPLE_FLOATING:
        s->diag_floating_sample++;
        break;
    case ESP32S3_ADC_SAMPLE_UNPOWERED:
        s->diag_unpowered_sample++;
        break;
    case ESP32S3_ADC_SAMPLE_DIGITAL_OWNED:
        s->diag_digital_owned_sample++;
        break;
    case ESP32S3_ADC_SAMPLE_UNKNOWN:
        s->diag_unknown_sample++;
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "esp32s3.sens: ADC%d conversion failed closed: %s sample at"
                  " channel %u (no DONE raised, data not consumable)\n",
                  unit + 1, why_str[why], s->unit[unit].channel);
    if (s->strict_invalid_sample) {
        error_report("esp32s3.sens: strict-invalid-sample: ADC%d wiring error"
                     " (%s sample), stopping simulation", unit + 1,
                     why_str[why]);
        s->pause_channel[unit] = s->unit[unit].channel;
        s->pause_source_generation[unit] =
            sens_source_generation(s, unit, s->pause_channel[unit]);
        s->pause_source_dependent[unit] =
            ((s->regs[R_POWER_XPD_SAR] >> POWER_FORCE_XPD_S) &
             POWER_FORCE_XPD_M) != POWER_XPD_SW_OFF;
        s->pause_pending |= BIT(unit);
        s->pause_scheduled_epoch = s->pause_epoch;
        qemu_bh_schedule(s->pause_bh);
    }
    s->unit[unit].fsm = SAR_FSM_IDLE;
}

static void sens_acquire(ESP32S3SensState *s, unsigned unit)
{
    Esp32S3AdcSample sample;
    Esp32S3AdcSampleValidity why;
    uint32_t power_force;
    bool internal_ground = false;
    /* The aperture instant is pinned once; the provider must solve and echo
     * exactly this stamp.  Comparing the echo against a later clock read
     * would false-positive under a running virtual clock. */
    uint64_t requested_ns = sens_now_ns(s);

    memset(&sample, 0, sizeof(sample));
    sample.sample_ns = requested_ns;

    /* Internal calibration mux: when the firmware routed the SAR input to
     * the internal silicon ground (REGI2C ADC_SARn_ENCAL_GND, driven by the
     * IDF self-calibration), the conversion measures that internal route —
     * hardware-defined 0 V, independent of any external net or provider. */
    if (!sens_ground_route(s, unit, &internal_ground)) {
        sens_fail_closed(s, unit, ESP32S3_ADC_SAMPLE_UNKNOWN);
        return;
    }

    if (internal_ground) {
        why = ESP32S3_ADC_SAMPLE_VALID;
    } else if (s->provider == NULL) {
        why = ESP32S3_ADC_SAMPLE_UNKNOWN;
    } else {
        ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(s->provider)
            ->sample(s->provider, unit, s->unit[unit].channel, &sample);
        why = sample.validity;
        if (why == ESP32S3_ADC_SAMPLE_VALID &&
            sample.sample_ns != requested_ns) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.sens: provider echoed a foreign sample"
                          " time (%" PRIu64 " != %" PRIu64 ")\n",
                          sample.sample_ns, requested_ns);
        }
    }

    /* Analog power gate: FORCE_XPD_SAR == 2'b10 forces the SAR unpowered. */
    power_force = (s->regs[R_POWER_XPD_SAR] >> POWER_FORCE_XPD_S) &
                  POWER_FORCE_XPD_M;
    if (why == ESP32S3_ADC_SAMPLE_VALID && power_force == POWER_XPD_SW_OFF) {
        why = ESP32S3_ADC_SAMPLE_UNPOWERED;
    }

    if (why != ESP32S3_ADC_SAMPLE_VALID) {
        sens_fail_closed(s, unit, why);
        return;
    }

    if (sample.voltage_v > ESP32S3_ADC_IDEAL_VDD_V) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: overvoltage %.3f V on ADC%d channel %u"
                      " clamped to VDD_A\n",
                      sample.voltage_v, unit + 1, s->unit[unit].channel);
    }
    s->unit[unit].code =
        esp32s3_adc_ideal_quantize(sample.voltage_v, s->unit[unit].atten);
}

static void sens_complete(ESP32S3SensState *s, unsigned unit)
{
    uint32_t data, reader;
    bool int_en;

    data = s->unit[unit].code & 0xfffu;
    if (unit == 1) {
        data |= (s->unit[unit].arbiter_denied ? RTC2_FLAG_NOT_STARTED
                                               : RTC2_FLAG_VALID) << 14;
    }
    if (sens_reader_ctrl(s, unit) &
        (unit ? READER2_DATA_INV : READER1_DATA_INV)) {
        data ^= 0x0fff; /* SARn_DATA_INV inverts the result field */
    }

    s->regs[unit ? R_MEAS2_CTRL2 : R_MEAS1_CTRL2] =
        (sens_meas_ctrl2(s, unit) & ~(SENS_CTRL2_DATA_M | SENS_CTRL2_DONE)) |
        data | SENS_CTRL2_DONE;

    reader = sens_reader_ctrl(s, unit);
    int_en = (unit == 0) ? (reader & READER1_INT_EN) != 0
                         : (reader & READER2_INT_EN) != 0;
    esp32s3_apb_saradc_rtc_result(s->apb_saradc, unit, data, int_en);
    s->unit[unit].fsm = SAR_FSM_IDLE;
}

/* Fire the next FSM phase.  Zero-wait phases collapse synchronously; every
 * wait is bounded and virtual-time driven (no host-wall blocking). */
static void sens_phase_advance(ESP32S3SensState *s, unsigned unit)
{
    ESP32S3SarUnit *u = &s->unit[unit];
    uint64_t cycles = 0;

    switch (u->fsm) {
    case SAR_FSM_XPD:
        u->fsm = SAR_FSM_SAMPLE;
        cycles = (uint64_t)SAR1_SAMPLE_CYCLE_DEFAULT *
                 SAR_SAMPLE_CYCLES_PER_CYCLE;
        break;
    case SAR_FSM_SAMPLE:
        sens_acquire(s, unit);
        if (u->fsm == SAR_FSM_IDLE) {
            return; /* failed closed */
        }
        u->fsm = SAR_FSM_CONVERT;
        cycles = SAR_CONVERT_CYCLES;
        break;
    case SAR_FSM_CONVERT:
        u->arb_extra = unit ? ((s->regs[R_READER2_CTRL] >> 16) & 0x3) : 0;
        u->fsm = SAR_FSM_RSTB;
        cycles = sens_fsm_wait(s, unit, SAR_FSM_RSTB);
        break;
    case SAR_FSM_RSTB:
        u->fsm = SAR_FSM_STANDBY;
        cycles = sens_fsm_wait(s, unit, SAR_FSM_STANDBY);
        if (unit) {
            cycles += u->arb_extra + 1; /* SAR2_WAIT_ARB_CYCLE */
        }
        break;
    case SAR_FSM_STANDBY:
        sens_complete(s, unit);
        return;
    default:
        return;
    }

    if (cycles == 0) {
        sens_phase_advance(s, unit);
    } else {
        timer_mod(u->timer, sens_now_ns(s) + sens_cycles_to_ns(s, unit, cycles));
    }
}

/* SAR2 arming signal for the regi2c lane's PWDET sequencer:
 * SENS_SAR_MEAS2_CTRL2.EN_PAD_FORCE && !SENS_SAR_MEAS2_MUX.RTC_FORCE. */
static void sens_sar2_armed_update(ESP32S3SensState *s)
{
    bool armed = (s->regs[R_MEAS2_CTRL2] & SENS_CTRL2_EN_PAD_FORCE) &&
                 !(s->regs[R_MEAS2_MUX] & MEAS2_MUX_RTC_FORCE);
    qemu_set_irq(s->sar2_armed_irq, armed);
}

/* Rising MEASn_START_SAR edge (IDF adc_oneshot_ll_start writes 0 then 1). */
static void sens_start(ESP32S3SensState *s, unsigned unit)
{
    ESP32S3SarUnit *u = &s->unit[unit];
    uint32_t ctrl2 = sens_meas_ctrl2(s, unit);
    uint32_t en_pad;
    unsigned ch = 0, popcount = 0;

    /* A new start clears the previous completion (documented FSM semantics:
     * DONE self-clears on the next measurement start). */
    s->regs[unit ? R_MEAS2_CTRL2 : R_MEAS1_CTRL2] &= ~SENS_CTRL2_DONE;

    if (!(ctrl2 & SENS_CTRL2_START_FORCE)) {
        /* ULP owns this controller (RTC_CNTL_ULP_CP_* start path); the ULP
         * is not modeled, so this SW start cannot run. */
        s->diag_ulp_ignored++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: ADC%d start ignored: MEAS%d_START_FORCE=0"
                      " (ULP-owned, ULP not modeled)\n", unit + 1, unit + 1);
        return;
    }
    if (unit == 0 && (s->regs[R_MEAS1_MUX] & READER1_DIG_FORCE)) {
        /* ADC1 is handed to the DIG controller; the RTC start is not in
         * control of the analog mux. */
        s->diag_dig_force_ignored++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: ADC1 start ignored: SAR1_DIG_FORCE=1"
                      " (DIG controller owns ADC1)\n");
        return;
    }
    if (!(s->regs[R_PERI_CLK_GATE] & GATE_SARADC_CLK_EN)) {
        /* SARADC RTC-domain clock gated: the FSM cannot make progress and no
         * DONE will ever be raised. */
        s->diag_gate_blocked++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: ADC%d start blocked: SAR_PERI_CLK_GATE_CONF"
                      ".SARADC_CLK_EN=0 (no DONE will be raised)\n", unit + 1);
        return;
    }

    en_pad = (ctrl2 >> SENS_CTRL2_EN_PAD_S) & SENS_CTRL2_EN_PAD_M;
    if (!(ctrl2 & SENS_CTRL2_EN_PAD_FORCE)) {
        en_pad = 0; /* ULP-selected pad bitmap; not modeled */
    }
    if (en_pad == 0) {
        /* HW measures channel 0 with its configured attenuation when no pad
         * is enabled (behavior relied on by IDF self-calibration). */
        ch = 0;
    } else {
        for (unsigned i = 0; i < 10; i++) {
            if (en_pad & BIT(i)) {
                ch = i;
                popcount++;
            }
        }
        if (popcount > 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.sens: ADC%d SAR%d_EN_PAD=0x%03x selects"
                          " multiple pads; measuring lowest channel %u\n",
                          unit + 1, unit + 1, en_pad, ch);
        }
    }

    /* Restart semantics: a start pulse while busy re-arms the FSM. */
    if (u->fsm != SAR_FSM_IDLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: ADC%d start while conversion in progress:"
                      " previous measurement cancelled and re-armed\n",
                      unit + 1);
        timer_del(u->timer);
    }

    u->channel = ch;
    u->atten = (s->regs[unit ? R_ATTEN2 : R_ATTEN1] >> (2 * ch)) & 0x3;
    u->arbiter_denied = false;
    u->arb_extra = 0;

    if (unit == 1) {
        /* SAR ADC2 arbiter (TRM 39.3.8).  With no PWDET contender modeled the
         * fair/fixed arbitration always grants the RTC controller; explicit
         * masking with GRANT_FORCE + WIFI_FORCE (and RTC_FORCE clear) denies
         * it: the conversion still completes but the data carries flag
         * 2'b01 "not started".  Scripted PWDET interruption (flag 2'b10) is
         * the ADC-05 extension point.  SENS_SAR_MEAS2_MUX.SAR2_RTC_FORCE
         * masks the arbiter in favor of the RTC controller. */
        if (!(s->regs[R_MEAS2_MUX] & MEAS2_MUX_RTC_FORCE)) {
            uint32_t arb = esp32s3_apb_saradc_get_reg(s->apb_saradc, 0x38 / 4);
            bool grant_force = (arb >> 5) & 0x1;
            bool wifi_force = (arb >> 4) & 0x1;
            bool rtc_force = (arb >> 3) & 0x1;

            if (grant_force && wifi_force && !rtc_force) {
                u->arbiter_denied = true;
                s->diag_arbiter_denied++;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "esp32s3.sens: ADC2 RTC start denied by arbiter"
                              " mask (WIFI_FORCE); data will carry flag 2'b01"
                              "\n");
            }
        }
    }

    u->fsm = SAR_FSM_XPD;
    {
        uint64_t cycles = sens_fsm_wait(s, unit, SAR_FSM_XPD);

        if (cycles == 0) {
            sens_phase_advance(s, unit); /* zero-wait collapse into SAMPLE */
        } else {
            timer_mod(u->timer,
                      sens_now_ns(s) + sens_cycles_to_ns(s, unit, cycles));
        }
    }
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

static uint64_t esp32s3_sens_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3SensState *s = ESP32S3_SENS(opaque);
    uint32_t idx = addr >> 2;

    switch (addr) {
    case 0x04: /* SAR_READER1_STATUS: live reader FSM state */
        return s->unit[0].fsm;
    case 0x28: /* SAR_READER2_STATUS */
        return s->unit[1].fsm;
    case 0x2C: /* SAR_MEAS2_CTRL1: CNTL_STATE mirrors the live SAR2 FSM */
        return (s->regs[R_MEAS2_CTRL1] & ~MEAS2_CNTL_STATE_M) |
               (s->unit[1].fsm & MEAS2_CNTL_STATE_M);
    case 0x40: /* SAR_SLAVE_ADDR1: MEAS_STATUS = live RTC reader FSM state */
        return (s->regs[R_SLAVE_ADDR1] & ~(0xffu << MEAS_STATUS_S)) |
               ((s->unit[0].fsm & 0xff) << MEAS_STATUS_S);
    case 0x50: /* SAR_TSENS_CTRL: READY/OUT never set (fail closed) */
        return s->regs[R_TSENS_CTRL] & ~(TSENS_OUT_M | TSENS_READY);
    case 0x9C: /* SAR_TOUCH_CHN_ST: no touch scan is ever fabricated */
        return 0;
    case 0xF0: /* SAR_COCPU_INT_ST */
        return s->regs[R_COCPU_INT_RAW] & s->regs[R_COCPU_INT_ENA];
    case 0x1FC: /* SENS_SARDATE */
        return s->regs[R_SARDATE];
    default:
        break;
    }

    if (idx < ARRAY_SIZE(s->regs)) {
        return s->regs[idx];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "esp32s3.sens: read out of range 0x%03x\n",
                  (unsigned)addr);
    return 0;
}

static void esp32s3_sens_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned int size)
{
    ESP32S3SensState *s = ESP32S3_SENS(opaque);
    uint32_t idx = addr >> 2;
    bool prev_start;

    switch (addr) {
    case 0x04: /* RO */
    case 0x28: /* RO */
        return;
    case 0x2C: /* SAR_MEAS2_CTRL1: CNTL_STATE [2:0] is RO */
        s->regs[R_MEAS2_CTRL1] = value & ~MEAS2_CNTL_STATE_M;
        return;
    case 0x40: /* SAR_SLAVE_ADDR1: MEAS_STATUS [29:22] is RO */
        s->regs[R_SLAVE_ADDR1] = value & ~(0xffu << MEAS_STATUS_S);
        return;
    case 0x0C: /* SAR_MEAS1_CTRL2 */
    case 0x30: /* SAR_MEAS2_CTRL2 */ {
        unsigned unit = (addr == 0x30);
        uint32_t reg = unit ? R_MEAS2_CTRL2 : R_MEAS1_CTRL2;

        prev_start = s->regs[reg] & SENS_CTRL2_START;
        /* DATA [15:0] and DONE [16] are RO. */
        s->regs[reg] = (value & ~(SENS_CTRL2_DATA_M | SENS_CTRL2_DONE)) |
                       (s->regs[reg] & (SENS_CTRL2_DATA_M | SENS_CTRL2_DONE));
        if (!prev_start && (value & SENS_CTRL2_START)) {
            sens_start(s, unit);
        }
        if (unit) {
            sens_sar2_armed_update(s);
        }
        return;
    }
    case 0x34: /* SAR_MEAS2_MUX */
        s->regs[R_MEAS2_MUX] = value;
        sens_sar2_armed_update(s);
        return;
    case 0x50: /* SAR_TSENS_CTRL */
        if (!(s->regs[R_TSENS_CTRL] & TSENS_DUMP_OUT) &&
            (value & TSENS_DUMP_OUT)) {
            /* Temperature measurement is unsupported: fail closed (no
             * READY, no OUT).  The regi2c lane observes the named edge. */
            s->diag_unsupported_meas++;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.sens: temperature-sensor measurement is not"
                          " modeled; no READY/OUT will be produced\n");
            qemu_set_irq(s->tsens_dump_irq, 1);
            qemu_set_irq(s->tsens_dump_irq, 0);
        }
        s->regs[R_TSENS_CTRL] = value & ~(TSENS_OUT_M | TSENS_READY);
        return;
    case 0x9C: /* SAR_TOUCH_CHN_ST: RO */
        return;
    case 0xE8: /* SAR_COCPU_INT_RAW: sources (touch/ulp) not modeled; SW W1S */
        s->regs[R_COCPU_INT_RAW] |= value;
        return;
    case 0xEC: /* SAR_COCPU_INT_ENA */
        s->regs[R_COCPU_INT_ENA] = value;
        return;
    case 0xF0: /* RO */
        return;
    case 0xF4: /* SAR_COCPU_INT_CLR */
        s->regs[R_COCPU_INT_RAW] &= ~value;
        return;
    case 0x104: /* SAR_PERI_CLK_GATE_CONF */
        s->regs[R_PERI_CLK_GATE] = value;
        return;
    case 0x108: /* SAR_PERI_RESET_CONF */
        s->regs[R_PERI_RESET] = value;
        if (value & RESET_SARADC) {
            sens_cancel_pause(s);
            /* RTC-side SARADC reset: FSM/results reset, configuration
             * retained (documented profile). */
            for (unsigned u = 0; u < 2; u++) {
                timer_del(s->unit[u].timer);
                s->unit[u].fsm = SAR_FSM_IDLE;
                s->regs[u ? R_MEAS2_CTRL2 : R_MEAS1_CTRL2] &=
                    ~(SENS_CTRL2_DATA_M | SENS_CTRL2_DONE);
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "esp32s3.sens: SAR_PERI_RESET_CONF.SARADC_RESET"
                          " pulsed: reader FSM and results reset\n");
        }
        return;
    case 0x10C: /* SAR_COCPU_INT_ENA_W1TS */
        s->regs[R_COCPU_INT_ENA] |= value;
        return;
    case 0x110: /* SAR_COCPU_INT_ENA_W1TC */
        s->regs[R_COCPU_INT_ENA] &= ~value;
        return;
    case 0x1FC: /* RO */
        return;
    default:
        break;
    }

    if (idx == R_POWER_XPD_SAR && s->regs[idx] != (uint32_t)value) {
        sens_cancel_pause(s);
    }
    if (idx < ARRAY_SIZE(s->regs)) {
        s->regs[idx] = value;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32s3.sens: write out of range 0x%03x\n",
                      (unsigned)addr);
    }
}

static const MemoryRegionOps esp32s3_sens_ops = {
    .read = esp32s3_sens_read,
    .write = esp32s3_sens_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void esp32s3_sens_reset_hold(Object *obj, ResetType type)
{
    ESP32S3SensState *s = ESP32S3_SENS(obj);

    sens_cancel_pause(s);
    memset(s->regs, 0, sizeof(s->regs));

    /* TRM reset defaults (sens_reg.h). */
    s->regs[R_READER1_CTRL] = BIT(29) | BIT(18) | 2; /* int_en, gated, div=2 */
    s->regs[R_READER2_CTRL] = BIT(30) | BIT(18) | 2;
    s->regs[R_MEAS2_CTRL1] = (7u << 24) | (2u << 16) | (2u << 8);
    s->regs[R_SARDATE] = 0x02101180u & 0x0fffffffu;

    for (unsigned u = 0; u < 2; u++) {
        timer_del(s->unit[u].timer);
        s->unit[u].fsm = SAR_FSM_IDLE;
        s->unit[u].channel = 0;
        s->unit[u].atten = 0;
        s->unit[u].code = 0;
        s->unit[u].arbiter_denied = false;
        s->unit[u].arb_extra = 0;
    }
    s->diag_invalid_sample = 0;
    s->diag_floating_sample = 0;
    s->diag_unpowered_sample = 0;
    s->diag_digital_owned_sample = 0;
    s->diag_unknown_sample = 0;
    s->diag_gate_blocked = 0;
    s->diag_unsupported_meas = 0;
    s->diag_arbiter_denied = 0;
    s->diag_ulp_ignored = 0;
    s->diag_dig_force_ignored = 0;
    qemu_set_irq(s->sar2_armed_irq, 0);
    qemu_set_irq(s->tsens_dump_irq, 0);
}

static void esp32s3_sens_init(Object *obj)
{
    ESP32S3SensState *s = ESP32S3_SENS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    s->pause_bh = qemu_bh_new(sens_pause_bh, s);

    memory_region_init_io(&s->iomem, obj, &esp32s3_sens_ops, s,
                          TYPE_ESP32S3_SENS, SENS_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_out_named(DEVICE(obj), &s->sar2_armed_irq, "sar2-armed", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->tsens_dump_irq,
                             "tsens-dump-out", 1);

    for (unsigned u = 0; u < 2; u++) {
        s->unit[u].index = u;
        s->unit[u].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sens_timer_trampoline,
                                        &s->unit[u]);
    }

    s->apb_clk = qdev_init_clock_in(DEVICE(obj), "apb-clk", NULL, s,
                                    ClockUpdate);
    s->rtc_fast_clk = qdev_init_clock_in(DEVICE(obj), "rtc-fast-clk", NULL, s,
                                         ClockUpdate);
}

static void esp32s3_sens_finalize(Object *obj)
{
    ESP32S3SensState *s = ESP32S3_SENS(obj);

    sens_cancel_pause(s);
    qemu_bh_delete(s->pause_bh);
    for (unsigned u = 0; u < 2; u++) {
        timer_free(s->unit[u].timer);
    }
}

static void sens_timer_trampoline(void *opaque)
{
    ESP32S3SarUnit *u = opaque;
    ESP32S3SensState *s = container_of(u, ESP32S3SensState, unit[u->index]);

    sens_phase_advance(s, u->index);
}

static void esp32s3_sens_realize(DeviceState *dev, Error **errp)
{
    ESP32S3SensState *s = ESP32S3_SENS(dev);

    if (s->apb_saradc == NULL) {
        error_setg(errp, "esp32s3.sens: link 'apb-saradc' is required");
        return;
    }
    if (s->provider) {
        Esp32S3AdcSampleProviderClass *pc =
            ESP32S3_ADC_SAMPLE_PROVIDER_GET_CLASS(s->provider);

        if (!pc->sample || !pc->source_generation) {
            error_setg(errp, "esp32s3.sens: sample-provider requires sample"
                       " and source_generation methods");
            return;
        }
    }
}

static const Property esp32s3_sens_properties[] = {
    /*
     * "sample-provider" is the canonical typed link, wired BEFORE realize
     * (-global long form or machine code) to a real provider object
     * (user-creatable -object or the net-solver device).  Physical source
     * changes at runtime go through the provider's own mutable properties
     * (rail-mv, float-net, digital-owned, powered), never by re-pointing
     * the ADC.
     */
    DEFINE_PROP_LINK("sample-provider", ESP32S3SensState, provider,
                     TYPE_ESP32S3_ADC_SAMPLE_PROVIDER,
                     Esp32S3AdcSampleProvider *),
    DEFINE_PROP_LINK("apb-saradc", ESP32S3SensState, apb_saradc,
                     TYPE_ESP32S3_APB_SARADC, ESP32S3ApbSaradcState *),
    DEFINE_PROP_LINK("regi2c", ESP32S3SensState, regi2c,
                     "esp32s3.regi2c", DeviceState *),
    DEFINE_PROP_BOOL("strict-invalid-sample", ESP32S3SensState,
                     strict_invalid_sample, false),
    DEFINE_PROP_UINT64("diag-invalid-sample", ESP32S3SensState,
                       diag_invalid_sample, 0),
    DEFINE_PROP_UINT64("diag-floating-sample", ESP32S3SensState,
                       diag_floating_sample, 0),
    DEFINE_PROP_UINT64("diag-unpowered-sample", ESP32S3SensState,
                       diag_unpowered_sample, 0),
    DEFINE_PROP_UINT64("diag-digital-owned-sample", ESP32S3SensState,
                       diag_digital_owned_sample, 0),
    DEFINE_PROP_UINT64("diag-unknown-sample", ESP32S3SensState,
                       diag_unknown_sample, 0),
    DEFINE_PROP_UINT64("diag-gate-blocked", ESP32S3SensState,
                       diag_gate_blocked, 0),
    DEFINE_PROP_UINT64("diag-unsupported-meas", ESP32S3SensState,
                       diag_unsupported_meas, 0),
    DEFINE_PROP_UINT64("diag-arbiter-denied", ESP32S3SensState,
                       diag_arbiter_denied, 0),
    DEFINE_PROP_UINT64("diag-ulp-ignored", ESP32S3SensState,
                       diag_ulp_ignored, 0),
    DEFINE_PROP_UINT64("diag-dig-force-ignored", ESP32S3SensState,
                       diag_dig_force_ignored, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_sens_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_sens_reset_hold;
    dc->realize = esp32s3_sens_realize;
    device_class_set_props(dc, esp32s3_sens_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo esp32s3_sens_info = {
    .name          = TYPE_ESP32S3_SENS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3SensState),
    .instance_init = esp32s3_sens_init,
    .instance_finalize = esp32s3_sens_finalize,
    .class_init    = esp32s3_sens_class_init,
};

static void esp32s3_sens_register_types(void)
{
    type_register_static(&esp32s3_sens_info);
}

type_init(esp32s3_sens_register_types)
