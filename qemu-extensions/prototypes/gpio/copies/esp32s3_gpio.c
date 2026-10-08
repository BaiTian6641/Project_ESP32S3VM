/*
 * ESP32-S3 GPIO emulation with GPIO matrix routing
 *
 * Implements the GPIO register model for ESP32-S3 per the TRM chapter 6
 * (IO MUX and GPIO Matrix), re-derived from the TRM and the ESP-IDF 6.1 SoC
 * headers:
 *  - Output data (GPIO_OUT/OUT1) with W1TS/W1TC atomic set/clear
 *  - Output enable (GPIO_ENABLE/ENABLE1) with W1TS/W1TC
 *  - Input registers (GPIO_IN/IN1) sampling the resolved pad levels
 *  - Per-pin configuration (GPIO_PINn: pad driver, interrupt type, INT_ENA)
 *  - GPIO matrix: 256 input selectors (GPIO_FUNCn_IN_SEL_CFG) and 49 output
 *    selectors (GPIO_FUNCm_OUT_SEL_CFG, signal 256 = simple GPIO output)
 *  - Interrupt status (GPIO_STATUS/STATUS1 with W1TS/W1TC) with edge/level
 *    detection per GPIO_PINn.INT_TYPE and per-CPU/NMI INT_ENA lanes routed
 *    to the four GPIO interrupt matrix sources (16..19)
 *
 * Electrical model (digital, pre-analog-gate):
 *  - Every pad has an external input line (named "gpio-in") and an output
 *    line ("gpio-out") carrying the resolved pad node level.
 *  - A pad drives its node when the IO_MUX function is GPIO (MCU_SEL = 0)
 *    and the output enable is active; open-drain pads release on output 1.
 *  - A released pad node reads the external driver level; with no external
 *    driver, the IO_MUX pull-up/down registers resolve the level
 *    (FUN_PU -> 1, FUN_PD -> 0, floating reads 0).
 *  - GPIO_IN samples the node through the input buffer (FUN_IE).
 *  - Drive strength (FUN_DRV) is stored but electrically inert until the
 *    analog gate; contention with an external push-pull driver resolves to
 *    the internal pad value.  Both boundaries are documented in the README.
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#if defined(__has_include)
#if __has_include("hw/xtensa/esp32s3_reset_domain.h")
#include "hw/xtensa/esp32s3_reset_domain.h"
#define ESP32S3_GPIO_RESET_DOMAIN 1
#endif
#endif
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/misc/esp32s3_rtc_cntl.h"

#define GPIO_WARNING 0

static void esp32s3_gpio_update_irq(ESP32S3GPIOState *s);
static void esp32s3_gpio_notify_observer(ESP32S3GPIOState *s);
static void esp32s3_gpio_unknown_use(ESP32S3GPIOState *s, int n);

/* ---------- Register-file helpers ---------- */

static inline uint32_t pin_field(ESP32S3GPIOState *s, int n, int shift, int width)
{
    return extract32(s->gpio_pin[n], shift, width);
}

static inline int pin_int_type(ESP32S3GPIOState *s, int n)
{
    return pin_field(s, n, R_GPIO_PINn_INT_TYPE_SHIFT, R_GPIO_PINn_INT_TYPE_LENGTH);
}

static inline int pin_int_ena(ESP32S3GPIOState *s, int n)
{
    return pin_field(s, n, R_GPIO_PINn_INT_ENA_SHIFT, R_GPIO_PINn_INT_ENA_LENGTH);
}

static inline int pin_open_drain(ESP32S3GPIOState *s, int n)
{
    return pin_field(s, n, R_GPIO_PINn_PAD_DRIVER_SHIFT, 1);
}

static inline int status_bit(ESP32S3GPIOState *s, int n)
{
    return n < 32 ? (s->gpio_status >> n) & 1 : (s->gpio_status1 >> (n - 32)) & 1;
}

static inline void set_status_bit(ESP32S3GPIOState *s, int n, int level)
{
    if (n < 32) {
        s->gpio_status = deposit32(s->gpio_status, n, 1, level);
    } else {
        s->gpio_status1 = deposit32(s->gpio_status1, n - 32, 1, level);
    }
}

static inline uint32_t in_bit(ESP32S3GPIOState *s, int n)
{
    return n < 32 ? (s->gpio_out >> n) & 1 : (s->gpio_out1 >> (n - 32)) & 1;
}

static inline uint32_t enable_bit(ESP32S3GPIOState *s, int n)
{
    return n < 32 ? (s->gpio_enable >> n) & 1 : (s->gpio_enable1 >> (n - 32)) & 1;
}

/* ---------- Pad resolution ---------- */

/* True when the pad node follows the GPIO matrix / simple GPIO output */
static bool esp32s3_gpio_matrix_active(ESP32S3GPIOState *s, int n)
{
    if (s->iomux == NULL) {
        return true;
    }
    /* MCU_SEL == PIN_FUNC_GPIO (1 on the S3) routes the pad through the
     * GPIO matrix; any other value selects a direct IO_MUX function, and
     * none of those peripherals is modelled, so the pad driver stays
     * released. */
    return FIELD_EX32(esp32s3_iomux_get(s->iomux, n), IO_MUX_GPIOn, MCU_SEL)
           == ESP32S3_IOMUX_MCU_SEL_GPIO;
}

static int esp32s3_gpio_input_enable(ESP32S3GPIOState *s, int n)
{
    /* Pads 22-25 are unpopulated on the ESP32-S3 chip: there is no pad,
     * the input buffer never drives the bank and reads return 0 with no
     * uncertainty (documented hardware read semantics, not a floating
     * indeterminate). */
    if (n >= 22 && n <= 25) {
        return 0;
    }
    if (s->iomux == NULL) {
        return 1;
    }
    return FIELD_EX32(esp32s3_iomux_get(s->iomux, n), IO_MUX_GPIOn, FUN_IE);
}

