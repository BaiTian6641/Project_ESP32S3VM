/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_OV2640_SERVICE_H
#define ESP32S3_OV2640_SERVICE_H
#include "hw/qdev-core.h"
/* Creates one SOC-owned native external-service registry. No CAM/GPIO or
 * framebuffer injection interface exists. Configuration is borrowed only
 * during preflight and activation; live slots own their immutable identity. */
void esp32s3_ov2640_service_create(Object *soc, DeviceState *electrical);
#endif
