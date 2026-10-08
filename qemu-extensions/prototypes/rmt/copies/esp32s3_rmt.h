/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ESP32S3_RMT_H
#define HW_TIMER_ESP32S3_RMT_H

#include "hw/sysbus.h"
#include "hw/clock.h"
#include "qemu/timer.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/dma/esp_gdma.h"
#include "hw/misc/esp32s3_electrical.h"

#define TYPE_ESP32S3_RMT "esp32s3.rmt"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3RmtState, ESP32S3_RMT)
#define ESP32S3_RMT_SYMBOLS 48
#define ESP32S3_RMT_MEMORY_WORDS (8 * ESP32S3_RMT_SYMBOLS)

typedef struct ESP32S3RmtTx {
    ESP32S3RmtState *parent;
    unsigned index;
    QEMUTimer *timer;
    uint32_t conf, applied, carrier, limit, status;
    uint32_t symbol, dma_channel;
    unsigned cursor, fifo, half, threshold_count, loops;
    bool active, armed, level, carrier_level, carrier_running, idle_carrier;
    bool dma_bound, word_pending;
    /* Q32 nanoseconds retain fractional phase until the scheduling boundary. */
    __uint128_t deadline, carrier_deadline;
    uint64_t period, carrier_period;
} ESP32S3RmtTx;

typedef struct ESP32S3RmtRx {
    ESP32S3RmtState *parent;
    unsigned index;
    QEMUTimer *idle_timer, *filter_timer;
    uint32_t conf0, conf1, applied0, applied1, carrier, limit, status;
    uint32_t dma_channel;
    unsigned cursor, fifo, half, threshold_count;
    bool active, valid, level, started, candidate_level, candidate;
    bool dma_bound;
    int64_t qualify_ns, counter_ns;
    __uint128_t elapsed_ticks_fp, qualify_deadline_fp;
    uint64_t period, filter_period;
} ESP32S3RmtRx;

struct ESP32S3RmtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ESP32S3GPIOState *gpio;
    ESPGdmaState *gdma;
    DeviceState *electrical;
    DeviceState *soc_reset;
    Clock *apb_clk, *xtal_clk, *rc_fast_clk;
    ESP32S3RmtTx tx[4];
    ESP32S3RmtRx rx[4];
    uint32_t ram[ESP32S3_RMT_MEMORY_WORDS];
    uint32_t int_raw, int_ena, sys_conf, tx_sim, date;
    bool gate, reset_held;
    uint8_t silicon_revision;
};
#endif
