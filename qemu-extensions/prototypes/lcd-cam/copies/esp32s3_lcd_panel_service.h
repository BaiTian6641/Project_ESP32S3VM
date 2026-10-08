/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_ESP32S3_LCD_PANEL_SERVICE_H
#define HW_MISC_ESP32S3_LCD_PANEL_SERVICE_H
#include "hw/qdev-core.h"

/* Call once after electrical_create and before project Apply. The SOC owns
 * /soc/lcd-panels and its registered factories/solved-frame subscription.
 * There is deliberately no controller pixel or framebuffer input API.
 *
 * QOM status-json: registered physical endpoints, lifetime counters/errors,
 * power epoch and actual decoder state. capture-request-json accepts exactly
 * {"componentId":string,"offset":uint,"count":1..64}; offsets are absolute
 * zero-based completed-frame sequences. Successful requests freeze metadata
 * until replaced; removed/identity-changed components invalidate requests.
 * Older-than-retained offsets error instead of hiding missing history.
 * framebuffer-request-json accepts exactly {"componentId":string,
 * "sequence":uint,"offset":uint,"count":1..65536}. The sequence must select
 * the latest capture or the previously frozen same-component capture.
 * The first setter copies that frame; later byte windows remain immutable.
 * framebuffer-json returns RGB888 hex bytes, timestamps, dimensions, SHA256.
 * RGB scanouts, including malformed ones, are recorded at VSYNC boundaries.
 * ST7789 captures occur at completed RAMWR address-window wraps.
 * No project-controlled host paths or files exist. A host/frontend may append
 * these immutable JSON windows and their decoded binary bytes to evidence.
 * Memory is bounded: two RGB888 buffers and 64 metadata records per panel,
 * plus one service-wide explicit-query frame snapshot and metadata snapshot.
 * Ring eviction is explicit (evicted_count / first_retained_sequence);
 * rendering or observation never stops or changes guest execution.
 */
void esp32s3_lcd_panel_service_create(Object *soc, DeviceState *electrical);
#endif
