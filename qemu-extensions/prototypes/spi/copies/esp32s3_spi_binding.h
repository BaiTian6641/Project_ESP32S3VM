/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_ESP32S3_SPI_BINDING_H
#define HW_SSI_ESP32S3_SPI_BINDING_H
#include "hw/misc/esp32s3_gpspi.h"
#include "hw/gpio/esp32s3_gpio.h"
void esp32s3_spi_bind_electrical(ESP32S3GpSpiState *a, ESP32S3GpSpiState *b,
                                ESP32S3GPIOState *gpio, DeviceState *electrical);
#endif
