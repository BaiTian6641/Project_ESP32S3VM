/*
 * ESP32-S3 reset-domain helpers
 *
 * Copyright (c) 2026 ESP32S3VM coreclk lane
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 *
 * The ESP32-S3 groups its resets into (TRM v1.8 ch.7 §7.1):
 *   - CPU resets (PROCPU / APPCPU): only the core is reset,
 *   - core/digital (PERIPH) resets: the digital peripherals restart,
 *   - RTC/chip resets: everything, including RTC_CNTL.
 *
 * The SoC records the pending reset domain before requesting the system
 * reset; peripheral devices consult these helpers from their
 * ResettableClass hold phase so that the blanket QEMU device reset only
 * clears guest-visible state when the pending domain actually covers the
 * digital/RTC peripherals.
 */
#ifndef HW_XTENSA_ESP32S3_RESET_DOMAIN_H
#define HW_XTENSA_ESP32S3_RESET_DOMAIN_H

#include "qemu/osdep.h"
#include "hw/qdev-core.h"

/**
 * esp32s3_reset_covers_periph: true when the pending reset on the ESP32-S3
 * SoC @soc clears the digital PERIPH domain (timers, SYSTIMER, GDMA, UART,
 * SPI, TWAI, crypto engines, SYSTEM registers). A pending domain of 0 means
 * power-on/chip reset or an explicit device reset and covers everything.
 */
bool esp32s3_reset_covers_periph(const DeviceState *soc);

/**
 * esp32s3_reset_covers_rtc: true when the pending reset on the ESP32-S3
 * SoC @soc clears the RTC domain (RTC_CNTL registers, RTC_TIME base).
 */
bool esp32s3_reset_covers_rtc(const DeviceState *soc);

#endif /* HW_XTENSA_ESP32S3_RESET_DOMAIN_H */
