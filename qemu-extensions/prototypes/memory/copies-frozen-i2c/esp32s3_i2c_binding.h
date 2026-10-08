/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_ESP32S3_I2C_BINDING_H
#define HW_I2C_ESP32S3_I2C_BINDING_H
#include "hw/i2c/esp32s3_i2c.h"
#include "hw/gpio/esp32s3_gpio.h"
void esp32s3_i2c_bind_electrical(ESP32S3I2CState *a, ESP32S3I2CState *b,
                                ESP32S3GPIOState *gpio, DeviceState *electrical);
/* Register only an actual preflighted component ID from the electrical graph.
 * No bus/address argument exists. Duplicate owner registration is idempotent.
 * Unregister before destroying opaque; matching active transfers are canceled. */
bool esp32s3_i2c_register_service(DeviceState *electrical, const char *component_id,
                                const S3I2CServiceOps *ops, void *opaque);
void esp32s3_i2c_unregister_service(DeviceState *electrical,
                                  const char *component_id, void *opaque);
#endif