/*
 * Sleep-mode effective configuration (pad-hold-sleep-semantics): when
 * the RTC domain latched SLEEP_EN and the pad has SLP_SEL set, the
 * IO_MUX SLP_* set replaces the awake FUN_* controls.  Otherwise the
 * awake configuration stays fully effective (IDF normal-startup SLP
 * configuration writes are raw state, not an active sleep). */
static bool esp32s3_gpio_sleep_effective(ESP32S3GPIOState *s, int n)
{
    uint32_t mux;

    if (s->rtc_cntl == NULL || !s->rtc_cntl->sleep_requested) {
        return false;
    }
    if (s->iomux == NULL) {
        return false;
    }
    mux = esp32s3_iomux_get(s->iomux, n);
    return FIELD_EX32(mux, IO_MUX_GPIOn, SLP_SEL) != 0;
}

static int esp32s3_gpio_eff_ie(ESP32S3GPIOState *s, int n)
{
    uint32_t mux;

    if (esp32s3_gpio_sleep_effective(s, n)) {
        mux = esp32s3_iomux_get(s->iomux, n);
        return FIELD_EX32(mux, IO_MUX_GPIOn, SLP_IE);
    }
    return esp32s3_gpio_input_enable(s, n);
}

/*
 * Resolve the pad net: the own output driver wins; otherwise an external
 * (solver or qdev) driver; otherwise the IO_MUX pulls; otherwise the net
 * is unresolved (floating).  Floating is NOT valid low — it is reported
 * through ESP32S3_GPIO_PAD_FLOATING and only the documented permissive
 * sampling rule maps it to a 0 register bit.
 */
static void esp32s3_gpio_resolve_pad(ESP32S3GPIOState *s, int n)
{
    uint32_t out_sel_cfg = s->func_out_sel_cfg[n];
    uint32_t out_sel = FIELD_EX32(out_sel_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_SEL);
    uint32_t out_val, oe;
    ESP32S3GpioNet *net = &s->net[n];

    /* Pad hold (RTC_CNTL hold registers, TRM ch.6.9): the frozen DRIVE
     * configuration latched at hold-set is authoritative for the output
     * side; the input/net validity is NOT mutated (solver-owned pads
     * keep solver validity even held; a released held pad follows the
     * net; a genuinely floating held pad stays UNKNOWN under strict
     * profiles).  A latched PERIPHERAL route cannot be re-driven without
     * its source model: strict-unsupported (consumption pauses under
     * strict profiles). */
    if (s->hold_active[n]) {
        if (s->hold_matrix_routed[n]) {
            net->valid = false;
            net->diag = ESP32S3_GPIO_PAD_UNKNOWN;
            return;
        }
        if (s->hold_oe[n] && !(s->hold_od[n] && s->hold_level[n] == 0)) {
            net->valid = true;
            net->level = s->hold_level[n];
            net->diag = (s->net[n].valid &&
                         s->net[n].diag == ESP32S3_GPIO_PAD_EXTERNAL &&
                         s->net[n].level != s->hold_level[n])
                        ? ESP32S3_GPIO_PAD_CONTENTION
                        : ESP32S3_GPIO_PAD_DRIVEN;
            return;
        }
        /* Held released: external/pulls/floating as usual (fall through) */
    }

    /* S3 FUNC_OEN_SEL: 1 = OE from GPIO_ENABLE, 0 = OE from the routed
     * peripheral output signal (IDF gpio_ll output_enable ctrl_by_periph
     * writes !ctrl).  On the simple-GPIO signal (256) the OE source is
     * hardwired to GPIO_ENABLE, so OEN_SEL does not apply there. */
    if (out_sel == GPIO_FUNC_OUT_SEL_NONE) {
        /* Simple GPIO output driven from GPIO_OUT / GPIO_ENABLE */
        out_val = in_bit(s, n);
        oe = enable_bit(s, n);
    } else if (out_sel <= GPIO_FUNC_OUT_SEL_INVALID_MAX) {
        /* Peripheral matrix output signal: no model sources any signal
         * yet, so the value is 0 and the signal OE (OEN_SEL = 0) releases
         * the pad; OEN_SEL = 1 hands OE to GPIO_ENABLE. */
        out_val = 0;
        oe = FIELD_EX32(out_sel_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_SEL) ?
             enable_bit(s, n) : 0;
    } else {
        /* Reserved selector values (257..511): pad not driven */
        out_val = 0;
        oe = 0;
    }

    if (FIELD_EX32(out_sel_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_INV_SEL)) {
        out_val ^= 1;
    }
    if (FIELD_EX32(out_sel_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_INV_SEL)) {
        oe ^= 1;
    }

    if (esp32s3_gpio_matrix_active(s, n) && oe &&
        !(pin_open_drain(s, n) && out_val == 1)) {
        bool attached = s->solver_owned || s->solver_attached[n];

        if (attached && net->valid && net->diag == ESP32S3_GPIO_PAD_EXTERNAL) {
            /* Attached solver level is authoritative over the internal
             * output (finite driver contention can resolve the opposite
             * level); CONTENTION reports raw-desired vs solved sample. */
            net->diag = (net->level != out_val) ?
                        ESP32S3_GPIO_PAD_CONTENTION : ESP32S3_GPIO_PAD_DRIVEN;
            net->valid = true;
            return;
        }
        net->diag = ESP32S3_GPIO_PAD_DRIVEN;
        net->valid = true;
        net->level = out_val;
        return;
    }

    /* Released pad: an external driver beats the weak pulls (a pull is
     * never an unconditional line level against a conflicting driver).
     * While the solver owns the pad, its validity report owns the answer:
     * an unresolved report stays UNKNOWN and never falls back to a pull
     * or an implicit floating-0 as if it were a valid sample. */
    if (net->valid && net->diag == ESP32S3_GPIO_PAD_EXTERNAL) {
        return;
    }
    if (s->solver_owned || s->solver_attached[n]) {
        net->valid = false;
        net->level = 0;
        net->diag = ESP32S3_GPIO_PAD_UNKNOWN;
        return;
    }
    if (s->iomux != NULL) {
        uint32_t pad = esp32s3_iomux_get(s->iomux, n);
        /* Effective pulls: awake FUN_PU/FUN_PD, or the SLP_* set when
         * the RTC domain latched sleep and the pad selects it. */
        int pu = (pad & R_IO_MUX_GPIOn_SLP_PU_MASK) != 0;
        int pd = (pad & R_IO_MUX_GPIOn_SLP_PD_MASK) != 0;
        if (!esp32s3_gpio_sleep_effective(s, n)) {
            pu = (pad & R_IO_MUX_GPIOn_FUN_PU_MASK) != 0;
            pd = (pad & R_IO_MUX_GPIOn_FUN_PD_MASK) != 0;
        }
        if (pu) {
            net->valid = true;
            net->level = 1;
            net->diag = ESP32S3_GPIO_PAD_PULLED_HIGH;
            return;
        }
        if (pd) {
            net->valid = true;
            net->level = 0;
            net->diag = ESP32S3_GPIO_PAD_PULLED_LOW;
            return;
        }
    }
    /* Floating: no driver, no pull — unresolved (0 V equivalent, not
     * valid low) */
    net->valid = false;
    net->level = 0;
    net->diag = ESP32S3_GPIO_PAD_FLOATING;
}

