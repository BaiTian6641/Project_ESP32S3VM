/*
 * ESP32-S3 GPIO emulation with GPIO matrix routing
 *
 * Register layout derived from the ESP32-S3 TRM chapter 6 (IO MUX and GPIO
 * Matrix) register summary, cross-checked against the ESP-IDF 6.1 SoC headers
 * (components/soc/esp32s3/register/soc/gpio_reg.h and gpio_struct.h):
 *   - GPIO0..GPIO48 (49 pads; pads 22..25 do not exist on the S3 chip)
 *   - Input signal matrix: 256 signals  (GPIO_FUNCn_IN_SEL_CFG @ 0x154)
 *   - Output signal matrix: 49 entries  (GPIO_FUNCm_OUT_SEL_CFG @ 0x554)
 *   - Four interrupt matrix sources: PROCPU level / PROCPU NMI /
 *     APPCPU level / APPCPU NMI (ETS_GPIO_INTR_SOURCE 16..ETS_GPIO_NMI_SOURCE2 19)
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "esp32_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

#define TYPE_ESP32S3_GPIO "esp32s3.gpio"
#define ESP32S3_GPIO(obj)           OBJECT_CHECK(ESP32S3GPIOState, (obj), TYPE_ESP32S3_GPIO)
#define ESP32S3_GPIO_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3GPIOClass, obj, TYPE_ESP32S3_GPIO)
#define ESP32S3_GPIO_CLASS(klass)   OBJECT_CLASS_CHECK(ESP32S3GPIOClass, klass, TYPE_ESP32S3_GPIO)

/* Bootstrap options for ESP32-S3 (4-bit, strapping register is 16 bit wide) */
#define ESP32S3_STRAP_MODE_FLASH_BOOT 0x4   /* SPI Boot */
#define ESP32S3_STRAP_MODE_UART_BOOT  0x0   /* ROM UART/USB download boot */

/* ESP32-S3 has 49 GPIO registers (GPIO0 - GPIO48); pads 22..25 are holes */
#define ESP32S3_GPIO_COUNT             49

/* Number of input peripheral signals routed through the GPIO matrix */
#define ESP32S3_GPIO_FUNC_IN_SEL_COUNT 256

/* The "1" register banks (OUT1/ENABLE1/IN1/STATUS1/...) are 22 bits wide */
#define ESP32S3_GPIO1_MASK  0x003FFFFF

/* ---------- Register offsets relative to DR_REG_GPIO_BASE (0x60004000) ---------- */

/* BT select (bluetooth pin muxing, stored only) */
REG32(GPIO_BT_SELECT, 0x0000)

/* Output data registers */
REG32(GPIO_OUT, 0x0004)
REG32(GPIO_OUT_W1TS, 0x0008)
REG32(GPIO_OUT_W1TC, 0x000C)
REG32(GPIO_OUT1, 0x0010)          /* bits [21:0], GPIO32-48 */
REG32(GPIO_OUT1_W1TS, 0x0014)
REG32(GPIO_OUT1_W1TC, 0x0018)

/* SDIO select */
REG32(GPIO_SDIO_SELECT, 0x001C)

/* Output enable registers */
REG32(GPIO_ENABLE, 0x0020)
REG32(GPIO_ENABLE_W1TS, 0x0024)
REG32(GPIO_ENABLE_W1TC, 0x0028)
REG32(GPIO_ENABLE1, 0x002C)       /* bits [21:0], GPIO32-48 */
REG32(GPIO_ENABLE1_W1TS, 0x0030)
REG32(GPIO_ENABLE1_W1TC, 0x0034)

/* GPIO_STRAP is at 0x0038 — defined in esp32_gpio.h */

/* Input registers (read-only pad samples) */
REG32(GPIO_IN, 0x003C)
REG32(GPIO_IN1, 0x0040)           /* bits [21:0], GPIO32-48 */

/* Interrupt status registers */
REG32(GPIO_STATUS, 0x0044)
REG32(GPIO_STATUS_W1TS, 0x0048)
REG32(GPIO_STATUS_W1TC, 0x004C)
REG32(GPIO_STATUS1, 0x0050)       /* bits [21:0], GPIO32-48 */
REG32(GPIO_STATUS1_W1TS, 0x0054)
REG32(GPIO_STATUS1_W1TC, 0x0058)

