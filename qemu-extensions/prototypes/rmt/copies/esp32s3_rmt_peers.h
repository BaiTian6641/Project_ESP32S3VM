/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_ESP32S3_RMT_PEERS_H
#define HW_TIMER_ESP32S3_RMT_PEERS_H

#include "hw/qdev-core.h"

#define TYPE_ESP32S3_RMT_PEERS "esp32s3-rmt-peers"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3RmtPeersState, ESP32S3_RMT_PEERS)
/* One native service consumes the one electrical graph's registered terminal
 * and real rail-epoch API. It does not own MCU routing or create hidden wires.
 * Read-only capture-json exposes actual WS2812 latches and NEC source state.
 * MCU reset is NOT a power cycle of these explicitly powered external peers. */
DeviceState *esp32s3_rmt_peers_create(Object *soc, DeviceState *electrical);

#endif