/* Emit the pad level on the observation line (digital view of the net) */
static void esp32s3_gpio_emit_line(ESP32S3GPIOState *s, int n)
{
    int level = s->net[n].valid ? s->net[n].level : 0;

    if (s->line_out[n] != level) {
        s->line_out[n] = level;
        qemu_set_irq(s->output_lines[n], level);
    }
}

/* Sample the input buffer, run edge/level interrupt detection for one pad */
static void esp32s3_gpio_update_pad(ESP32S3GPIOState *s, int n)
{
    int sample;
    int prev = s->sample[n];
    int type = pin_int_type(s, n);
    int event = 0;

    esp32s3_gpio_resolve_pad(s, n);
    esp32s3_gpio_emit_line(s, n);

    /* Input buffer: FUN_IE gates the sample.  An attached-solver pad
     * samples the solver level while valid, and holds its last sample
     * while the solver reports UNKNOWN (persisted rule — never a
     * fabricated 0).  A detached floating net samples 0 under the
     * documented permissive profile; esp32s3_gpio_pad_state() exposes
     * the unresolved state either way. */
    bool strict = s->strict_unknown || s->solver_owned || s->solver_attached[n];

    if (s->hold_active[n] && s->hold_oe[n] && !s->hold_matrix_routed[n]) {
        sample = esp32s3_gpio_input_enable(s, n) ? s->hold_level[n] : 0;
    } else if (!esp32s3_gpio_input_enable(s, n)) {
        sample = 0;
    } else if (s->net[n].valid) {
        sample = s->net[n].level;
    } else if (strict) {
        /* Passive sampling records the persisted value but does NOT
         * pause — the strict gate fires only at real consumption
         * (MMIO IN read, IRQ qualification, matrix_sample). */
        sample = s->sample[n];
    } else {
        sample = 0;
    }

    /* Hold transitions (RTC_CNTL hold registers via the rtc-cntl link):
     * latch the CURRENT resolved level when the hold bit turns on;
     * release resumes normal resolution. */
    {
        bool held_now = false;
        if (s->rtc_cntl != NULL) {
            if (n < 32) {
                held_now = (s->rtc_cntl->dig_pad_hold >> n) & 1u;
            }
            if (n < 22) {
                held_now |= (s->rtc_cntl->rtc_pad_hold >> n) & 1u;
            }
        }
        if (held_now && !s->hold_active[n]) {
            uint32_t osc = s->func_out_sel_cfg[n];

            /* Latch the DESIRED output config (raw register truth) */
            s->hold_level[n] = in_bit(s, n);
            s->hold_oe[n] = enable_bit(s, n);
            s->hold_od[n] = pin_open_drain(s, n);
            s->hold_pull_up[n] = s->iomux != NULL &&
                FIELD_EX32(esp32s3_iomux_get(s->iomux, n), IO_MUX_GPIOn, FUN_PU);
            s->hold_pull_down[n] = s->iomux != NULL &&
                FIELD_EX32(esp32s3_iomux_get(s->iomux, n), IO_MUX_GPIOn, FUN_PD);
            s->hold_matrix_routed[n] =
                FIELD_EX32(osc, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_SEL) !=
                GPIO_FUNC_OUT_SEL_NONE;
        }
        s->hold_active[n] = held_now;
    }

    switch (type) {
    case ESP32S3_GPIO_INT_RISING:
        event = !prev && sample;
        break;
    case ESP32S3_GPIO_INT_FALLING:
        event = prev && !sample;
        break;
    case ESP32S3_GPIO_INT_ANYEDGE:
        event = prev != sample;
        break;
    case ESP32S3_GPIO_INT_LOW:
        /* Level interrupt: re-asserted while the condition holds */
        event = !sample;
        break;
    case ESP32S3_GPIO_INT_HIGH:
        event = sample;
        break;
    default:
        event = 0;
        break;
    }

    if (event) {
        set_status_bit(s, n, 1);
    }
    s->sample[n] = sample;
}

static void esp32s3_gpio_update_all(ESP32S3GPIOState *s)
{
    for (int n = 0; n < ESP32S3_GPIO_COUNT; n++) {
        esp32s3_gpio_update_pad(s, n);
    }
    esp32s3_gpio_update_irq(s);
}

/* ---------- Interrupt lanes ---------- */

static void esp32s3_gpio_update_irq(ESP32S3GPIOState *s)
{
    int lane[4] = { 0, 0, 0, 0 };

    for (int n = 0; n < ESP32S3_GPIO_COUNT; n++) {
        int ena = pin_int_ena(s, n);
        int st = status_bit(s, n);

        lane[0] |= (st && (ena & 1)); /* PROCPU level  (source 16) */
        lane[1] |= (st && (ena & 4)); /* PROCPU NMI    (source 17) */
        lane[2] |= (st && (ena & 2)); /* APPCPU level  (source 18) */
        lane[3] |= (st && (ena & 8)); /* APPCPU NMI    (source 19) */
    }

    for (int n = 0; n < ESP32S3_GPIO_COUNT; n++) {
        if ((s->strict_unknown || s->solver_owned || s->solver_attached[n])
            && !s->net[n].valid && esp32s3_gpio_eff_ie(s, n)
            && pin_int_ena(s, n)) {
            esp32s3_gpio_unknown_use(s, n);
        }
    }
    qemu_set_irq(s->parent.irq, lane[0]);
    qemu_set_irq(s->irq_nmi, lane[1]);
    qemu_set_irq(s->irq_app, lane[2]);
    qemu_set_irq(s->irq_app_nmi, lane[3]);
}

