/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QOM-controlled host dependency probe for Espressif QEMU 9.2.2.
 * No guest MMIO/firmware API. No native peripheral or cycle-accuracy claim.
 * Responses never resume the VM; release and cont are separate explicit actions.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/unicode.h"
#include "qom/object_interfaces.h"
#include "chardev/char-fe.h"
#include "block/aio.h"
#include "hw/core/cpu.h"
#include "hw/resettable.h"
#include "migration/blocker.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/hostbus-probe.h"
#include "hostbus-json.h"
#include <math.h>

#define TYPE_HOSTBUS_PROBE "esp32s3-hostbus-probe"
OBJECT_DECLARE_SIMPLE_TYPE(HostbusProbe, HOSTBUS_PROBE)
#define FRAME_MAX (1024 * 1024)
#define PAYLOAD_MAX (64 * 1024)
#define TX_MAX (4 * 1024 * 1024)
#define PROBE_DEADLINE_NS INT64_C(1000000000)

struct HostbusProbe {
    Object parent_obj;
    ResettableState reset_state;
    CharBackend chr;
    char *chr_name;
    char *session;
    char *connection;
    char *last_error;
    char *last_payload;
    char *last_status;
    GByteArray *rx;
    GByteArray *tx;
    GHashTable *connections_seen;
    uint8_t prefix[4];
    unsigned prefix_bytes;
    uint32_t expected;
    guint tx_watch;
    QEMUBH *dependency_bh;
    QEMUBH *warp_bh;
    VMChangeStateEntry *vm_state;
    bool engaging;
    QEMUTimer *dependency_timer;
    QEMUTimer *completion_timer;
    QEMUTimer *host_timer;
    QEMUTimer *assembly_timer;
    Error *migration_blocker;
    bool completed;
    bool reset_registered;
    bool connected;
    bool negotiated;
    bool armed;
    bool owns_barrier;
    bool pending;
    bool resolved;
    bool completing;
    bool require_new_session;
    uint64_t epoch;
    uint64_t topology;
    uint64_t minimum_epoch;
    uint64_t minimum_topology;
    uint64_t tx_sequence;
    uint64_t rx_sequence;
    uint64_t ordinal;
    uint64_t late_replies;
    uint32_t consecutive_discarded;
    uint32_t frame_limit;
    uint32_t payload_limit;
    unsigned watchdog_ms;
    int64_t armed_ns;
    int64_t stopped_ns;
    int64_t completion_ns;
    int64_t delivered_ns;
};

static GSList *probes;
static void set_error(HostbusProbe *s, const char *reason);

static void discard(HostbusProbe *s)
{
    if (s->late_replies < UINT64_MAX) {
        ++s->late_replies;
    }
    if (++s->consecutive_discarded >= 256) {
        set_error(s, "Old/duplicate host record limit exceeded");
        qemu_chr_fe_disconnect(&s->chr);
    }
}

bool esp32s3vm_hostbus_resume_blocked(void)
{
    GSList *item;
    for (item = probes; item; item = item->next) {
        HostbusProbe *s = item->data;
        if (s->owns_barrier) {
            return true;
        }
    }
    return false;
}

bool esp32s3vm_hostbus_present(void)
{
    return probes != NULL;
}

static const char *str_field(const QDict *dict, const char *name)
{
    QString *value = qobject_to(QString, qdict_get(dict, name));
    return value ? qstring_get_str(value) : NULL;
}

static bool equals(const QDict *dict, const char *name, const char *text)
{
    const char *value = str_field(dict, name);
    return value && !strcmp(value, text);
}

static bool utf16_bounded(const char *text, size_t limit)
{
    size_t remaining, units = 0;
    if (!text) { return false; }
    remaining = strlen(text);
    while (remaining) {
        char *end;
        int codepoint = mod_utf8_codepoint(text, MIN(remaining, 6), &end);
        if (codepoint < 0) { return false; }
        size_t consumed = end - text;
        if (!consumed || consumed > remaining) { return false; }
        units += codepoint > 0xffff ? 2 : 1;
        if (units > limit) { return false; }
        text = end; remaining -= consumed;
    }
    return true;
}

