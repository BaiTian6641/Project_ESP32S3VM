/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_ELECTRICAL_INTERNAL_H
#define ESP32S3_ELECTRICAL_INTERNAL_H
#include "hw/misc/esp32s3_electrical.h"
#include "esp32s3_project.h"

/* Borrowed current accepted graph/sample: no allocation or secondary graph.
 * sample_at requires the current virtual timestamp and a successful settlement.
 * Callers must not retain either pointer across a topology Apply. */
Esp32S3Project *esp32s3_electrical_project(DeviceState *dev);
const EnRcSample *esp32s3_electrical_sample_at(DeviceState *dev, uint64_t ns);
uint64_t esp32s3_electrical_generation(DeviceState *dev);
bool esp32s3_electrical_model_changed(DeviceState *dev);
void esp32s3_electrical_models_update_power(Esp32S3Project *project,
                                           const EnRcSample *sample, uint64_t ns);
void esp32s3_electrical_models_transfer(const Esp32S3Project *previous,
                                       Esp32S3Project *candidate);
#endif