/* Masked status readback for one INT_ENA field bit (0..4) */
static uint32_t esp32s3_gpio_masked_status(ESP32S3GPIOState *s, int bank1, int bit)
{
    uint32_t r = 0;

    for (int n = 0; n < ESP32S3_GPIO_COUNT; n++) {
        if (((n < 32) != !bank1)) {
            continue;
        }
        if (status_bit(s, n) && (pin_int_ena(s, n) & (1u << bit))) {
            r |= 1u << (bank1 ? n - 32 : n);
        }
    }
    return r;
}

/* Level conditions currently true (GPIO_STATUS_NEXT) */
static uint32_t esp32s3_gpio_status_next(ESP32S3GPIOState *s, int bank1)
{
    uint32_t r = 0;

    for (int n = bank1 ? 32 : 0; n < (bank1 ? ESP32S3_GPIO_COUNT : 32); n++) {
        int sample = s->sample[n];
        int type = pin_int_type(s, n);

        if (((type == ESP32S3_GPIO_INT_LOW) && !sample) ||
            ((type == ESP32S3_GPIO_INT_HIGH) && sample)) {
            r |= 1u << (n - 32 * bank1);
        }
    }
    return r;
}

/* ---------- GPIO matrix input lookup ---------- */

uint32_t esp32s3_gpio_matrix_input(ESP32S3GPIOState *s, uint32_t signal)
{
    uint32_t cfg;
    uint32_t sel, val;

    if (signal >= ESP32S3_GPIO_FUNC_IN_SEL_COUNT) {
        return 0;
    }
    cfg = s->func_in_sel_cfg[signal];
    if (!FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, SIG_IN_SEL)) {
        /* Bypass: the signal is taken from a direct IO_MUX function; no
         * direct IO_MUX input path is modelled, so it reads 0. */
        return 0;
    }
    sel = FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_SEL);
    if (sel == GPIO_FUNC_IN_HIGH) {
        val = 1;
    } else if (sel == GPIO_FUNC_IN_LOW) {
        val = 0;
    } else if (sel < ESP32S3_GPIO_COUNT) {
        val = s->sample[sel];
    } else {
        esp32s3_gpio_unknown_use(s, (int)sel);
        val = 0;
    }
    if (FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_INV_SEL)) {
        val ^= 1;
    }
    return val;
}

bool esp32s3_gpio_matrix_sample(ESP32S3GPIOState *s, uint32_t signal,
                                bool *level)
{
    uint32_t cfg, sel;

    if (signal >= ESP32S3_GPIO_FUNC_IN_SEL_COUNT || level == NULL) {
        return false;
    }
    cfg = s->func_in_sel_cfg[signal];
    if (!FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, SIG_IN_SEL)) {
        /* Bypass: direct IO_MUX route is not modelled — unresolved */
        return false;
    }
    sel = FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_SEL);
    if (sel == GPIO_FUNC_IN_HIGH) {
        *level = true;
    } else if (sel == GPIO_FUNC_IN_LOW) {
        *level = false;
    } else if (sel < ESP32S3_GPIO_COUNT) {
        /* The input buffer gates the sample: with FUN_IE disabled the
         * routed pad reads unresolved (never a fabricated 0). */
        if (!esp32s3_gpio_input_enable(s, (int)sel)) {
            return false;
        }
        if ((s->strict_unknown || s->solver_owned || s->solver_attached[sel])
            && !s->net[sel].valid && esp32s3_gpio_eff_ie(s, (int)sel)) {
            esp32s3_gpio_unknown_use(s, (int)sel);
        }
        *level = s->sample[sel] != 0;
    } else {
        return false;
    }
    if (FIELD_EX32(cfg, GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_INV_SEL)) {
        *level = !*level;
    }
    return true;
}

/* ---------- Register read ---------- */