static bool decimal(const char *text, uint64_t *value)
{
    const char *p;
    if (!text || !*text || strlen(text) > 20 || (text[0] == '0' && text[1])) {
        return false;
    }
    for (p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return qemu_strtou64(text, NULL, 10, value) == 0;
}

static bool decimal_field(const QDict *dict, const char *name, uint64_t *value)
{
    return decimal(str_field(dict, name), value);
}

static bool uuid(const char *text)
{
    unsigned i;
    if (!text || strlen(text) != 36 || !strcmp(text, "00000000-0000-0000-0000-000000000000")) {
        return false;
    }
    for (i = 0; i < 36; ++i) {
        char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool exact_keys(const QDict *dict, const char *const *keys, size_t count)
{
    size_t i;
    if (!dict || qdict_size(dict) != count) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        if (!qdict_haskey(dict, keys[i])) {
            return false;
        }
    }
    return true;
}

static bool number_field(const QDict *dict, const char *name,
                         uint32_t minimum, uint32_t maximum, uint32_t *result)
{
    QNum *number = qobject_to(QNum, qdict_get(dict, name));
    double value;
    if (!number) {
        return false;
    }
    value = qnum_get_double(number);
    if (!isfinite(value) || value != floor(value) || value < minimum || value > maximum) {
        return false;
    }
    *result = value;
    return true;
}

static bool list_one(const QDict *dict, const char *name, const char *text)
{
    QList *list = qobject_to(QList, qdict_get(dict, name));
    const QListEntry *entry;
    QString *value;
    if (!list || qlist_size(list) != 1) {
        return false;
    }
    entry = qlist_first(list);
    value = qobject_to(QString, qlist_entry_obj(entry));
    return value && !strcmp(qstring_get_str(value), text);
}

static bool feature_list(QList *list, GHashTable *features)
{
    const QListEntry *entry;
    if (!list || qlist_size(list) > 32) {
        return false;
    }
    QLIST_FOREACH_ENTRY(list, entry) {
        QString *string = qobject_to(QString, qlist_entry_obj(entry));
        const char *text = string ? qstring_get_str(string) : NULL;
        const char *p;
        if (!text || !*text || strlen(text) > 64 || text[0] < 'a' || text[0] > 'z' || g_hash_table_contains(features, text)) {
            return false;
        }
        for (p = text; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) {
                return false;
            }
        }
        g_hash_table_add(features, (void *)text);
    }
    return true;
}

static void put_decimal(QDict *dict, const char *name, uint64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%" PRIu64, value);
    qdict_put_str(dict, name, text);
}

static QDict *new_envelope(HostbusProbe *s, const char *kind, const char *status, int64_t time_ns)
{
    QDict *dict = qdict_new();
    QDict *version = qdict_new();
    if (s->tx_sequence == UINT64_MAX) {
        qobject_unref(dict); qobject_unref(version);
        set_error(s, "Output sequence exhausted; reconnect required");
        return NULL;
    }
    qdict_put_int(version, "major", 1);
    qdict_put_int(version, "minor", 0);
    qdict_put(dict, "version", version);
    qdict_put_str(dict, "kind", kind);
    qdict_put_str(dict, "status", status);
    qdict_put_str(dict, "session_id", s->session);
    qdict_put_str(dict, "connection_id", s->connection);
    put_decimal(dict, "reset_epoch", s->epoch);
    put_decimal(dict, "topology_generation", s->topology);
    put_decimal(dict, "sequence", ++s->tx_sequence);
    qdict_put_str(dict, "request_id", "");
    put_decimal(dict, "virtual_time_ns", time_ns);
    return dict;
}

static void parser_clear(HostbusProbe *s)
{
    s->prefix_bytes = s->expected = 0;
    g_byte_array_set_size(s->rx, 0);
}

static void set_error(HostbusProbe *s, const char *reason)
{
    CPUState *cpu;
    g_free(s->last_error);
    s->last_error = g_strdup(reason);
    s->pending = s->resolved = false;
    if (s->engaging) {
        s->engaging = false;
    }
    timer_del(s->host_timer);
    /* An established barrier remains owned; never infer permission to resume. */
}

static gboolean flush_tx(void *unused, GIOCondition condition, void *opaque)
{
    HostbusProbe *s = opaque;
    int written;
    if (condition & G_IO_HUP) {
        s->tx_watch = 0;
        return G_SOURCE_REMOVE;
    }
    while (s->tx->len && s->connected) {
        written = qemu_chr_fe_write(&s->chr, s->tx->data, s->tx->len);
        if (!s->connected) {
            s->tx_watch = 0;
            return G_SOURCE_REMOVE;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return G_SOURCE_CONTINUE;
            }
            set_error(s, "Host chardev write failed");
            s->tx_watch = 0;
            return G_SOURCE_REMOVE;
        }
        if (!written) {
            return G_SOURCE_CONTINUE;
        }
        g_byte_array_remove_range(s->tx, 0, written);
    }
    s->tx_watch = 0;
    return G_SOURCE_REMOVE;
}

static bool send_dict(HostbusProbe *s, QDict *dict)
{
    GString *json = qobject_to_json(QOBJECT(dict));
    uint8_t prefix[4];
    unsigned i;
    if (!s->connected || json->len > s->frame_limit || json->len + 4 + s->tx->len > TX_MAX) {
        g_string_free(json, true);
        set_error(s, "Host chardev output/frame/sequence limit exceeded");
        return false;
    }
    for (i = 0; i < 4; ++i) {
        prefix[i] = json->len >> (24 - i * 8);
    }
    g_byte_array_append(s->tx, prefix, 4);
    g_byte_array_append(s->tx, (const uint8_t *)json->str, json->len);
    g_string_free(json, true);
    if (!s->tx_watch) {
        s->tx_watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP, flush_tx, s);
        if (!s->tx_watch) {
            set_error(s, "Unable to watch nonblocking host output");
            return false;
        }
    }
    return true;
}

