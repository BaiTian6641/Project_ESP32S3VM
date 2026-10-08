/*
 * ESP32-S3 RTC IO MUX
 *
 * Register file for the 22 RTC pads (RTC_GPIO0..21 = GPIO0..21) at
 * DR_REG_RTC_IO_MUX_BASE (0x60008400).  Layout per the ESP32-S3 TRM ch.6
 * register summary and the ESP-IDF 6.1 headers (rtc_io_reg.h / rtc_io_struct.h):
 *
 *   0x00 RTC_GPIO_OUT_REG      RTC GPIO 0-21 output data, bits [31:10]
 *   0x04 RTC_GPIO_OUT_W1TS     write-1-to-set, bits [31:10]
 *   0x08 RTC_GPIO_OUT_W1TC     write-1-to-clear, bits [31:10]
 *   0x0C RTC_GPIO_ENABLE_REG   output enable, bits [31:10]
 *   0x10 RTC_GPIO_ENABLE_W1TS
 *   0x14 RTC_GPIO_ENABLE_W1TC
 *   0x18 RTC_GPIO_STATUS_REG   interrupt status, bits [31:10]
 *   0x1C RTC_GPIO_STATUS_W1TS
 *   0x20 RTC_GPIO_STATUS_W1TC
 *   0x24 RTC_GPIO_IN_REG       input data (read-only), bits [31:10]
 *   0x28 RTC_GPIO_PINn_REG     n = 0..21: PAD_DRIVER [2], INT_TYPE [9:7],
 *                              WAKEUP_ENABLE [10]
 *   0x80 RTC_IO_RTC_DEBUG_SEL_REG (stored)
 *   0x84..0xD8 pad configuration registers:
 *       pad 0        -> RTC_IO_TOUCH_PAD14_REG (0xBC)
 *       pads 1..14   -> RTC_IO_TOUCH_PAD0..13_REG (0x84..0xB8)
 *       pad 15       -> RTC_IO_XTAL_32P_PAD_REG (0xC0)
 *       pad 16       -> RTC_IO_XTAL_32N_PAD_REG (0xC4)
 *       pad 17       -> RTC_IO_PAD_DAC1_REG (0xC8)
 *       pad 18       -> RTC_IO_PAD_DAC2_REG (0xCC)
 *       pads 19..21  -> RTC_IO_RTC_PAD19..21_REG (0xD0..0xD8)
 *       common fields: FUN_IE [13], SLP_OE [14], SLP_IE [15], SLP_SEL [16],
 *       FUN_SEL [18:17], MUX_SEL [19] (1 = RTC function owns the pad),
 *       RUE [27], RDE [28], DRV [30:29]; TOUCH pads add XPD/TIE_OPT/START
 *       [22:20], DAC pads add DAC [10:3], XPD_DAC [11], DAC_XPD_FORCE [12]
 *   0xDC RTC_IO_EXT_WAKEUP0_REG (SEL [31:27], stored)
 *   0xE0 RTC_IO_XTL_EXT_CTR_REG (SEL [31:27], stored)
 *   0xE4 RTC_IO_SAR_I2C_IO_REG  (stored)
 *   0xE8 RTC_IO_TOUCH_CTRL_REG  (stored)
 *   0x1FC RTC_IO_DATE_REG       version (read-only)
 *
 * Boundaries (documented, not modelled): deep-sleep hold, the touch/analog
 * sense path, the RTC-main/RTC-slow power domains, and the aggregation of
 * RTC GPIO interrupts into RTC_CNTL (RTC_CNTL_INT_ST_RTC_GPIO, which the
 * rtc_cntl device owns).
 *
 * Copyright (c) 2024-2026 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"

#define TYPE_ESP32S3_RTC_IO "esp32s3.rtc_io"
#define ESP32S3_RTC_IO(obj) OBJECT_CHECK(ESP32S3RtcIoState, (obj), TYPE_ESP32S3_RTC_IO)

#define ESP32S3_RTC_IO_REG_SIZE     0x200
#define ESP32S3_RTC_IO_GPIO_COUNT   22

/* RTC IO MUX base: inside the RTC_CNTL APB window, per TRM ch.4/ch.6 */
#define DR_REG_RTC_IO_MUX_BASE      0x60008400