static uint64_t esp32s3_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);
    uint64_t r = 0;
    hwaddr a = addr;
    int n;

    /* Per-pin config registers: 0x074 + n*4 for n=0..48 */
    if (a >= GPIO_PINn_REG_OFFSET(0) && a < GPIO_PINn_REG_OFFSET(ESP32S3_GPIO_COUNT)) {
        n = (a - GPIO_PINn_REG_OFFSET(0)) / 4;
        return s->gpio_pin[n];
    }

    /* Input signal matrix: 0x154 + n*4 for n=0..255 */
    if (a >= GPIO_FUNC_IN_SEL_CFG_OFFSET(0) &&
        a < GPIO_FUNC_IN_SEL_CFG_OFFSET(ESP32S3_GPIO_FUNC_IN_SEL_COUNT)) {
        n = (a - GPIO_FUNC_IN_SEL_CFG_OFFSET(0)) / 4;
        return s->func_in_sel_cfg[n];
    }

    /* Output signal matrix: 0x554 + n*4 for n=0..48 */
    if (a >= GPIO_FUNC_OUT_SEL_CFG_OFFSET(0) &&
        a < GPIO_FUNC_OUT_SEL_CFG_OFFSET(ESP32S3_GPIO_COUNT)) {
        n = (a - GPIO_FUNC_OUT_SEL_CFG_OFFSET(0)) / 4;
        return s->func_out_sel_cfg[n];
    }

    switch (a) {
    case A_GPIO_BT_SELECT:
        r = s->bt_select;
        break;
    case A_GPIO_OUT:
        r = s->gpio_out;
        break;
    case A_GPIO_OUT_W1TS:
    case A_GPIO_OUT_W1TC:
        r = 0; /* write-only */
        break;
    case A_GPIO_OUT1:
        r = s->gpio_out1;
        break;
    case A_GPIO_OUT1_W1TS:
    case A_GPIO_OUT1_W1TC:
        r = 0;
        break;

    case A_GPIO_SDIO_SELECT:
        r = s->sdio_select;
        break;

    case A_GPIO_ENABLE:
        r = s->gpio_enable;
        break;
    case A_GPIO_ENABLE_W1TS:
    case A_GPIO_ENABLE_W1TC:
        r = 0;
        break;
    case A_GPIO_ENABLE1:
        r = s->gpio_enable1;
        break;
    case A_GPIO_ENABLE1_W1TS:
    case A_GPIO_ENABLE1_W1TC:
        r = 0;
        break;

    case A_GPIO_STRAP:
        r = s->parent.strap_mode;
        break;

    case A_GPIO_IN:
    case A_GPIO_IN1:
        /* Bank read = input sampling event (TRM: GPIO_IN reflects the
         * sampled pad levels): re-sample all pads first so sleep/hold/
         * solver state changes since the last write are visible. */
        esp32s3_gpio_update_all(s);
        /* Strict gate at actual guest consumption: an UNKNOWN sample on
         * a solver-owned pad schedules the paused dependency and, from
         * vCPU context, aborts this access so the guest retries instead
         * of consuming a stale value as valid. */
        for (n = (a == A_GPIO_IN) ? 0 : 32;
             n < ((a == A_GPIO_IN) ? 32 : ESP32S3_GPIO_COUNT); n++) {
            if ((s->strict_unknown || s->solver_owned || s->solver_attached[n])
                && !s->net[n].valid && esp32s3_gpio_input_enable(s, n)) {
                /* Synchronous hook: the native owner may abort/retry this
                 * access from inside the handler; the default handler
                 * schedules the main-context paused dependency instead. */
                esp32s3_gpio_unknown_use(s, n);
            }
        }
        r = 0;
        for (n = 0; n < 32; n++) {
            r |= (uint32_t)s->sample[n] << n;
        }
        if (a == A_GPIO_IN1) {
            r = 0;
            for (n = 32; n < ESP32S3_GPIO_COUNT; n++) {
                r |= (uint32_t)s->sample[n] << (n - 32);
            }
        }
        break;

    case A_GPIO_STATUS:
        r = s->gpio_status;
        break;
    case A_GPIO_STATUS_W1TS:
    case A_GPIO_STATUS_W1TC:
        r = 0;
        break;
    case A_GPIO_STATUS1:
        r = s->gpio_status1;
        break;
    case A_GPIO_STATUS1_W1TS:
    case A_GPIO_STATUS1_W1TC:
        r = 0;
        break;

    /* Per-CPU interrupt status (read-only masked views: PROCPU level / NMI
     * and SDIO lanes, low and high banks; the APPCPU/NMI lanes assert
     * matrix sources 18/19 directly and have no status register) */
    case A_GPIO_PCPU_INT:
        r = esp32s3_gpio_masked_status(s, 0, 0);
        break;
    case A_GPIO_PCPU_NMI_INT:
        r = esp32s3_gpio_masked_status(s, 0, 2);
        break;
    case A_GPIO_CPUSDIO_INT:
        r = esp32s3_gpio_masked_status(s, 0, 4);
        break;
    case A_GPIO_PCPU_INT1:
        r = esp32s3_gpio_masked_status(s, 1, 0);
        break;
    case A_GPIO_PCPU_NMI_INT1:
        r = esp32s3_gpio_masked_status(s, 1, 2);
        break;
    case A_GPIO_CPUSDIO_INT1:
        r = esp32s3_gpio_masked_status(s, 1, 4);
        break;

    case A_GPIO_STATUS_NEXT:
        r = s->gpio_status | esp32s3_gpio_status_next(s, 0);
        break;
    case A_GPIO_STATUS_NEXT1:
        r = s->gpio_status1 | esp32s3_gpio_status_next(s, 1);
        break;

    case A_GPIO_CLOCK_GATE:
        r = s->clock_gate;
        break;
    case A_GPIO_DATE:
        r = s->date_reg;
        break;

    default:
        if (GPIO_WARNING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad read at offset 0x%04" HWADDR_PRIx "\n",
                          __func__, a);
        }
        break;
    }

    return r;
}

/* ---------- Register write ---------- */