/* Per-CPU interrupt status (read-only, STATUS masked by PINn.INT_ENA) */
REG32(GPIO_PCPU_INT, 0x005C)      /* PROCPU level int (matrix source 16) */
REG32(GPIO_PCPU_NMI_INT, 0x0060)  /* PROCPU NMI        (matrix source 17) */
REG32(GPIO_CPUSDIO_INT, 0x0064)   /* SDIO int status   (INT_ENA bit 4) */
REG32(GPIO_PCPU_INT1, 0x0068)     /* APPCPU level int  (matrix source 18) */
REG32(GPIO_PCPU_NMI_INT1, 0x006C) /* APPCPU NMI        (matrix source 19) */
REG32(GPIO_CPUSDIO_INT1, 0x0070)

/* Per-pin configuration: GPIO_PINn_REG = 0x0074 + n*4, n=0..48 */
#define GPIO_PINn_REG_OFFSET(n) (0x0074 + (n) * 4)

/* GPIO_PINn field layout (same for all n) */
    FIELD(GPIO_PINn, SYNC2_BYPASS, 0, 2)
    FIELD(GPIO_PINn, PAD_DRIVER, 2, 1)     /* 0=push-pull, 1=open-drain */
    FIELD(GPIO_PINn, SYNC1_BYPASS, 3, 2)
    /* bits 5-6: reserved */
    FIELD(GPIO_PINn, INT_TYPE, 7, 3)       /* 0=dis,1=rise,2=fall,3=any,4=low,5=high */
    FIELD(GPIO_PINn, WAKEUP_ENABLE, 10, 1)
    /* bits 11-12: CONFIG (stored) */
    FIELD(GPIO_PINn, INT_ENA, 13, 5)       /* bit0 PROCPU int, 1 APPCPU int,
                                              2 PROCPU NMI, 3 APPCPU NMI, 4 SDIO */

/* Interrupt types (GPIO_PINn.INT_TYPE) */
enum {
    ESP32S3_GPIO_INT_DISABLED = 0,
    ESP32S3_GPIO_INT_RISING   = 1,
    ESP32S3_GPIO_INT_FALLING  = 2,
    ESP32S3_GPIO_INT_ANYEDGE  = 3,
    ESP32S3_GPIO_INT_LOW      = 4,
    ESP32S3_GPIO_INT_HIGH     = 5,
};

/* GPIO_PINn.INT_ENA bit positions (register-relative) */
#define ESP32S3_GPIO_INT_ENA_PROCPU      (1U << 13)
#define ESP32S3_GPIO_INT_ENA_APPCPU      (1U << 14)
#define ESP32S3_GPIO_INT_ENA_PROCPU_NMI  (1U << 15)
#define ESP32S3_GPIO_INT_ENA_APPCPU_NMI  (1U << 16)
#define ESP32S3_GPIO_INT_ENA_SDIO        (1U << 17)
#define ESP32S3_GPIO_PINn_WRITE_MASK     0x0007FFFF  /* bits [18:0] */

/* Status "next" registers (0x14C/0x150: level conditions currently true) */
REG32(GPIO_STATUS_NEXT, 0x014C)
REG32(GPIO_STATUS_NEXT1, 0x0150)

/* Input signal matrix: GPIO_FUNCn_IN_SEL_CFG_REG = 0x154 + n*4, n=0..255 */
#define GPIO_FUNC_IN_SEL_CFG_OFFSET(n) (0x0154 + (n) * 4)

    FIELD(GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_SEL, 0, 6)     /* GPIO 0-48 or constant */
    FIELD(GPIO_FUNCn_IN_SEL_CFG, FUNC_IN_INV_SEL, 6, 1) /* invert routed input */
    FIELD(GPIO_FUNCn_IN_SEL_CFG, SIG_IN_SEL, 7, 1)      /* 1=GPIO matrix, 0=IO_MUX direct */

/* Special FUNC_IN_SEL values (constant inputs, TRM ch.6 Peripheral Input) */
#define GPIO_FUNC_IN_HIGH   0x38   /* input signal tied to logic high */
#define GPIO_FUNC_IN_LOW    0x3C   /* input signal tied to logic low  */
#define GPIO_FUNC_IN_SEL_WRITE_MASK 0xFF

/* Output signal matrix: GPIO_FUNCn_OUT_SEL_CFG_REG = 0x554 + n*4, n=0..48 */
#define GPIO_FUNC_OUT_SEL_CFG_OFFSET(n) (0x0554 + (n) * 4)

    FIELD(GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_SEL, 0, 9)      /* output signal index */
    FIELD(GPIO_FUNCn_OUT_SEL_CFG, FUNC_OUT_INV_SEL, 9, 1)  /* invert output */
    FIELD(GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_SEL, 10, 1)     /* 1=OE from signal, 0=GPIO_ENABLE */
    FIELD(GPIO_FUNCn_OUT_SEL_CFG, FUNC_OEN_INV_SEL, 11, 1) /* invert OE */