/* RTC data fields (OUT/ENABLE/STATUS/IN) sit in bits [31:10] */
#define ESP32S3_RTC_IO_DATA_SHIFT   10
#define ESP32S3_RTC_IO_DATA_MASK    0xFFFFFC00u

/* ------------------------------------------------------------------ */
/*  Register offsets                                                   */
/* ------------------------------------------------------------------ */

#define RTC_IO_OUT_OFF            0x00
#define RTC_IO_OUT_W1TS_OFF       0x04
#define RTC_IO_OUT_W1TC_OFF       0x08
#define RTC_IO_ENABLE_OFF         0x0C
#define RTC_IO_ENABLE_W1TS_OFF    0x10
#define RTC_IO_ENABLE_W1TC_OFF    0x14
#define RTC_IO_STATUS_OFF         0x18
#define RTC_IO_STATUS_W1TS_OFF    0x1C
#define RTC_IO_STATUS_W1TC_OFF    0x20
#define RTC_IO_IN_OFF             0x24

/* RTC GPIO pin config: 22 registers at 0x28..0x7C (stride 4) */
#define RTC_IO_PIN_OFF(n)         (0x28 + (n) * 0x04)
#define RTC_IO_PIN_LAST_OFF       (0x28 + 21 * 0x04)  /* 0x7C */

/* Debug select */
#define RTC_IO_DEBUG_SEL_OFF      0x80

/* Per-pad config register offset for RTC pad n (0..21) */
#define RTC_IO_PAD_OFF(n) \
    ((n) == 0 ? 0xBC : 0x84 + ((n) - 1) * 0x04)
#define RTC_IO_PAD_LAST_OFF       0xD8

/* Misc registers */
#define RTC_IO_EXT_WAKEUP0_OFF    0xDC
#define RTC_IO_XTL_EXT_CTR_OFF    0xE0
#define RTC_IO_SAR_I2C_IO_OFF     0xE4
#define RTC_IO_TOUCH_CTRL_OFF     0xE8

/* Version */
#define RTC_IO_DATE_OFF           0x1FC

/* Per-pad config reset default: DRV = 2, RDE = 1 (TRM default column) */
#define ESP32S3_RTC_IO_PAD_DEFAULT ((0x2 << 29) | (0x1 << 28))

/* RTC_IO_DATE version value */
#define ESP32S3_RTC_IO_DATE_VERSION 0x1905260

/*
 * Pad net resolution diagnostics (same policy as the digital GPIO model:
 * floating is reported, never folded into a valid low).
 */
typedef enum {
    ESP32S3_RTC_IO_PAD_FLOATING = 0,
    ESP32S3_RTC_IO_PAD_DRIVEN,
    ESP32S3_GPIO_RTC_PAD_EXTERNAL,
    ESP32S3_RTC_IO_PAD_PULLED_HIGH, /* RUE resolved the net */
    ESP32S3_RTC_IO_PAD_PULLED_LOW,  /* RDE resolved the net */
    ESP32S3_RTC_IO_PAD_CONTENTION,  /* RTC_GPIO drive vs valid external
                                     * driver at a different level */
    ESP32S3_RTC_IO_PAD_UNKNOWN,     /* solver attached but unresolved */
} ESP32S3RtcIoPadState;

/*
 * RAW effective drive configuration of one RTC pad for the net solver —
 * register truth, separate from the resolved rtcio-out line.
 */
typedef struct ESP32S3RtcIoDriveSnapshot {
    bool mux_sel;            /* RTC_IO pad MUX_SEL: 1 = RTC function owns */
    bool out_bit;            /* RTC_GPIO_OUT data bit */
    bool out_oe;             /* RTC_GPIO_ENABLE bit */
    bool open_drain;         /* RTC_GPIO_PINn.PAD_DRIVER */
    unsigned int_type;       /* RTC_GPIO_PINn.INT_TYPE */
    bool fun_ie;             /* pad FUN_IE input-buffer enable */
    bool rue, rde;           /* pad pull-up / pull-down enables */
    unsigned drv;            /* pad drive strength (stored, inert) */
    bool slp_sel;            /* pad SLP_SEL (stored) */
    bool xpd;                /* TOUCH/DAC XPD bit (stored, no analog path) */
    bool hold;               /* always false: deep-sleep hold is a boundary */
} ESP32S3RtcIoDriveSnapshot;