static void esp32s3_gpio_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned int size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);
    hwaddr a = addr;
    int n;

    /* Per-pin config registers */
    if (a >= GPIO_PINn_REG_OFFSET(0) && a < GPIO_PINn_REG_OFFSET(ESP32S3_GPIO_COUNT)) {
        n = (a - GPIO_PINn_REG_OFFSET(0)) / 4;
        s->gpio_pin[n] = (uint32_t)value & ESP32S3_GPIO_PINn_WRITE_MASK;
        esp32s3_gpio_notify_observer(s);
        esp32s3_gpio_update_pad(s, n);
        esp32s3_gpio_update_irq(s);
        return;
    }

    /* Input signal matrix */
    if (a >= GPIO_FUNC_IN_SEL_CFG_OFFSET(0) &&
        a < GPIO_FUNC_IN_SEL_CFG_OFFSET(ESP32S3_GPIO_FUNC_IN_SEL_COUNT)) {
        n = (a - GPIO_FUNC_IN_SEL_CFG_OFFSET(0)) / 4;
        s->func_in_sel_cfg[n] = (uint32_t)value & GPIO_FUNC_IN_SEL_WRITE_MASK;
        return;
    }

    /* Output signal matrix */
    if (a >= GPIO_FUNC_OUT_SEL_CFG_OFFSET(0) &&
        a < GPIO_FUNC_OUT_SEL_CFG_OFFSET(ESP32S3_GPIO_COUNT)) {
        n = (a - GPIO_FUNC_OUT_SEL_CFG_OFFSET(0)) / 4;
        s->func_out_sel_cfg[n] = (uint32_t)value & GPIO_FUNC_OUT_SEL_CFG_WRITE_MASK;
        esp32s3_gpio_notify_observer(s);
        esp32s3_gpio_update_pad(s, n);
        esp32s3_gpio_update_irq(s);
        return;
    }

    switch (a) {
    case A_GPIO_BT_SELECT:
        s->bt_select = (uint32_t)value & 0xFF;
        break;
    case A_GPIO_OUT:
        s->gpio_out = (uint32_t)value;
        break;
    case A_GPIO_OUT_W1TS:
        s->gpio_out |= (uint32_t)value;
        break;
    case A_GPIO_OUT_W1TC:
        s->gpio_out &= ~(uint32_t)value;
        break;
    case A_GPIO_OUT1:
        s->gpio_out1 = (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_OUT1_W1TS:
        s->gpio_out1 |= (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_OUT1_W1TC:
        s->gpio_out1 &= ~((uint32_t)value & ESP32S3_GPIO1_MASK);
        break;

    case A_GPIO_SDIO_SELECT:
        s->sdio_select = (uint32_t)value & 0xFF;
        break;

    case A_GPIO_ENABLE:
        s->gpio_enable = (uint32_t)value;
        break;
    case A_GPIO_ENABLE_W1TS:
        s->gpio_enable |= (uint32_t)value;
        break;
    case A_GPIO_ENABLE_W1TC:
        s->gpio_enable &= ~(uint32_t)value;
        break;
    case A_GPIO_ENABLE1:
        s->gpio_enable1 = (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_ENABLE1_W1TS:
        s->gpio_enable1 |= (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_ENABLE1_W1TC:
        s->gpio_enable1 &= ~((uint32_t)value & ESP32S3_GPIO1_MASK);
        break;

    case A_GPIO_STATUS:
        /* Direct write forces/clears status the same way the W1TS/W1TC
         * registers do: 1s set, 0s are ignored. */
        s->gpio_status |= (uint32_t)value;
        break;
    case A_GPIO_STATUS_W1TS:
        s->gpio_status |= (uint32_t)value;
        break;
    case A_GPIO_STATUS_W1TC:
        s->gpio_status &= ~(uint32_t)value;
        break;
    case A_GPIO_STATUS1:
        s->gpio_status1 |= (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_STATUS1_W1TS:
        s->gpio_status1 |= (uint32_t)value & ESP32S3_GPIO1_MASK;
        break;
    case A_GPIO_STATUS1_W1TC:
        s->gpio_status1 &= ~((uint32_t)value & ESP32S3_GPIO1_MASK);
        break;

    case A_GPIO_CLOCK_GATE:
        s->clock_gate = (uint32_t)value & 0x1;
        break;
    case A_GPIO_DATE:
        s->date_reg = (uint32_t)value;
        break;

    /* Read-only registers — writes ignored */
    case A_GPIO_STRAP:
    case A_GPIO_IN:
    case A_GPIO_IN1:
    case A_GPIO_STATUS_NEXT:
    case A_GPIO_STATUS_NEXT1:
    case A_GPIO_PCPU_INT:
    case A_GPIO_PCPU_NMI_INT:
    case A_GPIO_PCPU_INT1:
    case A_GPIO_PCPU_NMI_INT1:
    case A_GPIO_CPUSDIO_INT:
    case A_GPIO_CPUSDIO_INT1:
        break;

    default:
        if (GPIO_WARNING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write at offset 0x%04" HWADDR_PRIx "\n",
                          __func__, a);
        }
        break;
    }

    esp32s3_gpio_notify_observer(s);
    esp32s3_gpio_update_all(s);
}

/* ---------- External input lines and solver API ---------- */

/*
 * Common net-drive path: qdev "gpio-in" lines call this with valid=true
 * (a plain boolean digital driver); the DC solver can pass valid=false or
 * attach a voltage via esp32s3_gpio_drive_net().
 */
static void esp32s3_gpio_net_drive(ESP32S3GPIOState *s, int n,
                                   bool valid, bool level, double volts)
{
    ESP32S3GpioNet *net = &s->net[n];

    net->volts = volts;
    if (!valid) {
        /* The external driver detached: let the resolver re-settle pulls */
        net->valid = false;
        net->level = 0;
        net->diag = ESP32S3_GPIO_PAD_FLOATING;
        return;
    }
    net->valid = true;
    net->level = (level != 0);
    net->diag = ESP32S3_GPIO_PAD_EXTERNAL;
}

void esp32s3_gpio_drive_net(ESP32S3GPIOState *s, unsigned pad,
                            bool valid, bool level, double volts)
{
    if (pad >= ESP32S3_GPIO_COUNT) {
        return;
    }
    s->solver_attached[pad] = true;
    esp32s3_gpio_net_drive(s, (int)pad, valid, level, volts);
    esp32s3_gpio_update_pad(s, (int)pad);
    esp32s3_gpio_update_irq(s);
    /* No observer wake here: the solver caused this update itself. */
}

void esp32s3_gpio_set_drive_observer(ESP32S3GPIOState *s,
                                     ESP32S3GpioDriveObserver fn, void *opaque)
{
    s->drive_observer = fn;
    s->observer_opaque = opaque;
}

bool esp32s3_gpio_solver_attached(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->solver_attached[pad] : false;
}

void esp32s3_gpio_set_solver_owned(ESP32S3GPIOState *s, bool owned)
{
    s->solver_owned = owned;
}

void esp32s3_gpio_detach_net(ESP32S3GPIOState *s, unsigned pad)
{
    if (pad >= ESP32S3_GPIO_COUNT) {
        return;
    }
    s->solver_attached[pad] = false;
    esp32s3_gpio_update_pad(s, (int)pad);
    esp32s3_gpio_update_irq(s);
}

void esp32s3_gpio_set_unknown_handler(ESP32S3GPIOState *s,
                                      ESP32S3GpioUnknownFn fn, void *opaque)
{
    s->unknown_handler = fn;
    s->unknown_opaque = opaque;
}

static void esp32s3_gpio_unknown_bh(void *opaque)
{
    /* Bounded diagnosable paused dependency, established from the main
     * context: the VM stays paused until an external resume (no
     * auto-resume); the pad policy/provenance is reported once. */
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    if (!s->unknown_pad_reported) {
        s->unknown_pad_reported = true;
        warn_report("esp32s3.gpio: pad %u sampled UNKNOWN (enabled input, "
                    "no resolved net: attach a physical pull/source via "
                    "the electrical layer); VM paused (strict profile; "
                    "resume explicitly after the net resolves)",
                    s->unknown_first_pad);
    }
    vm_stop(RUN_STATE_PAUSED);
}

/* A solver-owned pad reports UNKNOWN at a consumption point */
static void esp32s3_gpio_unknown_use(ESP32S3GPIOState *s, int n)
{
    if (s->unknown_handler) {
        s->unknown_handler(s->unknown_opaque, (unsigned)n);
        return;
    }
    /* Default strict behavior: pause from the main context via BH */
    if (!s->unknown_bh) {
        s->unknown_bh = qemu_bh_new(esp32s3_gpio_unknown_bh, s);
    }
    if (!s->unknown_pad_reported) {
        s->unknown_first_pad = (unsigned)n;
    }
    qemu_bh_schedule(s->unknown_bh);
}

void esp32s3_gpio_consume_unknown(ESP32S3GPIOState *s, unsigned pad)
{
    if (pad >= ESP32S3_GPIO_COUNT) {
        return;
    }
    if (s->solver_owned || s->solver_attached[pad]) {
        esp32s3_gpio_unknown_use(s, (int)pad);
    }
}

/* Wake the drive observer (guest register/mux/reset changes only) */
static void esp32s3_gpio_notify_observer(ESP32S3GPIOState *s)
{
    if (s->drive_observer) {
        s->drive_observer(s->observer_opaque);
    }
}

ESP32S3GpioPadState esp32s3_gpio_pad_state(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->net[pad].diag : ESP32S3_GPIO_PAD_FLOATING;
}

bool esp32s3_gpio_net_valid(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->net[pad].valid : false;
}

double esp32s3_gpio_net_voltage(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->net[pad].volts : 0.0;
}

bool esp32s3_gpio_sleep_active(ESP32S3GPIOState *s)
{
    return s->rtc_cntl != NULL && s->rtc_cntl->sleep_requested;
}

bool esp32s3_gpio_hold_active(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->hold_active[pad] : false;
}

bool esp32s3_gpio_hold_level(ESP32S3GPIOState *s, unsigned pad)
{
    return pad < ESP32S3_GPIO_COUNT ? s->hold_level[pad] != 0 : false;
}

void esp32s3_gpio_get_drive_snapshot_ext(ESP32S3GPIOState *s, unsigned pad,
                                         ESP32S3GpioDriveSnapshot *out)
{
    uint32_t mux;

    esp32s3_gpio_get_drive_snapshot(s, pad, out);
    if (pad >= ESP32S3_GPIO_COUNT || s->iomux == NULL) {
        return;
    }
    mux = esp32s3_iomux_get(s->iomux, (int)pad);
    out->slp_sel = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_SEL) != 0;
    out->slp_ie = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_IE) != 0;
    out->slp_oe = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_OE) != 0;
    out->slp_pu = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_PU) != 0;
    out->slp_pd = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_PD) != 0;
    out->slp_drv = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_DRV);
    out->sleep_active = esp32s3_gpio_sleep_active(s);
    out->hold = s->hold_active[pad];
    out->hold_level = s->hold_level[pad];
    out->hold_oe = s->hold_oe[pad];
    out->hold_open_drain = s->hold_od[pad];
    out->hold_pull_up = s->hold_pull_up[pad];
    out->hold_pull_down = s->hold_pull_down[pad];
    out->hold_matrix_routed = s->hold_matrix_routed[pad];
}