#define GPIO_FUNC_OUT_SEL_CFG_WRITE_MASK 0xFFF

/* FUNC_OUT_SEL: 256 = simple GPIO output (GPIO_OUT/ENABLE drive the pad).
 * Values 0..255 select a peripheral matrix output signal; none of them is
 * currently sourced by a model, so the pad driver stays released. */
#define GPIO_FUNC_OUT_SEL_NONE  0x100
#define GPIO_FUNC_OUT_SEL_INVALID_MAX 0x1FF  /* 257..511: reserved, no drive */

/* Clock gate */
REG32(GPIO_CLOCK_GATE, 0x062C)
    FIELD(GPIO_CLOCK_GATE, CLK_EN, 0, 1)

/* Date/version */
REG32(GPIO_DATE, 0x06FC)

#define ESP32S3_GPIO_DATE_VERSION   0x2101191

/* MMIO range implemented by the model */
#define ESP32S3_GPIO_MMIO_SIZE      0x1000

/*
 * Electrical view of a pad net (digital, pre-analog-gate).
 *
 * "Floating is NOT valid low": an unresolved net is reported separately
 * from the sampled register bit.  Guest registers always return ordinary
 * bits (unresolved nets sample 0 under the documented permissive profile)
 * while esp32s3_gpio_pad_state() exposes the resolution diagnostics the
 * future analog solver will drive.
 */
typedef enum {
    ESP32S3_GPIO_PAD_FLOATING = 0, /* no driver, no pull: unresolved      */
    ESP32S3_GPIO_PAD_DRIVEN,       /* this pad's output driver            */
    ESP32S3_GPIO_PAD_EXTERNAL,     /* external/solver net driver          */
    ESP32S3_GPIO_PAD_PULLED_HIGH,  /* weak pull-up resolved the net       */
    ESP32S3_GPIO_PAD_PULLED_LOW,   /* weak pull-down resolved the net     */
    ESP32S3_GPIO_PAD_CONTENTION,   /* own driver vs valid external driver
                                    * at a different level; the digital
                                    * sampling policy reads the internal
                                    * drive until the analog gate resolves */
    ESP32S3_GPIO_PAD_UNKNOWN,      /* solver attached but reports the net
                                    * unresolved: never falls back to a
                                    * pull/internal bit as if it were a
                                    * valid sample (strict consumers must
                                    * pause before sampling/IRQ use)      */
} ESP32S3GpioPadState;

/*
 * RAW effective drive configuration of one pad — everything the future
 * net solver needs to build its own Thevenin view.  This is the register
 * truth, deliberately separate from the resolved gpio-out line (feeding a
 * resolved line back into a solver would create false feedback and cannot
 * express contention).
 */
typedef struct ESP32S3GpioDriveSnapshot {
    /* Ownership */
    unsigned mcu_sel;       /* IO_MUX function; ESP32S3_IOMUX_MCU_SEL_GPIO (1)
                             * = GPIO matrix path */
    bool matrix_gpio;       /* mcu_sel == ESP32S3_IOMUX_MCU_SEL_GPIO */
    /* Output driver */
    bool out_oe;            /* GPIO_ENABLE bank bit (simple GPIO path) */
    unsigned out_sel;       /* FUNC_OUT_SEL raw (256 = simple GPIO) */
    bool out_inv;           /* FUNC_OUT_INV_SEL */
    bool oen_from_signal;   /* true when OE comes from the routed
                             * peripheral signal (FUNC_OEN_SEL == 0 on S3);
                             * false when GPIO_ENABLE drives OE
                             * (FUNC_OEN_SEL == 1, the IDF default) */
    bool oen_inv;           /* FUNC_OEN_INV_SEL */
    bool out_level;         /* raw source level (GPIO_OUT bank bit) */
    bool open_drain;        /* GPIO_PINn.PAD_DRIVER */
    unsigned drive_strength;/* IO_MUX FUN_DRV (stored, electrically inert) */
    /* Input */
    bool ie;                /* IO_MUX FUN_IE input-buffer enable */
    /* Pulls */
    bool pull_up;           /* IO_MUX FUN_PU */
    bool pull_down;         /* IO_MUX FUN_PD */
    /* Sleep / hold ownership (stored bits, not resolved in this model) */
    bool slp_sel;           /* IO_MUX SLP_SEL */
    bool hold;              /* always false: pad hold is a documented boundary */
    bool analog_owned;      /* always false on digital pads; RTC/analog
                             * ownership lives in esp32s3_rtc_io_* + SENS */
} ESP32S3GpioDriveSnapshot;

