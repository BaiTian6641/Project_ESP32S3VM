/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ESP32S3_MCPWM_H
#define HW_ESP32S3_MCPWM_H
#include "hw/sysbus.h"
#include "hw/clock.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "qemu/timer.h"
#define TYPE_ESP32S3_MCPWM "esp32s3.mcpwm"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3McpwmState, ESP32S3_MCPWM)
/* One instance is one complete group. All phases use source cycles * 1e9,
 * retaining the sub-cycle remainder rather than rounding every edge to ns. */
typedef struct S3McpwmTimer {
    uint64_t phase;
    uint16_t count, period, prescale;
    bool running, down;
} S3McpwmTimer;
typedef struct S3McpwmDelay {
    uint64_t remaining, carry;
    bool input, output, pending;
} S3McpwmDelay;
typedef struct S3McpwmCarrier {
    uint64_t remaining;
    bool input, output;
} S3McpwmCarrier;
typedef struct S3McpwmOperator {
    uint16_t compare[2], delay[2];
    uint32_t actions[2], force;
    uint64_t carry[2];
    bool generator[2], brake_level[2], cbc_level[2], ost_level[2];
    bool cbc, ost;
    S3McpwmDelay edge[2];
    S3McpwmCarrier carrier[2];
} S3McpwmOperator;
struct ESP32S3McpwmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ESP32S3GPIOState *gpio;
    DeviceState *electrical, *soc_reset;
    Clock *apb_clk, *pll_clk;
    QEMUTimer *event;
    uint32_t group, reg[0x128 / 4];
    S3McpwmTimer timer[3];
    S3McpwmOperator oper[3];
    uint64_t pll_hz, apb_hz, capture_phase, event_carry;
    uint64_t group_phase, fault_remaining;
    uint32_t fault_pending, fault_events[3], sync_mask;
    int64_t last_ns;
    uint32_t capture_count;
    uint16_t capture_prescale[3];
    bool input[9], input_valid[9], capture_divided[3];
    bool enabled, reset_held, subscribed, updating, input_pending, realized, publish_all;
    bool dependency;
};
#endif
