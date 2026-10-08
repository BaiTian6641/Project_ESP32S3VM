/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "esp32s3_electrical_observers.h"

static ESP32S3ElectricalObserverSlot *observer_append(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverKind kind,
    void *opaque)
{
    if (set->count == ESP32S3_ELECTRICAL_OBSERVER_CAPACITY ||
        set->last_serial == UINT64_MAX) {
        return NULL;
    }

    ESP32S3ElectricalObserverSlot *slot = &set->slots[set->count++];
    slot->serial = ++set->last_serial;
    slot->opaque = opaque;
    slot->kind = kind;
    if (set->publishing) {
        set->mutated = true;
    }
    return slot;
}

static void observer_remove_at(ESP32S3ElectricalObserverSet *set, unsigned index)
{
    --set->count;
    if (index < set->count) {
        memmove(&set->slots[index], &set->slots[index + 1],
                (set->count - index) * sizeof(set->slots[0]));
    }
    if (set->publishing) {
        set->mutated = true;
    }
}

bool esp32s3_electrical_observers_add_model_notify(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverNotify notify,
    void *opaque)
{
    if (!set || !notify) {
        return false;
    }
    for (unsigned i = 0; i < set->count; ++i) {
        const ESP32S3ElectricalObserverSlot *slot = &set->slots[i];
        if (slot->kind == ESP32S3_ELECTRICAL_OBSERVER_MODEL &&
            slot->notify.model == notify && slot->opaque == opaque) {
            return true;
        }
    }

    ESP32S3ElectricalObserverSlot *slot = observer_append(
        set, ESP32S3_ELECTRICAL_OBSERVER_MODEL, opaque);
    if (!slot) {
        return false;
    }
    slot->notify.model = notify;
    return true;
}

void esp32s3_electrical_observers_remove_model_notify(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverNotify notify,
    void *opaque)
{
    if (!set || !notify) {
        return;
    }
    for (unsigned i = 0; i < set->count; ++i) {
        const ESP32S3ElectricalObserverSlot *slot = &set->slots[i];
        if (slot->kind == ESP32S3_ELECTRICAL_OBSERVER_MODEL &&
            slot->notify.model == notify && slot->opaque == opaque) {
            observer_remove_at(set, i);
            return;
        }
    }
}

bool esp32s3_electrical_observers_subscribe(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverTimedNotify notify,
    void *opaque)
{
    if (!set || !notify) {
        return false;
    }
    for (unsigned i = 0; i < set->count; ++i) {
        const ESP32S3ElectricalObserverSlot *slot = &set->slots[i];
        if (slot->kind == ESP32S3_ELECTRICAL_OBSERVER_TIMED &&
            slot->notify.timed == notify && slot->opaque == opaque) {
            return true;
        }
    }

    ESP32S3ElectricalObserverSlot *slot = observer_append(
        set, ESP32S3_ELECTRICAL_OBSERVER_TIMED, opaque);
    if (!slot) {
        return false;
    }
    slot->notify.timed = notify;
    return true;
}

void esp32s3_electrical_observers_unsubscribe(
    ESP32S3ElectricalObserverSet *set, ESP32S3ElectricalObserverTimedNotify notify,
    void *opaque)
{
    if (!set || !notify) {
        return;
    }
    for (unsigned i = 0; i < set->count; ++i) {
        const ESP32S3ElectricalObserverSlot *slot = &set->slots[i];
        if (slot->kind == ESP32S3_ELECTRICAL_OBSERVER_TIMED &&
            slot->notify.timed == notify && slot->opaque == opaque) {
            observer_remove_at(set, i);
            return;
        }
    }
}

/* Compaction may have moved the cursor's slot or removed it altogether. The
 * serial remains the position in registration order, even after self-removal.
 * Only a callback that actually mutated the set needs this binary re-seek.
 */
static unsigned observer_after(const ESP32S3ElectricalObserverSet *set,
                               uint64_t serial)
{
    unsigned low = 0;
    unsigned high = set->count;

    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (set->slots[middle].serial <= serial) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

bool esp32s3_electrical_observers_publish(ESP32S3ElectricalObserverSet *set,
                                        uint64_t ns)
{
    if (!set || set->publishing) {
        return false;
    }

    const uint64_t cutoff = set->last_serial;
    unsigned index = 0;
    set->publishing = true;
    while (index < set->count) {
        const ESP32S3ElectricalObserverSlot *slot = &set->slots[index];
        const uint64_t serial = slot->serial;
        if (serial > cutoff) {
            break;
        }

        set->mutated = false;
        /* The callable and argument are evaluated before entering the callback.
         * Never access slot after it returns: removal may have compacted it.
         */
        if (slot->kind == ESP32S3_ELECTRICAL_OBSERVER_MODEL) {
            slot->notify.model(slot->opaque);
        } else {
            slot->notify.timed(slot->opaque, ns);
        }
        index = set->mutated ? observer_after(set, serial) : index + 1;
    }
    set->publishing = false;
    set->mutated = false;
    return true;
}