typedef struct ESP32S3GpioNet {
    bool valid;    /* node level is resolved (never floating) */
    bool level;    /* resolved digital level when valid       */
    double volts;  /* last solver-provided voltage (inert)    */
    ESP32S3GpioPadState diag;
} ESP32S3GpioNet;

/* Named qdev GPIO arrays */
#define ESP32S3_GPIO_INPUT_LINES   "gpio-in"   /* external drivers, 49 lines */
#define ESP32S3_GPIO_OUTPUT_LINES  "gpio-out"  /* resolved pad levels, 49 lines */
#define ESP32S3_GPIO_IOMUX_CHANGE  "iomux-change" /* from iomux pad-change */
#define ESP32S3_GPIO_IRQ_NMI       "nmi-int"   /* PROCPU NMI lane   (source 17) */
#define ESP32S3_GPIO_IRQ_APP       "app-int"   /* APPCPU level lane (source 18) */
#define ESP32S3_GPIO_IRQ_APP_NMI   "app-nmi"   /* APPCPU NMI lane   (source 19) */
/* sysbus irq index 0 is the PROCPU level lane (matrix source 16) */

/* Raw-truth change notifier type (see esp32s3_gpio_set_drive_observer) */
typedef void (*ESP32S3GpioDriveObserver)(void *opaque);

/* Unknown-consumption hook type (see esp32s3_gpio_set_unknown_handler) */
typedef void (*ESP32S3GpioUnknownFn)(void *opaque, unsigned pad);

typedef struct ESP32S3GPIOState {
    Esp32GpioState parent;

    /* Register file */
    uint32_t bt_select;
    uint32_t gpio_out;         /* GPIO_OUT    (GPIO0-31) */
    uint32_t gpio_out1;        /* GPIO_OUT1   (GPIO32-48) */
    uint32_t gpio_enable;      /* GPIO_ENABLE (GPIO0-31) */
    uint32_t gpio_enable1;     /* GPIO_ENABLE1(GPIO32-48) */
    uint32_t gpio_status;      /* GPIO_STATUS (GPIO0-31) */
    uint32_t gpio_status1;     /* GPIO_STATUS1(GPIO32-48) */
    uint32_t sdio_select;
    uint32_t gpio_pin[ESP32S3_GPIO_COUNT];
    uint32_t func_in_sel_cfg[ESP32S3_GPIO_FUNC_IN_SEL_COUNT];
    uint32_t func_out_sel_cfg[ESP32S3_GPIO_COUNT];
    uint32_t clock_gate;
    uint32_t date_reg;

    /* Electrical view of the pads */
    ESP32S3IOMuxState *iomux;                   /* link: "iomux" */
    ESP32S3GpioNet net[ESP32S3_GPIO_COUNT];     /* resolved pad nets */
    bool solver_attached[ESP32S3_GPIO_COUNT];   /* external/solver driver */
    bool solver_owned;                          /* solver authoritative on
                                                 * every pad, even unattached */
    bool strict_unknown;                        /* pause-before-consumption on
                                                 * any floating/unknown input */
    DeviceState *soc_reset;                     /* link: "soc-reset" (domain
                                                 * gating, esp32s3_reset_domain) */
    ESP32S3GpioUnknownFn unknown_handler;       /* strict pause hook */
    void *unknown_opaque;
    struct QemuBH *unknown_bh;                  /* main-context pause */
    bool unknown_paused_reported;
    ESP32S3GpioDriveObserver drive_observer;    /* raw-truth change wake */
    void *observer_opaque;
    uint8_t line_out[ESP32S3_GPIO_COUNT];       /* last level on gpio-out */
    uint8_t sample[ESP32S3_GPIO_COUNT];         /* input-buffer samples */
    bool irq_out[4];                            /* PROCPU/NMI/APP/APPNMI lanes */
    qemu_irq output_lines[ESP32S3_GPIO_COUNT];
    qemu_irq irq_nmi;
    qemu_irq irq_app;
    qemu_irq irq_app_nmi;
} ESP32S3GPIOState;

typedef struct ESP32S3GPIOClass {
    Esp32GpioClass parent;
} ESP32S3GPIOClass;

/*
 * Effective level of GPIO matrix input signal @signal (0..255) following
 * GPIO_FUNCn_IN_SEL_CFG (pad routing, inversion and the constant input
 * values 0x38/0x3C).  Intended for peripheral models that consume matrix
 * inputs (UART, RMT, ...); returns 0 for a bypassed (SIG_IN_SEL=0) signal.
 */
uint32_t esp32s3_gpio_matrix_input(ESP32S3GPIOState *s, uint32_t signal);