void esp32s3_gpio_get_drive_snapshot(ESP32S3GPIOState *s, unsigned pad,
                                     ESP32S3GpioDriveSnapshot *out)
{
    uint32_t mux, pin, out_cfg;

    memset(out, 0, sizeof(*out));
    if (pad >= ESP32S3_GPIO_COUNT) {
        return;
    }

    mux = s->iomux ? esp32s3_iomux_get(s->iomux, (int)pad) : 0;
    pin = s->gpio_pin[pad];
    out_cfg = s->func_out_sel_cfg[pad];

    out->mcu_sel = FIELD_EX32(mux, IO_MUX_GPIOn, MCU_SEL);
    out->matrix_gpio = out->mcu_sel == ESP32S3_IOMUX_MCU_SEL_GPIO;
    out->out_sel = FIELD_EX32(out_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_SEL);
    out->out_inv = FIELD_EX32(out_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_INV_SEL);
    /* Semantic truth: OE sourced from the routed peripheral signal only
     * for a peripheral selection with FUNC_OEN_SEL == 0 (S3 polarity;
     * simple GPIO 256 hardwires OE to GPIO_ENABLE) */
    out->oen_from_signal =
        out->out_sel != GPIO_FUNC_OUT_SEL_NONE &&
        !FIELD_EX32(out_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_SEL);
    out->oen_inv = FIELD_EX32(out_cfg, GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_INV_SEL);
    out->out_oe = enable_bit(s, (int)pad);
    out->out_level = in_bit(s, (int)pad);
    out->open_drain = pin_field(s, (int)pad,
                                R_GPIO_PINn_PAD_DRIVER_SHIFT, 1);
    out->drive_strength = FIELD_EX32(mux, IO_MUX_GPIOn, FUN_DRV);
    out->ie = FIELD_EX32(mux, IO_MUX_GPIOn, FUN_IE);
    out->pull_up = FIELD_EX32(mux, IO_MUX_GPIOn, FUN_PU);
    out->pull_down = FIELD_EX32(mux, IO_MUX_GPIOn, FUN_PD);
    out->slp_sel = FIELD_EX32(mux, IO_MUX_GPIOn, SLP_SEL);
    /* hold / analog_owned stay false: documented boundaries */
}

