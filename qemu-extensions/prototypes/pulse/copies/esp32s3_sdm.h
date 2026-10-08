/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_SDM_H
#define ESP32S3_SDM_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "qemu/timer.h"
#define TYPE_ESP32S3_SDM "esp32s3-sdm"
#define ESP32S3_SDM_PROFILE "s3-sdm-second-order-transfer-v1"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3SdmState, ESP32S3_SDM)

typedef struct ESP32S3SdmChannel {
    uint32_t reg;
    __uint128_t phase;
    int32_t error1, error2;
    int8_t delayed_input;
    bool level;
} ESP32S3SdmChannel;

struct ESP32S3SdmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DeviceState *electrical, *soc_reset;
    Clock *apb_clk;
    QEMUTimer *event;
    ESP32S3SdmChannel channels[8];
    uint32_t cg, misc, version;
    uint64_t hz;
    int64_t last_ns;
    bool realized;
};
#endif
