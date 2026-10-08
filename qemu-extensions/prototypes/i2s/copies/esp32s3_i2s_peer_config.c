/* Strict registered i2s-sample-peer parameters, schema version 1.
 * Geometry follows the pinned ESP-IDF 6.1 S3 HW-v2 slot defaults; this
 * describes an external digital peer, not a silicon PCM/PDM converter.
 */
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_i2s_peer.h"
#include "qapi/error.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"

#define PEER_VECTOR_LIMIT 65536U

static bool integer(QObject *object, const char *field, uint64_t maximum,
                    uint64_t *value, Error **errp)
{
    QNum *number = qobject_to(QNum, object);
    if (!number || !qnum_get_try_uint(number, value) || *value > maximum) {
        error_setg(errp, "i2s-sample-peer '%s' requires an unsigned integer <= %" PRIu64,
                   field, maximum);
        return false;
    }
    return true;
}

static bool boolean(QDict *parameters, const char *field, bool *value,
                    Error **errp)
{
    QBool *object = qobject_to(QBool, qdict_get(parameters, field));
    if (!object) {
        error_setg(errp, "i2s-sample-peer '%s' requires a JSON boolean", field);
        return false;
    }
    *value = qbool_get_bool(object);
    return true;
}

static int enumeration(QDict *parameters, const char *field,
                       const char *const *values, size_t count, Error **errp)
{
    QString *object = qobject_to(QString, qdict_get(parameters, field));
    const char *text = object ? qstring_get_str(object) : NULL;
    for (size_t i = 0; text && i < count; ++i) {
        if (!strcmp(text, values[i])) {
            return i;
        }
    }
    error_setg(errp, "i2s-sample-peer '%s' requires a supported exact string", field);
    return -1;
}

void esp32s3_i2s_peer_config_clear(S3I2sPeerConfig *config)
{
    if (config) {
        g_free(config->tx_samples);
        g_free(config->raw_bits);
        memset(config, 0, sizeof(*config));
    }
}

static void hash_unsigned(GChecksum *checksum, uint64_t value, unsigned bytes)
{
    uint8_t encoded[8];
    for (unsigned i = 0; i < bytes; ++i) {
        encoded[i] = value >> (8 * i);
    }
    g_checksum_update(checksum, encoded, bytes);
}

static void config_identity(S3I2sPeerConfig *config)
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    uint8_t vector_digest[32];
    gsize digest_size = sizeof(vector_digest);
    if (config->raw_bits) {
        g_checksum_update(checksum, config->raw_bits, config->raw_bit_count);
    } else {
        for (size_t i = 0; i < config->tx_sample_count; ++i) {
            hash_unsigned(checksum, config->tx_samples[i], 4);
        }
    }
    g_checksum_get_digest(checksum, vector_digest, &digest_size);
    g_checksum_reset(checksum);
    static const char domain[] = "i2s-sample-peer-config-v1";
    g_checksum_update(checksum, (const uint8_t *)domain, sizeof(domain) - 1);
    const uint64_t values[] = {
        config->role, config->format, config->data_bits, config->slot_bits,
        config->slots, config->slot_mask, config->ws_width, config->ws_pol,
        config->bit_shift, config->left_align, config->lsb_first,
        config->rate_num_hz, config->rate_den, config->repeat,
        config->capture_capacity, config->tx_sample_count, config->raw_bit_count,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(values); ++i) {
        hash_unsigned(checksum, values[i], 8);
    }
    g_checksum_update(checksum, vector_digest, sizeof(vector_digest));
    g_strlcpy(config->identity, g_checksum_get_string(checksum),
              sizeof(config->identity));
    g_checksum_free(checksum);
}

/* The caller supplies an empty output config, clears it after use or moves
 * its owned vectors into the service. Failure leaves output untouched.
 * The JSON vectors are copied exactly once into their final owned buffers.
 */
