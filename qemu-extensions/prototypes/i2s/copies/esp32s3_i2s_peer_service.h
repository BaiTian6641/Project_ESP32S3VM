/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_ESP32S3_I2S_PEER_SERVICE_H
#define HW_MISC_ESP32S3_I2S_PEER_SERVICE_H

#include "hw/qdev-core.h"

/* /machine/soc/i2s-sample-peers QOM export:
 *   capture-request-json (write-only string):
 *     {"componentId": string, "offset": uint, "count": uint in 1..1024,
 *      "transitions": optional {"controllerId": 0|1, "peerOffset": uint,
 *                                "controllerOffset": uint, "count": 1..1024}}
 *   capture-json (read-only string):
 *     {"version":1, "kind":"i2s-peer-din-capture",
 *      "component_id":string, "config_identity":canonical SHA256,
 *      "offset":uint, "total":uint, "events":[
 *        {"ns":int64, "sequence":uint64, "sample":uint32,
 *         "slot":uint16, "raw":bool}], "status":actual peer status}
 *   status-json (read-only string):
 *     {"version":1, "kind":"i2s-peer-status", "peers":[actual status],
 *      "controllers":[actual metadata summaries plus capture_enabled]}
 * The window contains min(count,total-offset) immutable actual DIN records;
 * offset==total returns an empty window. Missing peers/out-of-range offsets
 * are errors. Successful requests persist until replaced, peer removal or
 * canonical identity change. Failed setters do not replace a prior request.
 * Status includes all C status fields plus actual registered endpoint rails,
 * power-known/powered, epoch, power-on time and generation. Unknown rails and
 * absent errors are null. No source vectors or inferred TX samples export.
 * activation_error means actual construction/subscription/setup failure.
 * A subscribed peer that immediately encounters unknown rails or another
 * runtime dependency is instead started with status.paused/dependency;
 * it remains VM-paused/logged and outputs released, without activation_error.
 * Peer status.transitions gives its getter summary without offset/records.
 * Controller summaries are actual getter fields without offset/records;
 * disabled capture returns only actual controller ID and capture_enabled:false.
 * This cold discovery exposes retained ranges before any page request, so
 * exporters never guess ranges or parse errors to recover overwritten data.
 * Optional transitions adds {"version":1,"peer":window,"controller":window}
 * to capture-json, preserving all DIN fields and all actual startup events.
 * Windows export the exact payload-free C getter fields (peer.h/i2s.h), with
 * records in absolute sequence order. Offsets are absolute record sequences,
 * not ring indices. count is retained count; records length is the bounded
 * requested page length. first_sequence,total,lost,capacity,first_index always
 * expose retention/overwrites. offset==total is an empty page. Lost/out-of-range
 * offsets, missing actual controller or disabled controller capture are errors,
 * never fake empty metadata. Actual SOC i2s0/i2s1 C controller ID must match the
 * requested controllerId; neither geometry nor canonical peer identity selects
 * a controller. Peer config activation and native power origins/generation
 * accompany actual source epochs/IDs. No source values occur in transitions.
 * BEGIN is successful outgoing publication; COMPLETE is actual outgoing
 * serial/physical boundary completion, independent of DIN success. Raw edges
 * are physical WS halfphases, not the later master BCLK DIN sampling rise.
 * Queries allocate JSON only while explicitly exporting, never write disk or
 * advance/reset hardware. Getter storage is borrowed and read synchronously.
 */

/* Call once after electrical_create, before any project Apply. The SOC owns
 * the returned child; the service holds the electrical provider alive until
 * its factory, observers, peer timers and terminal drives are detached. */
Object *esp32s3_i2s_peer_service_create(Object *soc, DeviceState *electrical);

#endif
