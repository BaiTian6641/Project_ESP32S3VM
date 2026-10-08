/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_LEDC_H
#define ESP32S3_LEDC_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "qemu/timer.h"
#include "hw/gpio/esp32s3_gpio.h"
#define TYPE_ESP32S3_LEDC "esp32s3-ledc"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3LedcState, ESP32S3_LEDC)
#define ESP32S3_LEDC_MMIO_SIZE 0x1000
#define ESP32S3_LEDC_MATRIX_BASE 73
#define ESP32S3_LEDC_IRQ_SOURCE 35

typedef struct ESP32S3LedcTimer {
    uint32_t conf, active, pending;
    uint32_t count;
    uint8_t div_fraction;
    bool update;
    /* Source-clock phase in billionths of a cycle. Retained across gates,
     * pause, clock switches and divider updates; reset alone clears it. */
    __uint128_t source_phase;
} ESP32S3LedcTimer;

typedef struct ESP32S3LedcChannel {
    uint32_t conf0, conf1, hpoint, duty;
    uint32_t active0, active_hpoint, pending0, pending_hpoint;
    uint32_t current_duty, width, fade_conf;
    uint16_t fade_left, fade_cycle, overflows;
    uint8_t duty_fraction;
    bool update, fade, fade_dirty, level;
} ESP32S3LedcChannel;

struct ESP32S3LedcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical;
    DeviceState *soc_reset;
    Clock *apb_clk, *xtal_clk, *rc_fast_clk;
    QEMUTimer *event;
    ESP32S3LedcTimer timers[4];
    ESP32S3LedcChannel channels[8];
    uint32_t conf, int_raw, int_ena, date;
    uint64_t hz;
    int64_t last_ns;
    bool gate, reset_held, realized, publish_all;
};
#endif