/* RTC_IO raw-truth change notifier type (see esp32s3_rtc_io_set_drive_observer) */
typedef void (*ESP32S3RtcIoDriveObserver)(void *opaque);

/* ------------------------------------------------------------------ */
/*  Device state                                                       */
/* ------------------------------------------------------------------ */

typedef struct ESP32S3RtcIoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* GPIO output / enable / status (bits [31:10]); input is sampled */
    uint32_t out;
    uint32_t enable;
    uint32_t status;
    uint32_t in;

    /* Per-pin config (22 pins) */
    uint32_t pin[ESP32S3_RTC_IO_GPIO_COUNT];

    /* Per-pad config (22 pads) */
    uint32_t pad[ESP32S3_RTC_IO_GPIO_COUNT];

    /* Misc */
    uint32_t debug_sel;
    uint32_t ext_wakeup0;
    uint32_t xtl_ext_ctr;
    uint32_t sar_i2c_io;
    uint32_t touch_ctrl;

    /* Electrical view */
    ESP32S3RtcIoPadState diag[ESP32S3_RTC_IO_GPIO_COUNT];
    bool net_valid[ESP32S3_RTC_IO_GPIO_COUNT];
    bool solver_attached[ESP32S3_RTC_IO_GPIO_COUNT];
    bool solver_owned;
    ESP32S3RtcIoDriveObserver drive_observer;
    void *observer_opaque;
    DeviceState *soc_reset;                     /* link: "soc-reset" */
    uint8_t ext_level[ESP32S3_RTC_IO_GPIO_COUNT];
    uint8_t sample[ESP32S3_RTC_IO_GPIO_COUNT];
    uint8_t line_level[ESP32S3_RTC_IO_GPIO_COUNT];
    qemu_irq input_lines[ESP32S3_RTC_IO_GPIO_COUNT];
    qemu_irq output_lines[ESP32S3_RTC_IO_GPIO_COUNT];
} ESP32S3RtcIoState;

typedef struct ESP32S3RtcIoClass {
    SysBusDeviceClass parent_class;
} ESP32S3RtcIoClass;

/* Named qdev GPIO arrays */
#define ESP32S3_RTC_IO_INPUT_LINES   "rtcio-in"
#define ESP32S3_RTC_IO_OUTPUT_LINES  "rtcio-out"

/*
 * Resolved level of RTC pad n (0..21): the RTC_GPIO output when the pad
 * drives, otherwise the external driver, otherwise 0.  Exposed for the
 * future net-layer integration.
 */
uint32_t esp32s3_rtc_io_pad_level(ESP32S3RtcIoState *s, unsigned pad);

/* Solver entry point: drive a RTC pad net with validity (and inert voltage). */
void esp32s3_rtc_io_drive_net(ESP32S3RtcIoState *s, unsigned pad,
                              bool valid, bool level, double volts);

/* Resolution diagnostics / net validity for RTC pad @pad. */
ESP32S3RtcIoPadState esp32s3_rtc_io_pad_state(ESP32S3RtcIoState *s, unsigned pad);
bool esp32s3_rtc_io_net_valid(ESP32S3RtcIoState *s, unsigned pad);

/* RAW effective drive configuration of RTC pad @pad (net-solver view). */
void esp32s3_rtc_io_get_drive_snapshot(ESP32S3RtcIoState *s, unsigned pad,
                                       ESP32S3RtcIoDriveSnapshot *out);

/*
 * Raw-truth change notifier and strict-sampling gate — same contract as
 * the digital GPIO model (see esp32s3_gpio.h); RTC pad validity enum:
 * ESP32S3_RTC_IO_PAD_UNKNOWN when the solver is attached but unresolved.
 */
void esp32s3_rtc_io_set_drive_observer(ESP32S3RtcIoState *s,
                                   ESP32S3RtcIoDriveObserver fn, void *opaque);
bool esp32s3_rtc_io_solver_attached(ESP32S3RtcIoState *s, unsigned pad);
void esp32s3_rtc_io_set_solver_owned(ESP32S3RtcIoState *s, bool owned);