static bool hello(HostbusProbe *s, QDict *record, QDict *data,
                  uint64_t epoch, uint64_t topology, uint64_t sequence)
{
    static const char *const data_keys[] = {"capabilities", "required_capabilities", "limits"};
    static const char *const limit_keys[] = {"frame_bytes", "bus_payload_bytes", "in_flight"};
    const char *session = str_field(record, "session_id");
    const char *connection = str_field(record, "connection_id");
    QDict *limits = qobject_to(QDict, qdict_get(data, "limits"));
    g_autoptr(GHashTable) offered = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) required = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter iterator;
    void *key;
    uint32_t frame, payload, in_flight;
    QDict *ack, *ack_data, *ack_limits;
    QList *selected, *selected_required;
    bool fresh_session = session && s->session && strcmp(session, s->session);
    if (s->negotiated || sequence != 1 || !uuid(session) || !uuid(connection)
        || g_hash_table_contains(s->connections_seen, connection)
        || g_hash_table_size(s->connections_seen) >= 256
        || (!fresh_session && s->require_new_session)
        || (!fresh_session && (epoch < MAX(s->minimum_epoch, s->epoch) ||
                               topology < MAX(s->minimum_topology, s->topology)))
        || !equals(record, "request_id", "") || !equals(record, "status", "ok")
        || !exact_keys(data, data_keys, ARRAY_SIZE(data_keys)) || !exact_keys(limits, limit_keys, ARRAY_SIZE(limit_keys))
        || !feature_list(qobject_to(QList, qdict_get(data, "capabilities")), offered)
        || !feature_list(qobject_to(QList, qdict_get(data, "required_capabilities")), required)
        /* 1024 bytes covers the fixed probe envelope with max decimal fields. */
        || !number_field(limits, "frame_bytes", 1024, FRAME_MAX, &frame)
        || !number_field(limits, "bus_payload_bytes", 1, PAYLOAD_MAX, &payload)
        || !number_field(limits, "in_flight", 1, 64, &in_flight)
        || !g_hash_table_contains(offered, "bus.gpio.v1")
        || !g_hash_table_contains(offered, "control.reset-generation") || !g_hash_table_contains(offered, "payload.base64")) {
        set_error(s, "Unsupported or malformed host hello");
        return false;
    }
    g_hash_table_iter_init(&iterator, required);
    while (g_hash_table_iter_next(&iterator, &key, NULL)) {
        if (strcmp(key, "bus.gpio.v1") && strcmp(key, "control.reset-generation") && strcmp(key, "payload.base64")) {
            set_error(s, "Host requires an unsupported probe capability");
            return false;
        }
    }
    g_free(s->session); g_free(s->connection);
    s->session = g_strdup(session); s->connection = g_strdup(connection);
    g_hash_table_add(s->connections_seen, g_strdup(connection));
    s->epoch = epoch; s->topology = topology; s->rx_sequence = sequence;
    s->consecutive_discarded = 0;
    s->minimum_epoch = epoch; s->minimum_topology = topology;
    s->require_new_session = false;
    s->tx_sequence = s->ordinal = 0;
    s->frame_limit = frame; s->payload_limit = payload;
    ack = new_envelope(s, "hello_ack", "ok", 0);
    if (!ack) { return false; }
    ack_data = qdict_new(); ack_limits = qdict_new();
    selected = qlist_new(); selected_required = qlist_new();
    qlist_append_str(selected, "bus.gpio.v1");
    qlist_append_str(selected, "control.reset-generation");
    qlist_append_str(selected, "payload.base64");
    qlist_append_str(selected_required, "control.reset-generation");
    qlist_append_str(selected_required, "payload.base64");
    qdict_put(ack_data, "capabilities", selected);
    qdict_put(ack_data, "required_capabilities", selected_required);
    qdict_put_int(ack_limits, "frame_bytes", frame);
    qdict_put_int(ack_limits, "bus_payload_bytes", payload);
    qdict_put_int(ack_limits, "in_flight", 1);
    qdict_put(ack_data, "limits", ack_limits); qdict_put(ack, "data", ack_data);
    s->negotiated = send_dict(s, ack);
    qobject_unref(ack);
    if (s->negotiated && !s->owns_barrier && !s->armed) {
        g_clear_pointer(&s->last_error, g_free);
    }
    if (s->negotiated) { timer_del(s->host_timer); }
    return s->negotiated;
}

