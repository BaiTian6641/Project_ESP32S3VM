/*
 * ESP32-S3 IO MUX peripheral
 *
 * Per-pad configuration registers at DR_REG_IO_MUX_BASE (0x60009000):
 *   IO_MUX_PIN_CTRL_REG @ 0x00 (stored only)
 *   IO_MUX_GPIOn_REG    @ 0x04 + 4*n, n = 0..48
 *   IO_MUX_DATE_REG     @ 0xFC
 *
 * Field layout per the ESP32-S3 TRM ch.6 register summary and the ESP-IDF 6.1
 * header components/soc/esp32s3/register/soc/io_mux_reg.h:
 *   bit 0     SLP_OE      output enable in sleep mode (stored, no sleep model)
 *   bit 1     SLP_SEL     sleep-mode select (stored)
 *   bit 2     SLP_PD      pull-down in sleep mode (stored)
 *   bit 3     SLP_PU      pull-up in sleep mode (stored)
 *   bit 4     SLP_IE      input enable in sleep mode (stored)
 *   bit 5-6   SLP_DRV     drive strength in sleep mode (stored)
 *   bit 7     FUN_PD      function pull-down enable (drives the digital line)
 *   bit 8     FUN_PU      function pull-up enable (drives the digital line)
 *   bit 9     FUN_IE      function input enable (gates GPIO_IN sampling)
 *   bit 10-11 FUN_DRV     function drive strength (stored; electrically inert)
 *   bit 12-14 MCU_SEL     pad function: 0 = GPIO matrix, != 0 = IO_MUX function
 *   bit 15    FILTER_EN   input filter (stored; timing-neutral in this model)
 *
 * Reset value of every pad register is FUN_DRV=2 (20 mA), i.e. 0x800, per the
 * TRM default column.  Pad-specific boot defaults for the SPI-flash pads
 * (GPIO26-32) are not modelled; the ROM/bootloader re-programs them.
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
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

#define TYPE_ESP32S3_IOMUX "esp32s3.iomux"
#define ESP32S3_IOMUX(obj) OBJECT_CHECK(ESP32S3IOMuxState, (obj), TYPE_ESP32S3_IOMUX)

/* ESP32-S3 has 49 GPIO pad registers (GPIO0 - GPIO48; pads 22..25 are holes) */
#define ESP32S3_IOMUX_GPIO_COUNT   49

/* Register offsets */
REG32(IO_MUX_PIN_CTRL, 0x0000)
    FIELD(IO_MUX_PIN_CTRL, CLK1, 0, 4)
    FIELD(IO_MUX_PIN_CTRL, CLK2, 4, 4)
    FIELD(IO_MUX_PIN_CTRL, CLK3, 8, 4)

#define IO_MUX_GPIOn_REG_OFFSET(n) (0x0004 + (n) * 4)

/* Fields within each IO_MUX_GPIOn_REG */
REG32(IO_MUX_GPIOn, 0x0004)
    FIELD(IO_MUX_GPIOn, SLP_OE, 0, 1)
    FIELD(IO_MUX_GPIOn, SLP_SEL, 1, 1)
    FIELD(IO_MUX_GPIOn, SLP_PD, 2, 1)
    FIELD(IO_MUX_GPIOn, SLP_PU, 3, 1)
    FIELD(IO_MUX_GPIOn, SLP_IE, 4, 1)
    FIELD(IO_MUX_GPIOn, SLP_DRV, 5, 2)
    FIELD(IO_MUX_GPIOn, FUN_PD, 7, 1)
    FIELD(IO_MUX_GPIOn, FUN_PU, 8, 1)
    FIELD(IO_MUX_GPIOn, FUN_IE, 9, 1)
    FIELD(IO_MUX_GPIOn, FUN_DRV, 10, 2)
    FIELD(IO_MUX_GPIOn, MCU_SEL, 12, 3)
    FIELD(IO_MUX_GPIOn, FILTER_EN, 15, 1)

/*
 * MCU_SEL value that routes the pad through the GPIO matrix.  On the
 * ESP32-S3 this is 1 (IDF PIN_FUNC_GPIO, io_mux_reg.h:140) — *not* 0 as on
 * the classic ESP32; most pads use function 0 for a named direct peripheral
 * (e.g. FUNC_U0TXD_U0TXD = 0 on GPIO43).  MCU_SEL != 1 hands the pad to a
 * direct IO_MUX function.
 */
#define ESP32S3_IOMUX_MCU_SEL_GPIO  1

/* Version register */
REG32(IO_MUX_DATE, 0x00FC)

/* Total register region size */
#define ESP32S3_IOMUX_REGS_SIZE     0x100

/* Named qdev line pulsing on pad register writes */
#define ESP32S3_IOMUX_CHANGE_IRQ   "pad-change"

/* Per-pad reset value: FUN_DRV = 2 (20 mA) */
#define ESP32S3_IOMUX_GPIO_REG_DEFAULT  (0x2 << R_IO_MUX_GPIOn_FUN_DRV_SHIFT)

/* IO_MUX_DATE version value (from TRM) */
#define ESP32S3_IOMUX_DATE_VERSION      0x2006050

typedef struct ESP32S3IOMuxState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Pin control register */
    uint32_t pin_ctrl;

    /* Per-GPIO pad configuration registers */
    uint32_t gpio_reg[ESP32S3_IOMUX_GPIO_COUNT];

    /* Date/version register */
    uint32_t date_reg;

    /* Pulsed on every pad register write so the GPIO model re-resolves
     * the affected pads (named output, one line) */
    qemu_irq notify;
    DeviceState *soc_reset;                     /* link: "soc-reset" */
} ESP32S3IOMuxState;

static inline uint32_t esp32s3_iomux_get(ESP32S3IOMuxState *s, unsigned pad)
{
    return pad < ESP32S3_IOMUX_GPIO_COUNT ? s->gpio_reg[pad] : 0;
}
