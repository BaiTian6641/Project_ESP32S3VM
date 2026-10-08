/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP32S3_ELECTRICAL_OBSERVERS_H
#define ESP32S3_ELECTRICAL_OBSERVERS_H

#include <stdbool.h>
#include <stdint.h>

#define ESP32S3_ELECTRICAL_OBSERVER_CAPACITY 32

typedef void (*ESP32S3ElectricalObserverNotify)(void *opaque);
typedef void (*ESP32S3ElectricalObserverTimedNotify)(void *opaque, uint64_t ns);

typedef enum ESP32S3ElectricalObserverKind {
    ESP32S3_ELECTRICAL_OBSERVER_MODEL,
    ESP32S3_ELECTRICAL_OBSERVER_TIMED,
} ESP32S3ElectricalObserverKind;

typedef struct ESP32S3ElectricalObserverSlot {
    uint64_t serial;
    void *opaque;
    ESP32S3ElectricalObserverKind kind;
    union {
        ESP32S3ElectricalObserverNotify model;
        ESP32S3ElectricalObserverTimedNotify timed;
    } notify;
} ESP32S3ElectricalObserverSlot;

/* Embed by value and initialize to zero. No allocation or teardown is needed.
 * The fields are helper-owned; only slots[0..count) are live. All calls require
 * BQL serialization. The set must remain alive and must not be reset until an
 * active publish returns; callback opaque objects need only remain alive until
 * their matching registration is removed (and any active callback returns).
 */
typedef struct ESP32S3ElectricalObserverSet {
    ESP32S3ElectricalObserverSlot slots[ESP32S3_ELECTRICAL_OBSERVER_CAPACITY];
    uint64_t last_serial;
    unsigned count;
    bool publishing;
    bool mutated;
} ESP32S3ElectricalObserverSet;

/* Both forms share the 32-slot capacity and one registration order. Identity is
 * (kind, callback, opaque); duplicates succeed without moving or changing the
 * entry, even at capacity. NULL callbacks/sets fail; NULL opaque is valid.
 * A new entry fails at capacity or if its uint64_t monotonic serial is exhausted;
 * serials are never recycled, including after removal. Removal of an absent or
 * invalid registration is a no-op. No helper owns the callback or opaque.
 */
bool esp32s3_electrical_observers_add_model_notify(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverNotify notify,
    void *opaque);
void esp32s3_electrical_observers_remove_model_notify(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverNotify notify,
    void *opaque);
bool esp32s3_electrical_observers_subscribe(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverTimedNotify notify,
    void *opaque);
void esp32s3_electrical_observers_unsubscribe(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverTimedNotify notify,
    void *opaque);

/* Call only AFTER publishing an actual solved frame. Dispatch is in shared
 * registration order with ns passed unchanged to timed callbacks. Each entry
 * present at entry is eligible once: removals immediately cancel future calls,
 * while newly added/re-added entries wait for the next actual frame. Callbacks
 * may remove themselves, earlier/later entries, or add either callback form.
 *
 * Return true after a complete dispatch, including an empty set. NULL sets and
 * reentrant calls return false without invoking callbacks or retaining ns;
 * there is no recursive dispatch, pending-frame queue, or synthetic replay.
 * The caller owns solved-frame phasing and bounded reentrant drive deltas.
 */
bool esp32s3_electrical_observers_publish(ESP32S3ElectricalObserverSet *set,
                                        uint64_t ns);

#endif