/*
 * Strict matrix sample (no silent fallback): returns true and writes
 * *level when signal @signal is matrix-routed (SIG_IN_SEL=1; pad sample
 * or 0x38/0x3C constants, inversion applied).  Returns false when the
 * signal is bypassed (SIG_IN_SEL=0: direct IO_MUX route, unmodelled in
 * this lane) or out of range — consumers must treat that as unresolved,
 * never as a valid 0.
 *
 * Validity contract: for a routed pad the returned level is the
 * FUN_IE-gated digital sample.  It carries no net-validity/UNKNOWN gate —
 * callers consuming solver-resolved nets must check the pad net first
 * (esp32s3_gpio_net_valid / pad_state or the electrical layer) before
 * treating the level as authoritative; the strict pre-consumption pause
 * path is the MMIO input read / IRQ evaluation, not this raw helper.
 */
bool esp32s3_gpio_matrix_sample(ESP32S3GPIOState *s, uint32_t signal, bool *level);

/*
 * Drive a pad net from an external device or the future DC solver:
 * @valid=false marks the net unresolved (floating); @volts records the
 * solver voltage (electrically inert until the analog gate).  This is the
 * voltage/validity entry point; the boolean "gpio-in" qdev lines feed the
 * same path with valid=true.
 */
void esp32s3_gpio_drive_net(ESP32S3GPIOState *s, unsigned pad,
                            bool valid, bool level, double volts);

/* Resolution diagnostics for pad @pad (floating/pulled/driven states). */
ESP32S3GpioPadState esp32s3_gpio_pad_state(ESP32S3GPIOState *s, unsigned pad);

/* True when pad @pad's net is resolved (not floating). */
bool esp32s3_gpio_net_valid(ESP32S3GPIOState *s, unsigned pad);

/*
 * Public strict-consumption gate: run the pad's UNKNOWN policy (native
 * unknown handler, else the default main-context paused dependency BH)
 * exactly as MMIO input reads do.  Call before treating a directly
 * sampled pad level (bypass IO_MUX route, external sampler) as valid.
 */
void esp32s3_gpio_consume_unknown(ESP32S3GPIOState *s, unsigned pad);

/* Last solver-provided voltage for pad @pad (0 before the analog gate). */
double esp32s3_gpio_net_voltage(ESP32S3GPIOState *s, unsigned pad);

/*
 * RAW effective drive configuration of pad @pad for the net solver
 * (see ESP32S3GpioDriveSnapshot).  Register truth, independent of the
 * resolved gpio-out line.
 */
void esp32s3_gpio_get_drive_snapshot(ESP32S3GPIOState *s, unsigned pad,
                                     ESP32S3GpioDriveSnapshot *out);

/*
 * Raw-truth change notifier (net-solver wake): invoked after every guest
 * register write to the GPIO/IO_MUX/RTC_IO register files and after device
 * reset — never on solver drive updates (the solver caused those) and
 * never on mere resolved-line changes.  The callback may query any
 * snapshot/diagnostic.  Pass fn = NULL to unregister.
 */
void esp32s3_gpio_set_drive_observer(ESP32S3GPIOState *s,
                                 ESP32S3GpioDriveObserver fn, void *opaque);

/*
 * Strict-sampling gate: true when an external solver/driver is attached
 * to pad @pad's net.  While attached, the solver's valid level is the
 * authoritative sample (even against the internal output driver; the
 * CONTENTION diagnostic still reports raw-desired vs solved sample), an
 * unresolved report holds ESP32S3_GPIO_PAD_UNKNOWN and the input keeps
 * its last sample (persisted rule; no fabricated 0).  Strict consumers
 * pause before input reads/IRQ evaluation on UNKNOWN.
 */
bool esp32s3_gpio_solver_attached(ESP32S3GPIOState *s, unsigned pad);

/*
 * Device-wide solver ownership: when true, every pad behaves as attached
 * (solver valid level authoritative, unresolved = UNKNOWN); the internal
 * drive/pull resolution never produces the sample.
 */
void esp32s3_gpio_set_solver_owned(ESP32S3GPIOState *s, bool owned);

/*
 * Unknown-consumption hook (strict gate, native owns the pause): invoked
 * whenever a sample is consumed while the solver owns the pad and reports
 * UNKNOWN — GPIO_IN reads, matrix input lookups and IRQ evaluation pass
 * here before the persisted last sample is used.  The handler may stop
 * the VM / abort the MMIO access so the guest never consumes a stale
 * value as valid.  fn=NULL restores the default persisted rule.
 */
void esp32s3_gpio_set_unknown_handler(ESP32S3GPIOState *s,
                                      ESP32S3GpioUnknownFn fn, void *opaque);
