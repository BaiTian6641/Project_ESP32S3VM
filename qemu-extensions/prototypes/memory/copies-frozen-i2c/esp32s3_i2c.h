/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_ESP32S3_I2C_H
#define HW_I2C_ESP32S3_I2C_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "qemu/fifo8.h"
#include "qemu/timer.h"
#include "hw/i2c/esp32s3_i2c_service.h"
#define TYPE_ESP32S3_I2C "esp32s3-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3I2CState, ESP32S3_I2C)
/* No provider => no reachable device and unresolved lines, never ACK. */
typedef struct S3I2CProvider {
    void *opaque;
    bool (*sample)(void *opaque, unsigned controller, bool *sda, bool *scl);
    void (*drive)(void *opaque, unsigned controller, bool sda_release, bool scl_release);
    S3I2CService *(*resolve)(void *opaque, unsigned controller, uint8_t address,
                             bool *collision);
    void (*slave_drive)(void *opaque, S3I2CService *service, bool sda_release,
                         bool scl_release);
} S3I2CProvider;
struct ESP32S3I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Clock *xtal, *rc_fast;
    DeviceState *soc_reset;
    QEMUTimer *timer;
    Fifo8 tx, rx;
    uint32_t reg[128];
    uint32_t active_cmd[8];
    uint32_t controller;
    uint64_t generation;
    bool gate, reset_asserted, executing, protocol_open, need_address, restart;
    bool reading, nack, waiting, sda, scl;
    unsigned command_index, remaining, bit_phase;
    uint8_t byte, address;
    int64_t wait_start;
    S3I2CService *service;
    S3I2CProvider provider;
};
void esp32s3_i2c_bind(ESP32S3I2CState *s, const S3I2CProvider *provider);
void esp32s3_i2c_invalidate(ESP32S3I2CState *s);
#endif