static void esp32s3_gpio_input_line(void *opaque, int n, int level)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    if (n < 0 || n >= ESP32S3_GPIO_COUNT) {
        return;
    }
    /* qdev "gpio-in" lines are plain boolean drivers: the level holds on
     * the net (no detach concept).  In solver-owned mode they are ignored
     * entirely — only esp32s3_gpio_drive_net() may write the solved net
     * (a floating native wire must not become a fabricated EXTERNAL bit). */
    if (s->solver_owned) {
        return;
    }
    esp32s3_gpio_net_drive(s, n, true, level != 0, 0.0);
    esp32s3_gpio_update_pad(s, n);
    esp32s3_gpio_update_irq(s);
}

/* IO_MUX pad registers changed: re-resolve every pad net and sample */
static void esp32s3_gpio_iomux_change(void *opaque, int n, int level)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    esp32s3_gpio_notify_observer(s);
    esp32s3_gpio_update_all(s);
}

/* RTC_CNTL hold-register writes (pad-hold-change): re-resolve with hold
 * latch transitions applied. */
static void esp32s3_gpio_hold_change(void *opaque, int n, int level)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    esp32s3_gpio_notify_observer(s);
    esp32s3_gpio_update_all(s);
}

/* ---------- Reset / lifecycle ---------- */

static void esp32s3_gpio_reset_hold(Object *obj, ResetType type)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

#ifdef ESP32S3_GPIO_RESET_DOMAIN
    /* Preserve state on CPU-only resets: only the PERIPH domain (or a
     * chip/power-on reset) clears the digital GPIO register file. */
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) {
        return;
    }
#endif
    s->gpio_out = 0;
    s->gpio_out1 = 0;
    s->gpio_enable = 0;
    s->gpio_enable1 = 0;
    s->gpio_status = 0;
    s->gpio_status1 = 0;
    s->bt_select = 0;
    s->sdio_select = 0;
    s->clock_gate = 0;
    s->date_reg = ESP32S3_GPIO_DATE_VERSION;

    for (int i = 0; i < ESP32S3_GPIO_COUNT; i++) {
        bool was_held = s->hold_active[i];
        bool was_level = s->hold_level[i];

        s->gpio_pin[i] = 0;
        s->unknown_pad_reported = false;
        memset(&s->net[i], 0, sizeof(s->net[i]));
        s->net[i].diag = ESP32S3_GPIO_PAD_FLOATING;
        s->solver_attached[i] = false;
        s->line_out[i] = 0;
        s->sample[i] = 0;
        if (was_held) {
            /* Held pads keep their latched level through resets (TRM
             * ch.6.9: the clamp persists until the hold bit clears). */
            s->hold_active[i] = true;
            s->hold_level[i] = was_level;
        }
        /* Input matrix default: all peripheral inputs tied to constant low */
        s->func_in_sel_cfg[i] = GPIO_FUNC_IN_LOW;
        /* Output matrix default: no peripheral signal routed (simple GPIO) */
        s->func_out_sel_cfg[i] = GPIO_FUNC_OUT_SEL_NONE;
        qemu_set_irq(s->output_lines[i], 0);
    }
    for (int i = ESP32S3_GPIO_COUNT; i < ESP32S3_GPIO_FUNC_IN_SEL_COUNT; i++) {
        s->func_in_sel_cfg[i] = GPIO_FUNC_IN_LOW;
    }
    esp32s3_gpio_notify_observer(s);
}

static void esp32s3_gpio_init(Object *obj)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32S3_STRAP_MODE_FLASH_BOOT, &error_fatal);

    qdev_init_gpio_in_named(DEVICE(obj), esp32s3_gpio_input_line,
                            ESP32S3_GPIO_INPUT_LINES, ESP32S3_GPIO_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), esp32s3_gpio_iomux_change,
                            ESP32S3_GPIO_IOMUX_CHANGE, 1);
    qdev_init_gpio_in_named(DEVICE(obj), esp32s3_gpio_hold_change,
                            ESP32S3_GPIO_HOLD_CHANGE, 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->output_lines,
                             ESP32S3_GPIO_OUTPUT_LINES, ESP32S3_GPIO_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq_nmi,
                             ESP32S3_GPIO_IRQ_NMI, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq_app,
                             ESP32S3_GPIO_IRQ_APP, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq_app_nmi,
                             ESP32S3_GPIO_IRQ_APP_NMI, 1);

    object_property_add_link(OBJECT(obj), "soc-reset", TYPE_DEVICE,
                             (Object **)&s->soc_reset,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    object_property_add_link(OBJECT(obj), "rtc-cntl", TYPE_ESP32S3_RTC_CNTL,
                             (Object **)&s->rtc_cntl,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    object_property_add_link(OBJECT(obj), "iomux", TYPE_ESP32S3_IOMUX,
                             (Object **)&s->iomux,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static void esp32s3_gpio_class_init(ObjectClass *klass, void *data)
{
    Esp32GpioClass *gc = ESP32_GPIO_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    /* Override the parent virtual register file */
    gc->gpio_read = esp32s3_gpio_read;
    gc->gpio_write = esp32s3_gpio_write;

    rc->phases.hold = esp32s3_gpio_reset_hold;
    {
        static Property esp32s3_gpio_props[] = {
            DEFINE_PROP_BOOL("strict-unknown", ESP32S3GPIOState,
                             strict_unknown, false),
            DEFINE_PROP_END_OF_LIST(),
        };
        device_class_set_props(dc, esp32s3_gpio_props);
    }
    dc->desc = "ESP32-S3 GPIO with matrix";
}

static const TypeInfo esp32s3_gpio_info = {
    .name = TYPE_ESP32S3_GPIO,
    .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32S3GPIOState),
    .instance_init = esp32s3_gpio_init,
    .class_init = esp32s3_gpio_class_init,
    .class_size = sizeof(ESP32S3GPIOClass),
};

static void esp32s3_gpio_register_types(void)
{
    type_register_static(&esp32s3_gpio_info);
}

type_init(esp32s3_gpio_register_types)