static void response(HostbusProbe *s, QDict *record, QDict *data, uint64_t virtual_ns)
{
    static const char *const keys[] = {"bus", "controller_id", "endpoint_ids", "net_ids", "phase_statuses",
        "modeled_latency_ns", "accepted_length", "payload_encoding", "payload", "error_message"};
    uint64_t ordinal, latency;
    uint32_t accepted;
    const char *encoded = str_field(data, "payload");
    const char *status = str_field(record, "status");
    gsize length = 0;
    g_autofree guchar *decoded = NULL;
    g_autofree char *canonical = NULL;
    QList *phases;
    const QListEntry *entry;
    if (!s->pending || !decimal_field(record, "request_id", &ordinal) || ordinal != s->ordinal) {
        discard(s);
        return;
    }
    if (!exact_keys(data, keys, ARRAY_SIZE(keys)) || !equals(data, "bus", "gpio")
        || !equals(data, "controller_id", "hostbus-probe") || !list_one(data, "endpoint_ids", "probe.peer")
        || !list_one(data, "net_ids", "probe.net") || !equals(data, "payload_encoding", "base64")
        || !encoded || strlen(encoded) > 4 || !utf16_bounded(str_field(data, "error_message"), 1024)
        || !decimal_field(data, "modeled_latency_ns", &latency) || !number_field(data, "accepted_length", 0, 0, &accepted)
        || virtual_ns > INT64_MAX || virtual_ns < s->stopped_ns || latency != virtual_ns - s->stopped_ns
        || !status || (!strcmp(status, "timeout") ? virtual_ns < s->stopped_ns + PROBE_DEADLINE_NS : virtual_ns > s->stopped_ns + PROBE_DEADLINE_NS)) {
        set_error(s, "Invalid probe response context/length/virtual completion");
        return;
    }
    if (strcmp(status, "ok") && strcmp(status, "nack") && strcmp(status, "error") && strcmp(status, "timeout") && strcmp(status, "unavailable")) {
        set_error(s, "Invalid probe response status"); return;
    }
    phases = qobject_to(QList, qdict_get(data, "phase_statuses"));
    if (!phases || qlist_size(phases) != 1) { set_error(s, "Invalid probe phase result count"); return; }
    QLIST_FOREACH_ENTRY(phases, entry) {
        QDict *phase = qobject_to(QDict, qlist_entry_obj(entry));
        uint32_t index;
        static const char *const phase_keys[] = {"index", "status"};
        if (!exact_keys(phase, phase_keys, ARRAY_SIZE(phase_keys)) || !number_field(phase, "index", 0, 0, &index)
            || !(equals(phase, "status", "ack") || equals(phase, "status", "nack") || equals(phase, "status", "error") || equals(phase, "status", "timeout"))) {
            set_error(s, "Invalid probe phase result"); return;
        }
    }
    decoded = g_base64_decode(encoded, &length);
    canonical = g_base64_encode(decoded, length);
    if (length > 1 || strcmp(canonical, encoded)) { set_error(s, "Noncanonical/oversized probe RX payload"); return; }
    s->pending = false; s->resolved = true; s->completion_ns = virtual_ns;
    s->consecutive_discarded = 0;
    g_free(s->last_payload); s->last_payload = g_strdup(encoded);
    g_free(s->last_status); s->last_status = g_strdup(status);
    timer_del(s->host_timer);
    /* Explicit release + external cont are required. Never vm_start here. */
}

static void handle_record(HostbusProbe *s)
{
    static const char *const keys[] = {"version", "kind", "status", "session_id", "connection_id",
        "reset_epoch", "topology_generation", "sequence", "request_id", "virtual_time_ns", "data"};
    static const char *const version_keys[] = {"major", "minor"};
    Error *error = NULL;
    QObject *object;
    QDict *record, *version, *data;
    uint64_t epoch, topology, sequence, virtual_ns;
    uint32_t major, minor;
    if (memchr(s->rx->data, 0, s->rx->len) ||
        !g_utf8_validate((const char *)s->rx->data, s->rx->len, NULL) ||
        !hostbus_json_prescan(s->rx->data, s->rx->len)) {
        set_error(s, "Host record requires strict NUL-free UTF-8 JSON with depth <=64"); return;
    }
    g_byte_array_append(s->rx, (const uint8_t *)"", 1);
    object = qobject_from_json((const char *)s->rx->data, &error);
    if (!object) {
        set_error(s, error ? error_get_pretty(error) : "Invalid host JSON");
        error_free(error);
        return;
    }
    record = qobject_to(QDict, object);
    version = record ? qobject_to(QDict, qdict_get(record, "version")) : NULL;
    data = record ? qobject_to(QDict, qdict_get(record, "data")) : NULL;
    if (!exact_keys(record, keys, ARRAY_SIZE(keys)) || !exact_keys(version, version_keys, ARRAY_SIZE(version_keys)) || !data
        || !number_field(version, "major", 1, 1, &major) || !number_field(version, "minor", 0, 0, &minor)
        || !decimal_field(record, "reset_epoch", &epoch) || !decimal_field(record, "topology_generation", &topology)
        || !decimal_field(record, "sequence", &sequence) || !sequence || !decimal_field(record, "virtual_time_ns", &virtual_ns)
        || !uuid(str_field(record, "session_id")) || !uuid(str_field(record, "connection_id"))) {
        set_error(s, "Malformed host envelope"); goto out;
    }
    if (equals(record, "kind", "hello")) { hello(s, record, data, epoch, topology, sequence); goto out; }
    if (!s->negotiated || !equals(record, "session_id", s->session) || !equals(record, "connection_id", s->connection)
        || sequence <= s->rx_sequence || epoch < s->epoch || topology < s->topology) {
        discard(s); goto out;
    }
    s->rx_sequence = sequence;
    if (equals(record, "kind", "reset")) {
        static const char *const reset_keys[] = {"reason"};
        if (!equals(record, "status", "ok") || !equals(record, "request_id", "")
            || !exact_keys(data, reset_keys, ARRAY_SIZE(reset_keys)) || !str_field(data, "reason")
            || strlen(str_field(data, "reason")) > 2048 || (epoch == s->epoch && topology == s->topology)) {
            set_error(s, "Malformed host reset"); goto out;
        }
        timer_del(s->dependency_timer); timer_del(s->completion_timer); timer_del(s->host_timer);
        s->armed = s->pending = s->resolved = s->completing = s->owns_barrier = false;
        s->epoch = epoch; s->topology = topology;
        s->minimum_epoch = epoch; s->minimum_topology = topology;
        s->consecutive_discarded = 0;
        g_clear_pointer(&s->last_error, g_free);
    } else if (epoch != s->epoch || topology != s->topology) {
        set_error(s, "Future host generation without reset");
    } else if (equals(record, "kind", "response")) {
        response(s, record, data, virtual_ns);
    } else {
        set_error(s, "Unsupported host record kind");
    }
out:
    qobject_unref(object);
}

