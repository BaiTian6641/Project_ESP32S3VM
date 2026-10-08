/*
 * ESP32 GPIO emulation (common parent for the Espressif GPIO models)
 *
 * The default register file only implements the GPIO_STRAP register that is
 * shared by every Espressif SoC.  SoC specific models (ESP32, ESP32-S3,
 * ESP32-C3, ...) subclass this device and override the gpio_read/gpio_write
 * class handlers.
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"

static uint64_t esp32_gpio_read_default(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;

    switch (addr) {
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;

    default:
        break;
    }
    return r;
}

static void esp32_gpio_write_default(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned int size)
{
}

/* Virtual dispatch wrappers for MemoryRegionOps */
static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioClass *klass = ESP32_GPIO_GET_CLASS(opaque);
    return klass->gpio_read(opaque, addr, size);
}

static void esp32_gpio_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned int size)
{
    Esp32GpioClass *klass = ESP32_GPIO_GET_CLASS(opaque);
    klass->gpio_write(opaque, addr, value, size);
}

static const MemoryRegionOps gpio_ops = {
    .read =  esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
}

static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT, &error_fatal);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s,
                          TYPE_ESP32_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static Property esp32_gpio_properties[] = {
    /* The strap_mode needs to be explicitly set in the instance init, thus, set
     * the default value to 0. */
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    Esp32GpioClass *gc = ESP32_GPIO_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);

    /* Default virtual methods — children override */
    gc->gpio_read = esp32_gpio_read_default;
    gc->gpio_write = esp32_gpio_write_default;
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
