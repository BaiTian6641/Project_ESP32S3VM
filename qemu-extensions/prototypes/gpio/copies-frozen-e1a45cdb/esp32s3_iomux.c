/*
 * ESP32-S3 IO MUX peripheral
 *
 * Register store for the 49 IO_MUX_GPIOn pad registers with TRM reset
 * defaults.  The pad function select (MCU_SEL), the input enable (FUN_IE)
 * and the internal pulls (FUN_PU / FUN_PD) are consumed by the GPIO model
 * through the "iomux" link property; drive strength is stored but is
 * electrically inert until the analog solver gate (documented boundary).
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 ESP32S3VM project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32s3_iomux.h"
#if defined(__has_include)
#if __has_include("hw/xtensa/esp32s3_reset_domain.h")
#include "hw/xtensa/esp32s3_reset_domain.h"
#define ESP32S3_IOMUX_RESET_DOMAIN 1
#endif
#endif

#define IOMUX_WARNING 0

/* Pad register write mask: bits [15:0] are implemented */
#define IOMUX_PAD_WRITE_MASK 0x0000FFFF

static bool esp32s3_iomux_pad_addr(hwaddr addr, unsigned *pad)
{
    if (addr < IO_MUX_GPIOn_REG_OFFSET(0) ||
        addr > IO_MUX_GPIOn_REG_OFFSET(ESP32S3_IOMUX_GPIO_COUNT - 1) ||
        ((addr - IO_MUX_GPIOn_REG_OFFSET(0)) % 4) != 0) {
        return false;
    }
    *pad = (addr - IO_MUX_GPIOn_REG_OFFSET(0)) / 4;
    return true;
}

static uint64_t esp32s3_iomux_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(opaque);
    uint64_t r = 0;
    unsigned pad;

    switch (addr) {
    case A_IO_MUX_PIN_CTRL:
        r = s->pin_ctrl;
        break;

    case A_IO_MUX_DATE:
        r = s->date_reg;
        break;

    default:
        if (esp32s3_iomux_pad_addr(addr, &pad)) {
            r = esp32s3_iomux_get(s, pad);
        } else if (IOMUX_WARNING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad read at offset 0x%04" HWADDR_PRIx "\n",
                          __func__, addr);
        }
        break;
    }

    return r;
}

static void esp32s3_iomux_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(opaque);
    unsigned pad;

    switch (addr) {
    case A_IO_MUX_PIN_CTRL:
        s->pin_ctrl = (uint32_t)value & 0x00000FFF; /* CLK1/2/3, 4 bits each */
        break;

    case A_IO_MUX_DATE:
        /* Version register is read-only */
        break;

    default:
        if (esp32s3_iomux_pad_addr(addr, &pad)) {
            s->gpio_reg[pad] = (uint32_t)value & IOMUX_PAD_WRITE_MASK;
            /* Let the GPIO consumer re-resolve the pad nets */
            qemu_set_irq(s->notify, 0);
            qemu_set_irq(s->notify, 1);
        } else if (IOMUX_WARNING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write at offset 0x%04" HWADDR_PRIx "\n",
                          __func__, addr);
        }
        break;
    }
}

static const MemoryRegionOps esp32s3_iomux_ops = {
    .read = esp32s3_iomux_read,
    .write = esp32s3_iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void esp32s3_iomux_reset_hold(Object *obj, ResetType type)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(obj);

#ifdef ESP32S3_IOMUX_RESET_DOMAIN
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) {
        return;
    }
#endif
    s->pin_ctrl = 0;
    for (int i = 0; i < ESP32S3_IOMUX_GPIO_COUNT; i++) {
        s->gpio_reg[i] = ESP32S3_IOMUX_GPIO_REG_DEFAULT;
    }
    s->date_reg = ESP32S3_IOMUX_DATE_VERSION;
}

static void esp32s3_iomux_init(Object *obj)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_iomux_ops, s,
                          TYPE_ESP32S3_IOMUX, ESP32S3_IOMUX_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->notify, ESP32S3_IOMUX_CHANGE_IRQ, 1);
    object_property_add_link(OBJECT(obj), "soc-reset", TYPE_DEVICE,
                             (Object **)&s->soc_reset,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static void esp32s3_iomux_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_iomux_reset_hold;
    dc->desc = "ESP32-S3 IO MUX";
}

static const TypeInfo esp32s3_iomux_info = {
    .name = TYPE_ESP32S3_IOMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3IOMuxState),
    .instance_init = esp32s3_iomux_init,
    .class_init = esp32s3_iomux_class_init,
};

static void esp32s3_iomux_register_types(void)
{
    type_register_static(&esp32s3_iomux_info);
}

type_init(esp32s3_iomux_register_types);