static int can_read(void *opaque)
{
    return 64 * 1024;
}

static void read_bytes(void *opaque, const uint8_t *bytes, int size)
{
    HostbusProbe *s = opaque;
    while (size > 0) {
        if (!s->expected && !s->prefix_bytes) {
            /* Progress does not extend an incomplete frame's host deadline. */
            timer_mod(s->assembly_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + s->watchdog_ms);
        }
        if (!s->expected) {
            unsigned take = MIN(size, 4 - s->prefix_bytes);
            memcpy(s->prefix + s->prefix_bytes, bytes, take);
            s->prefix_bytes += take; bytes += take; size -= take;
            if (s->prefix_bytes != 4) { continue; }
            s->expected = ldl_be_p(s->prefix);
            s->prefix_bytes = 0;
            if (!s->expected || s->expected > s->frame_limit) {
                set_error(s, "Oversized/empty host length prefix"); parser_clear(s); qemu_chr_fe_disconnect(&s->chr); return;
            }
        }
        unsigned take = MIN(size, s->expected - s->rx->len);
        g_byte_array_append(s->rx, bytes, take); bytes += take; size -= take;
        if (s->rx->len == s->expected) {
            timer_del(s->assembly_timer);
            handle_record(s); parser_clear(s);
            if (!s->connected) { return; }
        }
    }
}

static void char_event(void *opaque, QEMUChrEvent event)
{
    HostbusProbe *s = opaque;
    if (event == CHR_EVENT_OPENED) {
        s->connected = true; s->negotiated = false;
        s->rx_sequence = s->tx_sequence = 0;
        s->consecutive_discarded = 0;
        s->frame_limit = FRAME_MAX; s->payload_limit = PAYLOAD_MAX;
        parser_clear(s);
        timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + s->watchdog_ms);
    } else if (event == CHR_EVENT_CLOSED) {
        s->connected = s->negotiated = false;
        timer_del(s->assembly_timer); timer_del(s->host_timer);
        if (s->tx_watch) { g_source_remove(s->tx_watch); s->tx_watch = 0; }
        g_byte_array_set_size(s->tx, 0); parser_clear(s);
        if ((s->owns_barrier || s->armed) && !s->last_error) {
            set_error(s, "Host disconnected; explicit cancel/reset required");
        }
    }
}

static void host_timeout(void *opaque)
{
    HostbusProbe *s = opaque;
    set_error(s, "Host watchdog expired; no guest hardware timeout was generated");
}

static void assembly_timeout(void *opaque)
{
    HostbusProbe *s = opaque;
    set_error(s, "Incomplete host frame watchdog expired");
    parser_clear(s);
    qemu_chr_fe_disconnect(&s->chr);
}

