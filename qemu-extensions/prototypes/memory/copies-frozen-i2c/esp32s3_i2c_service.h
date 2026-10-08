/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_I2C_SERVICE_H
#define ESP32S3_I2C_SERVICE_H
#include "qemu/osdep.h"
#define S3_I2C_SERVICE_BYTES 256
/* One persistent instance per electrically reachable component, not per bus. */
typedef enum S3I2CServiceKind {
    S3_I2C_SHT21, S3_I2C_EEPROM, S3_I2C_EXTERNAL
} S3I2CServiceKind;
/* Complete callbacks are mandatory; the owner retains opaque until unregister.
 * Electrical route/power validation precedes address(), which must also enforce
 * device-specific reset/PWDN/XCLK readiness. No address-only registration. */
typedef struct S3I2CServiceOps {
    bool (*address)(void *opaque, bool reading, bool restart, int64_t now_ns);
    bool (*write)(void *opaque, uint8_t byte, int64_t now_ns);
    bool (*read)(void *opaque, uint8_t *byte, int64_t now_ns);
    void (*read_ack)(void *opaque, bool nack);
    void (*stop)(void *opaque, int64_t now_ns);
    void (*cancel)(void *opaque);
    void (*power_reset)(void *opaque, int64_t power_on_ns);
    int64_t (*ready_ns)(void *opaque);
} S3I2CServiceOps;
typedef struct S3I2CService {
    S3I2CServiceKind kind;
    const S3I2CServiceOps *ops;
    void *opaque;
    uint8_t memory[S3_I2C_SERVICE_BYTES];
    uint8_t page[16], page_base;
    uint16_t page_mask;
    uint8_t pointer, command, user_register, response[3], response_pos;
    uint16_t sample_number;
    uint32_t write_count;
    int64_t ready_ns;
    bool addressed, reading, pointer_pending, write_pending, conversion_pending;
    bool final_nack, stopped_after_pointer;
} S3I2CService;
void s3_i2c_service_init(S3I2CService *s, S3I2CServiceKind kind);
bool s3_i2c_service_init_external(S3I2CService *s,
                                 const S3I2CServiceOps *ops, void *opaque);
/* Return false for modeled address/data NACK. Restart is not STOP. */
bool s3_i2c_service_address(S3I2CService *s, bool reading, bool restart, int64_t now_ns);
bool s3_i2c_service_write(S3I2CService *s, uint8_t byte, int64_t now_ns);
bool s3_i2c_service_read(S3I2CService *s, uint8_t *byte, int64_t now_ns);
void s3_i2c_service_read_ack(S3I2CService *s, bool nack);
void s3_i2c_service_stop(S3I2CService *s, int64_t now_ns);
void s3_i2c_service_cancel(S3I2CService *s);
/* Actual electrical power loss/return invalidates volatile state, not EEPROM
 * nonvolatile storage. SHT21 power-on readiness is 15ms after now_ns. */
void s3_i2c_service_power_reset(S3I2CService *s, int64_t now_ns);
/* Ready time for SHT21 hold-master conversion; no wall-clock sleeps. */
int64_t s3_i2c_service_ready_ns(const S3I2CService *s);
#endif
