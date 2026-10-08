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

/* Native slave edge FSM states. Only electrically published frames advance
 * them; no firmware-visible shortcut bypasses the shared SDA/SCL nets. */
enum {
    S3_I2C_SLV_IDLE = 0,
    S3_I2C_SLV_ADDR,
    S3_I2C_SLV_ADDR2,
    S3_I2C_SLV_IN,
    S3_I2C_SLV_OUT,
};

/* Stretch causes as published in SR[15:14]; 3 means "no active stretch". */
enum {
    S3_I2C_STRETCH_NONE = 3,
    S3_I2C_STRETCH_ADDR = 0,
    S3_I2C_STRETCH_TX = 1,
    S3_I2C_STRETCH_RX = 2,
};

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
    /* Multi-master arbitration: the address byte is transmitted bit by bit
     * against the shared net; a released high read low is a lost arbitration. */
    unsigned addr_bit;
    uint8_t addr_byte;
    /* Slave-mode edge FSM state. */
    unsigned slave_state, slave_bit;
    uint8_t slave_byte;
    bool slave_matched, slave_hdr_matched, slave_rw;
    bool slave_nacked;
    bool slave_out_armed, slave_sda_low, slave_sda_ack, slave_stretch;
    uint8_t slave_cause;
    uint32_t slave_rw_point;
    int64_t slave_low_since;
    bool sl_sda, sl_scl;         /* accepted (glitch-filtered) line levels */
    bool sl_sda_raw, sl_scl_raw; /* pending raw candidates */
    int64_t sl_sda_since, sl_scl_since;
    int64_t slave_filter_deadline, slave_protect_deadline;
};
void esp32s3_i2c_bind(ESP32S3I2CState *s, const S3I2CProvider *provider);
void esp32s3_i2c_invalidate(ESP32S3I2CState *s);
/* One electrical frame was published: run the slave edge FSM from the actual
 * sampled SDA/SCL levels of this controller's routed pads. */
void esp32s3_i2c_slave_edge(ESP32S3I2CState *s);
#endif