bool esp32s3_i2s_peer_parse(const QDict *component, S3I2sPeerConfig *output,
                          Error **errp)
{
    QDict *attributes = component ? qobject_to(QDict, qdict_get(component, "attributes")) : NULL;
    QDict *parameters = attributes ? qobject_to(QDict, qdict_get(attributes, "native_i2s_peer")) : NULL;
    static const char *const fields[] = {
        "role", "format", "dataBits", "slotBits", "slots", "slotMask",
        "wsWidth", "wsPol", "bitShift", "leftAlign", "lsbFirst",
        "sampleRateNumeratorHz", "sampleRateDenominator", "repeat",
        "captureCapacity", "txSamples", "rawBits",
    };
    static const char *const roles[] = {"master", "slave"};
    static const char *const formats[] = {"philips", "msb", "pcm", "tdm", "raw-pdm"};
    S3I2sPeerConfig config = {0};
    uint64_t data_bits, slot_bits, slots, mask, ws_width, denominator, capacity;
    int role, format;
    QList *vector;
    const QListEntry *entry;
    size_t count, index = 0;
    uint64_t maximum;
    const char *vector_field;
    if (!parameters || !output) {
        error_setg(errp, "i2s-sample-peer requires attributes.native_i2s_peer object and output config");
        return false;
    }
    for (const QDictEntry *field = qdict_first(parameters); field;
         field = qdict_next(parameters, field)) {
        bool known = false;
        for (size_t i = 0; i < G_N_ELEMENTS(fields); ++i) {
            known |= !strcmp(qdict_entry_key(field), fields[i]);
        }
        if (!known) {
            error_setg(errp, "Unknown i2s-sample-peer parameter '%s'", qdict_entry_key(field));
            return false;
        }
    }
    role = enumeration(parameters, "role", roles, G_N_ELEMENTS(roles), errp);
    if (role < 0) {
        return false;
    }
    format = enumeration(parameters, "format", formats, G_N_ELEMENTS(formats), errp);
    if (format < 0) {
        return false;
    }
    config.role = role == 0 ? S3_I2S_PEER_MASTER : S3_I2S_PEER_SLAVE;
    switch (format) {
    case 0: config.format = S3_I2S_PEER_PHILIPS; break;
    case 1: config.format = S3_I2S_PEER_MSB; break;
    case 2: config.format = S3_I2S_PEER_PCM; break;
    case 3: config.format = S3_I2S_PEER_TDM; break;
    case 4: config.format = S3_I2S_PEER_RAW_PDM; break;
    default: g_assert_not_reached();
    }
    if (!integer(qdict_get(parameters, "dataBits"), "dataBits", 32, &data_bits, errp) ||
        !integer(qdict_get(parameters, "slotBits"), "slotBits", 32, &slot_bits, errp) ||
        !integer(qdict_get(parameters, "slots"), "slots", 16, &slots, errp) ||
        !integer(qdict_get(parameters, "slotMask"), "slotMask", UINT16_MAX, &mask, errp) ||
        !integer(qdict_get(parameters, "wsWidth"), "wsWidth", 128, &ws_width, errp) ||
        !integer(qdict_get(parameters, "sampleRateNumeratorHz"), "sampleRateNumeratorHz",
                 UINT64_MAX, &config.rate_num_hz, errp) ||
        !integer(qdict_get(parameters, "sampleRateDenominator"), "sampleRateDenominator",
                 UINT32_MAX, &denominator, errp) ||
        !integer(qdict_get(parameters, "captureCapacity"), "captureCapacity",
                 PEER_VECTOR_LIMIT, &capacity, errp) ||
        !boolean(parameters, "wsPol", &config.ws_pol, errp) ||
        !boolean(parameters, "bitShift", &config.bit_shift, errp) ||
        !boolean(parameters, "leftAlign", &config.left_align, errp) ||
        !boolean(parameters, "lsbFirst", &config.lsb_first, errp) ||
        !boolean(parameters, "repeat", &config.repeat, errp)) {
        return false;
    }
    vector_field = format == 4 ? "rawBits" : "txSamples";
    if (qdict_haskey(parameters, format == 4 ? "txSamples" : "rawBits")) {
        error_setg(errp, "i2s-sample-peer requires exactly one format-appropriate source vector");
        return false;
    }
    vector = qobject_to(QList, qdict_get(parameters, vector_field));
    count = vector ? qlist_size(vector) : 0;
    if (!count || count > PEER_VECTOR_LIMIT) {
        error_setg(errp, "i2s-sample-peer '%s' requires 1..65536 entries", vector_field);
        return false;
    }
    maximum = format == 4 ? 1 : data_bits == 32 ? UINT32_MAX : (UINT32_C(1) << data_bits) - 1;
    if (format == 4) {
        config.raw_bits = g_new(uint8_t, count);
        config.raw_bit_count = count;
    } else {
        config.tx_samples = g_new(uint32_t, count);
        config.tx_sample_count = count;
    }
    QLIST_FOREACH_ENTRY(vector, entry) {
        uint64_t value;
        if (!integer(qlist_entry_obj(entry), vector_field, maximum, &value, errp)) {
            esp32s3_i2s_peer_config_clear(&config);
            return false;
        }
        if (format == 4) {
            config.raw_bits[index++] = value;
        } else {
            config.tx_samples[index++] = value;
        }
    }
    config.data_bits = data_bits;
    config.slot_bits = slot_bits;
    config.slots = slots;
    config.slot_mask = mask;
    config.ws_width = ws_width;
    config.rate_den = denominator;
    config.capture_capacity = capacity;
    if (!esp32s3_i2s_peer_config_validate(&config, errp)) {
        esp32s3_i2s_peer_config_clear(&config);
        return false;
    }
    config_identity(&config);
    *output = config;
    return true;
}