static void completion(void *opaque)
{
    HostbusProbe *s = opaque;
    assert(!qemu_in_vcpu_thread());
    assert(bql_locked());
    s->completing = false; s->resolved = false;
    s->delivered_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void warp_bh(void *opaque)
{
    HostbusProbe *s = opaque;
    assert(!qemu_in_vcpu_thread());
    assert(bql_locked());
    if (s->armed && runstate_is_running() && icount_enabled()) {
        /*
         * With an idle WFI guest no instruction advances the virtual clock,
         * and timerlist_rearm() could not start the icount warp while the
         * machine was paused when the dependency was armed. The clock is
         * enabled again by now; the warp advances it exactly to the earliest
         * virtual deadline, which the sweeper below pins to armed_ns.
         */
        icount_start_warp_timer();
    }
}

static void probe_vm_state_change(void *opaque, bool running, RunState state)
{
    HostbusProbe *s = opaque;
    /*
     * The vm-state notifier runs from vm_prepare_start() while the virtual
     * clock is still disabled, so the warp would compute no deadline here.
     * Defer it to a bottom half, which runs after vm_start() re-enabled the
     * clock and before or during the first serialized vCPU window.
     */
    if (running && state == RUN_STATE_RUNNING && s->armed && icount_enabled()) {
        qemu_bh_schedule(s->warp_bh);
    }
}

static void dependency_fire(void *opaque)
{
    HostbusProbe *s = opaque;
    CPUState *cpu;
    assert(bql_locked());
    /*
     * Runs from timerlist_run_timers() on the main-loop virtual timerlist,
     * in whichever thread reached the deadline first. The serialized vCPU
     * budget stops the guest clock exactly at armed_ns, and this claim sets
     * the stop flags before any later budget window can prepare, so no
     * guest instruction runs past armed_ns. cpu_pause() neither waits nor
     * touches the virtual clock, so it is safe from inside a timerlist run
     * (unlike pause_all_vcpus()/vm_stop(), which would self-deadlock on the
     * timerlist done event). The barrier bottom half finishes the central
     * stop outside the run; vm_stop() then finds the vCPUs already paused.
     */
    s->armed = false; s->owns_barrier = true; s->engaging = true;
    CPU_FOREACH(cpu) {
        cpu_pause(cpu);
    }
    qemu_bh_schedule(s->dependency_bh);
}

static void dependency_bh(void *opaque)
{
    HostbusProbe *s = opaque;
    QDict *request, *data, *phase;
    QList *endpoints, *nets, *phases;
    CPUState *cpu;
    assert(!qemu_in_vcpu_thread());
    assert(bql_locked());
    if (!s->engaging) {
        /* Spurious notify, cancelled, or already finished. */
        return;
    }
    /*
     * The fire claim set the stop flags before any later budget window
     * could prepare, so no guest instruction ran past armed_ns. This
     * bottom half is not inside any timerlist run, so vm_stop()'s
     * clock-disable wait cannot self-deadlock the way it would from inside
     * a timer callback, and pause_all_vcpus() finds the vCPUs already
     * stopped.
     */
    if (vm_stop(RUN_STATE_PAUSED) < 0) { set_error(s, "Unable to stop VM at host dependency"); return; }
    s->engaging = false;
    s->stopped_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->stopped_ns != s->armed_ns) { set_error(s, "Virtual stop missed its deadline; this icount profile is not qualified"); return; }
    if (!s->negotiated || !s->connected || s->last_error) { set_error(s, "Host unavailable at scheduled dependency"); return; }
    if (s->ordinal == UINT64_MAX || s->stopped_ns > INT64_MAX - PROBE_DEADLINE_NS) { set_error(s, "Probe counter/deadline exhausted"); return; }
    ++s->ordinal;
    request = new_envelope(s, "request", "pending", s->stopped_ns);
    if (!request) { return; }
    put_decimal(request, "request_id", s->ordinal);
    data = qdict_new(); endpoints = qlist_new(); nets = qlist_new(); phases = qlist_new(); phase = qdict_new();
    qdict_put_str(data, "bus", "gpio"); qdict_put_str(data, "controller_id", "hostbus-probe");
    qlist_append_str(endpoints, "probe.peer"); qlist_append_str(nets, "probe.net");
    qdict_put(data, "endpoint_ids", endpoints); qdict_put(data, "net_ids", nets);
    qdict_put_str(phase, "kind", "sample"); qdict_put_int(phase, "length", 1); qlist_append(phases, phase);
    qdict_put(data, "phases", phases); qdict_put_str(data, "bit_length", "8"); qdict_put_str(data, "bit_order", "msb-first");
    qdict_put_null(data, "mode"); qdict_put_null(data, "chip_select"); qdict_put_int(data, "read_length", 1);
    put_decimal(data, "virtual_deadline_ns", s->stopped_ns + PROBE_DEADLINE_NS);
    qdict_put_str(data, "payload_encoding", "base64"); qdict_put_str(data, "payload", ""); qdict_put(request, "data", data);
    s->pending = send_dict(s, request); qobject_unref(request);
    if (s->pending) { timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + s->watchdog_ms); }
}

