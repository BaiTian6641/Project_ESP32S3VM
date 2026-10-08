/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ESP32S3_PCNT_H
#define HW_ESP32S3_PCNT_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/misc/esp32s3_electrical.h"
#include "qemu/timer.h"

#define TYPE_ESP32S3_PCNT "esp32s3-pcnt"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3PcntState, ESP32S3_PCNT)
#define ESP32S3_PCNT_UNITS 4

typedef struct ESP32S3PcntInput {
    /* Filter age in Q32 APB cycles, independent of source-clock changes. */
    uint64_t age;
    bool valid, raw, filtered, pending;
} ESP32S3PcntInput;

typedef struct ESP32S3PcntUnit {
    uint32_t conf[3], applied_thres, applied_limits, status;
    int16_t count;
    /* pulse0, pulse1, control0, control1, in target matrix signal order. */
    ESP32S3PcntInput input[4];
} ESP32S3PcntUnit;

struct ESP32S3PcntState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical, *soc_reset;
    Clock *apb_clk;
    QEMUTimer *filter_timer;
    ESP32S3PcntUnit unit[ESP32S3_PCNT_UNITS];
    uint64_t period, last_ns, cycle_remainder;
    uint32_t ctrl, int_raw, int_ena, date;
    bool gate, reset_held, subscribed;
};
#endif