static char *get_chardev(Object *obj, Error **errp) { return g_strdup(HOSTBUS_PROBE(obj)->chr_name); }
static void set_chardev(Object *obj, const char *value, Error **errp)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    if (s->completed) { error_setg(errp, "chardev cannot change after creation"); return; }
    g_free(s->chr_name); s->chr_name = g_strdup(value);
}
static char *get_watchdog(Object *obj, Error **errp) { return g_strdup_printf("%u", HOSTBUS_PROBE(obj)->watchdog_ms); }
static void set_watchdog(Object *obj, const char *value, Error **errp)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj); uint64_t number;
    if (!decimal(value, &number) || !number || number > 600000 || s->armed || s->owns_barrier) {
        error_setg(errp, "watchdog-ms must be canonical 1..600000 and changed only while idle"); return;
    }
    s->watchdog_ms = number;
}
static char *get_phase(Object *obj, Error **errp)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    return g_strdup(s->armed ? "armed" : s->engaging ? "blocked" : s->last_error ? (s->owns_barrier ? "blocked-error" : "error")
        : s->pending ? "blocked" : s->completing ? "completion-scheduled" : s->resolved ? "resolved"
        : s->delivered_ns >= 0 ? "completed" : s->negotiated ? "ready" : s->connected ? "handshaking" : "disconnected");
}
static bool get_blocked(Object *obj, Error **errp) { return HOSTBUS_PROBE(obj)->owns_barrier; }
static char *get_virtual_clock(Object *obj, Error **errp) { return g_strdup_printf("%" PRId64, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)); }
static char *get_armed(Object *obj, Error **errp) { return g_strdup_printf("%" PRId64, HOSTBUS_PROBE(obj)->armed_ns); }
static char *get_stopped(Object *obj, Error **errp) { return g_strdup_printf("%" PRId64, HOSTBUS_PROBE(obj)->stopped_ns); }
static char *get_delivered(Object *obj, Error **errp) { return g_strdup_printf("%" PRId64, HOSTBUS_PROBE(obj)->delivered_ns); }
static char *get_error(Object *obj, Error **errp) { return g_strdup(HOSTBUS_PROBE(obj)->last_error ?: ""); }
static char *get_payload(Object *obj, Error **errp) { return g_strdup(HOSTBUS_PROBE(obj)->last_payload ?: ""); }
static char *get_late(Object *obj, Error **errp) { return g_strdup_printf("%" PRIu64, HOSTBUS_PROBE(obj)->late_replies); }
static char *get_cpu_step_flags(Object *obj, Error **errp)
{
    CPUState *cpu;
    GString *text = g_string_new(NULL);
    CPU_FOREACH(cpu) {
        g_string_append_printf(text, "%s%d:%d", text->len ? "," : "",
                               cpu->cpu_index, cpu->singlestep_enabled);
    }
    return g_string_free(text, false);
}

static void cancel(HostbusProbe *s)
{
    CPUState *cpu;
    timer_del(s->dependency_timer); timer_del(s->completion_timer); timer_del(s->host_timer);
    timer_del(s->assembly_timer);
    if (s->engaging) {
        s->engaging = false;
    }
    s->armed = s->pending = s->resolved = s->completing = s->owns_barrier = false;
    s->consecutive_discarded = 0;
    g_clear_pointer(&s->last_error, g_free);
    qemu_chr_fe_disconnect(&s->chr);
    /* The VM is still paused. An external explicit cont is needed. */
}
static void set_control(Object *obj, const char *value, Error **errp)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    uint64_t delay;
    int64_t now;
    if (!s->completed) { error_setg(errp, "control is available only after object creation"); return; }
    if (!strcmp(value, "cancel")) { cancel(s); return; }
    if (!strcmp(value, "release")) {
        if (!s->owns_barrier || !s->resolved || s->pending || s->last_error || !runstate_check(RUN_STATE_PAUSED)) {
            error_setg(errp, "release requires a resolved paused barrier without host error"); return;
        }
        s->owns_barrier = false; s->completing = true;
        timer_mod_ns(s->completion_timer, s->completion_ns);
        return;
    }
    if (!g_str_has_prefix(value, "arm:") || !decimal(value + 4, &delay) || !delay) {
        error_setg(errp, "control accepts arm:<positive decimal ns>, release, or cancel"); return;
    }
    if (!icount_enabled() || qemu_tcg_mttcg_enabled() || !s->negotiated || !s->connected
        || s->armed || s->owns_barrier || s->pending || s->completing || s->last_error) {
        error_setg(errp, "arm requires idle negotiated peer and serialized TCG/icount"); return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (delay > INT64_MAX - now) { error_setg(errp, "arm exceeds signed QEMU timer domain"); return; }
    s->armed_ns = now + delay; s->delivered_ns = -1; s->armed = true;
    timer_mod_ns(s->dependency_timer, s->armed_ns);
}

static void reset_hold(Object *obj, ResetType type)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    bool active = s->negotiated || s->armed || s->owns_barrier || s->pending ||
                  s->resolved || s->completing ||
                  timer_pending(s->dependency_timer) ||
                  timer_pending(s->completion_timer);
    if (!active && !s->session) {
        /* The initial board reset precedes the first async hello. Do not
         * disconnect that startup socket or invent an advanced wire epoch. */
        return;
    }
    if (s->epoch != UINT64_MAX) { s->minimum_epoch = s->epoch + 1; }
    s->minimum_topology = MAX(s->minimum_topology, s->topology);
    cancel(s);
    s->armed_ns = s->stopped_ns = s->completion_ns = s->delivered_ns = -1;
    g_clear_pointer(&s->last_payload, g_free);
    g_clear_pointer(&s->last_status, g_free);
    if (s->epoch == UINT64_MAX) {
        s->require_new_session = true;
        set_error(s, "Reset epoch exhausted; a new host session is required");
    }
}
static void complete(UserCreatable *uc, Error **errp)
{
    HostbusProbe *s = HOSTBUS_PROBE(uc);
    Chardev *chr = s->chr_name ? qemu_chr_find(s->chr_name) : NULL;
    if (!chr || !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_SOCKET)) {
        error_setg(errp, "A dedicated socket chardev is required; UART/stdio/mux is not supported");
        return;
    }
    if (!qemu_chr_fe_init(&s->chr, chr, errp)) { return; }
    s->dependency_timer = aio_timer_new(qemu_get_aio_context(), QEMU_CLOCK_VIRTUAL, SCALE_NS, dependency_fire, s);
    s->dependency_bh = qemu_bh_new(dependency_bh, s);
    s->warp_bh = qemu_bh_new(warp_bh, s);
    s->completion_timer = aio_timer_new(qemu_get_aio_context(), QEMU_CLOCK_VIRTUAL, SCALE_NS, completion, s);
    s->host_timer = aio_timer_new(qemu_get_aio_context(), QEMU_CLOCK_REALTIME, SCALE_MS, host_timeout, s);
    s->assembly_timer = aio_timer_new(qemu_get_aio_context(), QEMU_CLOCK_REALTIME, SCALE_MS, assembly_timeout, s);
    error_setg(&s->migration_blocker, "Hostbus probe external-peer/barrier state is not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) { return; }
    qemu_register_resettable(OBJECT(s)); s->reset_registered = true;
    s->vm_state = qemu_add_vm_change_state_handler(probe_vm_state_change, s);
    probes = g_slist_prepend(probes, s); s->completed = true;
    qemu_chr_fe_set_handlers(&s->chr, can_read, read_bytes, char_event, NULL, s, NULL, true);
}
static bool can_delete(UserCreatable *uc)
{
    HostbusProbe *s = HOSTBUS_PROBE(uc);
    return !s->armed && !s->owns_barrier && !s->pending && !s->completing;
}
static void init(Object *obj)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    s->rx = g_byte_array_new(); s->tx = g_byte_array_new();
    s->connections_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->frame_limit = FRAME_MAX; s->payload_limit = PAYLOAD_MAX; s->watchdog_ms = 5000;
    s->armed_ns = s->stopped_ns = s->completion_ns = s->delivered_ns = -1;
}
static void finalize(Object *obj)
{
    HostbusProbe *s = HOSTBUS_PROBE(obj);
    if (s->reset_registered) { qemu_unregister_resettable(obj); }
    if (s->vm_state) { qemu_del_vm_change_state_handler(s->vm_state); }
    probes = g_slist_remove(probes, s);
    if (s->tx_watch) { g_source_remove(s->tx_watch); }
    if (s->dependency_bh) { qemu_bh_delete(s->dependency_bh); }
    if (s->warp_bh) { qemu_bh_delete(s->warp_bh); }
    if (s->dependency_timer) { timer_free(s->dependency_timer); }
    if (s->completion_timer) { timer_free(s->completion_timer); }
    if (s->host_timer) { timer_free(s->host_timer); }
    if (s->assembly_timer) { timer_free(s->assembly_timer); }
    if (s->migration_blocker) { migrate_del_blocker(&s->migration_blocker); }
    qemu_chr_fe_deinit(&s->chr, false);
    g_byte_array_unref(s->rx); g_byte_array_unref(s->tx);
    g_hash_table_unref(s->connections_seen);
    g_free(s->chr_name); g_free(s->session); g_free(s->connection);
    g_free(s->last_error); g_free(s->last_payload); g_free(s->last_status);
}
static ResettableState *get_reset_state(Object *obj) { return &HOSTBUS_PROBE(obj)->reset_state; }
static void class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    ucc->complete = complete; ucc->can_be_deleted = can_delete;
    rc->get_state = get_reset_state; rc->phases.hold = reset_hold;
    object_class_property_add_str(oc, "chardev", get_chardev, set_chardev);
    object_class_property_add_str(oc, "watchdog-ms", get_watchdog, set_watchdog);
    object_class_property_add_str(oc, "control", NULL, set_control);
    object_class_property_add_str(oc, "phase", get_phase, NULL);
    object_class_property_add_bool(oc, "resume-blocked", get_blocked, NULL);
    object_class_property_add_str(oc, "virtual-ns", get_virtual_clock, NULL);
    object_class_property_add_str(oc, "armed-ns", get_armed, NULL);
    object_class_property_add_str(oc, "stopped-ns", get_stopped, NULL);
    object_class_property_add_str(oc, "delivered-ns", get_delivered, NULL);
    object_class_property_add_str(oc, "last-error", get_error, NULL);
    object_class_property_add_str(oc, "last-payload", get_payload, NULL);
    object_class_property_add_str(oc, "late-replies", get_late, NULL);
    object_class_property_add_str(oc, "cpu-step-flags", get_cpu_step_flags, NULL);
}
static const TypeInfo info = {
    .name = TYPE_HOSTBUS_PROBE, .parent = TYPE_OBJECT, .instance_size = sizeof(HostbusProbe),
    .instance_init = init, .instance_finalize = finalize, .class_init = class_init,
    .interfaces = (InterfaceInfo[]) { { TYPE_USER_CREATABLE }, { TYPE_RESETTABLE_INTERFACE }, {} },
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types);
