/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qjson.h"
#include "esp32s3_project.h"

static const char *text(QDict *d, const char *key, Error **errp)
{
    const char *s = d ? qdict_get_try_str(d, key) : NULL;
    if (!s || !*s || !g_utf8_validate(s, -1, NULL)) {
        error_setg(errp, "v3 field '%s' requires nonempty UTF-8 text", key);
        return NULL;
    }
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        if (!g_unichar_isspace(g_utf8_get_char(p))) {
            return s;
        }
    }
    error_setg(errp, "v3 field '%s' cannot be whitespace", key);
    return NULL;
}

static bool identity(EnAdptId *dst, const char *id, Error **errp)
{
    size_t len = id ? strlen(id) : 0;
    if (!len || len > EN_ADPT_ID_MAX || !g_ascii_isalnum(id[0])) {
        error_setg(errp, "Invalid v3 identifier '%s'", id ? id : "(absent)");
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!g_ascii_isalnum(id[i]) && !strchr("_.:/-", id[i])) {
            error_setg(errp, "Invalid ASCII v3 identifier '%s'", id);
            return false;
        }
    }
    memcpy(dst->text, id, len + 1);
    return true;
}

static int dict_id_cmp(const void *a, const void *b)
{
    return strcmp(qdict_get_str(*(QDict * const *)a, "id"),
                  qdict_get_str(*(QDict * const *)b, "id"));
}

static GPtrArray *objects(QList *list, size_t bound, Error **errp)
{
    GPtrArray *array = g_ptr_array_new();
    const QListEntry *entry;
    if (!list || qlist_size(list) > bound) {
        error_setg(errp, "Missing v3 array or count exceeds %zu", bound);
        g_ptr_array_unref(array);
        return NULL;
    }
    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(entry));
        if (!d || !qdict_get_try_str(d, "id")) {
            error_setg(errp, "v3 array entry requires an object and string id");
            g_ptr_array_unref(array);
            return NULL;
        }
        g_ptr_array_add(array, d);
    }
    qsort(array->pdata, array->len, sizeof(void *), dict_id_cmp);
    return array;
}

static int choice(const char *s, const char *const *names, size_t count)
{
    for (size_t i = 0; s && i < count; ++i) {
        if (!strcmp(s, names[i])) {
            return i;
        }
    }
    return -1;
}

static bool quantity(QDict *q, EnAdptQuantity *out, Error **errp)
{
    static const char *const units[] = {"V", "mV", "A", "mA", "ohm", "kohm", "Mohm",
                                       "ratio", "%", "state", "F", "uF", "nF", "pF", "count"};
    const char *unit = q ? qdict_get_try_str(q, "unit") : NULL;
    QObject *value = q ? qdict_get(q, "value") : NULL;
    int u = choice(unit, units, G_N_ELEMENTS(units));
    if (u == 9) {
        const char *state = qobject_to(QString, value) ? qstring_get_str(qobject_to(QString, value)) : NULL;
        if (!state || (strcmp(state, "open") && strcmp(state, "closed"))) {
            error_setg(errp, "Switch state requires open/closed state quantity");
            return false;
        }
        out->unit = !strcmp(state, "open") ? EN_ADPT_UNIT_STATE_OPEN : EN_ADPT_UNIT_STATE_CLOSED;
        out->value = 0;
        return true;
    }
    if (u < 0 || !qobject_to(QNum, value)) {
        error_setg(errp, "Unsupported or malformed electrical quantity unit '%s'", unit ? unit : "(absent)");
        return false;
    }
    double scale = 1;
    bool divide = false;
    EnAdptUnit kind;
    switch (u) {
    case 0: kind = EN_ADPT_UNIT_V; break;
    case 1: kind = EN_ADPT_UNIT_V; scale = 1000; divide = true; break;
    case 2: kind = EN_ADPT_UNIT_A; break;
    case 3: kind = EN_ADPT_UNIT_A; scale = 1000; divide = true; break;
    case 4: kind = EN_ADPT_UNIT_OHM; break;
    case 5: kind = EN_ADPT_UNIT_OHM; scale = 1e3; break;
    case 6: kind = EN_ADPT_UNIT_OHM; scale = 1e6; break;
    case 7: kind = EN_ADPT_UNIT_RATIO; break;
    case 8: kind = EN_ADPT_UNIT_RATIO; scale = 100; divide = true; break;
    case 10: kind = EN_ADPT_UNIT_F; break;
    case 11: kind = EN_ADPT_UNIT_F; scale = 1000000; divide = true; break;
    case 12: kind = EN_ADPT_UNIT_F; scale = 1000000000; divide = true; break;
    case 13: kind = EN_ADPT_UNIT_F; scale = 1000000000000; divide = true; break;
    default: kind = EN_ADPT_UNIT_COUNT; break;
    }
    out->unit = kind;
    out->value = qnum_get_double(qobject_to(QNum, value));
    if (scale != 1) {
        /* Divide by exact integer denominators, not rounded reciprocals:
         * equivalent SI quantities must preserve canonical double identity. */
        out->value = divide ? out->value / scale : out->value * scale;
    }
    if (out->value == 0) {
        out->value = 0; /* Canonicalize signed zero for electrical identity. */
    }
    if (!isfinite(out->value)) {
        error_setg(errp, "Electrical quantity must be finite in SI units");
        return false;
    }
    return true;
}

static int parameter_cmp(const void *a, const void *b)
{
    return strcmp(((const EnAdptParameter *)a)->name, ((const EnAdptParameter *)b)->name);
}
static int endpoint_cmp(const void *a, const void *b)
{
    return strcmp(((const EnAdptId *)a)->text, ((const EnAdptId *)b)->text);
}

static size_t flat_role(const Esp32S3Project *p, const EnAdptComponent *c,
                        const char *role)
{
    for (size_t i = 0; i < c->terminal_count; ++i) {
        if (!strcmp(c->terminals[i].role, role)) {
            return c->terminals + i - p->terminals;
        }
    }
    return SIZE_MAX;
}

static bool parse(Esp32S3Project *p, QDict *root, Error **errp)
{
    static const char *const kinds[] = {"mcu", "device", "resistor", "capacitor", "voltage-source",
                                       "current-source", "potentiometer", "switch", "ground"};
    static const char *const domains[] = {"digital", "analog", "power", "ground", "passive", "unspecified"};
    static const char *const directions[] = {"input", "output", "inout", "passive", "unspecified"};
    g_autoptr(GString) signature = g_string_new(NULL);
    EnAdptId project_id;
    QDict *profile = root ? qobject_to(QDict, qdict_get(root, "profile")) : NULL;
    QDict *runtime = root ? qobject_to(QDict, qdict_get(root, "runtime")) : NULL;
    QDict *electrical = runtime ? qobject_to(QDict, qdict_get(runtime, "electrical")) : NULL;
    QDict *geometry = root ? qobject_to(QDict, qdict_get(root, "geometry")) : NULL;
    const char *driver = electrical ? qdict_get_try_str(electrical, "driver_profile") : NULL;
    bool reserved[49] = {0};
    int64_t version;
    QNum *version_value = root ? qobject_to(QNum, qdict_get(root, "version")) : NULL;
    if (!version_value || !qnum_get_try_int(version_value, &version) || version != 3 ||
        !profile || g_strcmp0(qdict_get_try_str(profile, "chip"), "esp32s3") ||
        !qobject_to(QDict, qdict_get(root, "firmware")) || !runtime || !geometry ||
        !qobject_to(QDict, qdict_get(geometry, "components")) ||
        !qobject_to(QDict, qdict_get(geometry, "nets"))) {
        error_setg(errp, "Native electrical Apply requires a complete esp32s3 v3 project document");
        return false;
    }
    const char *id = text(root, "id", errp);
    if (!id || !identity(&project_id, id, errp) || !text(root, "name", errp) ||
        !text(profile, "board", errp) || !text(profile, "module", errp)) {
        return false;
    }
    if (qdict_haskey(profile, "reservations_known") &&
        !qobject_to(QBool, qdict_get(profile, "reservations_known"))) {
        error_setg(errp, "profile.reservations_known requires a boolean");
        return false;
    }
    g_autofree char *module = g_utf8_strdown(qdict_get_str(profile, "module"), -1);
    if (!strcmp(module, "esp32-s3-wroom-1-n8r8") ||
        !strcmp(module, "esp32-s3-wroom-1u-n8r8") ||
        !strcmp(module, "esp32-s3-wroom-2-n32r16v")) {
        reserved[35] = reserved[36] = reserved[37] = true;
    }
    if (qdict_haskey(profile, "reserved_gpios")) {
        QList *pins = qobject_to(QList, qdict_get(profile, "reserved_gpios"));
        if (!pins || qlist_size(pins) > EN_ADPT_MAX_TERMINALS) {
            error_setg(errp, "profile.reserved_gpios requires a bounded array");
            return false;
        }
        const QListEntry *entry;
        QLIST_FOREACH_ENTRY(pins, entry) {
            QObject *value = qlist_entry_obj(entry);
            QDict *record = qobject_to(QDict, value);
            QNum *number = qobject_to(QNum, record ? qdict_get(record, "gpio") : value);
            int64_t pin;
            if (!number || !qnum_get_try_int(number, &pin) || pin < 0 || pin > 48 ||
                (pin >= 22 && pin <= 25)) {
                error_setg(errp, "Profile reservation requires a physical S3 GPIO");
                return false;
            }
            reserved[pin] = true;
        }
    }
    g_string_append_printf(signature, "profile:%zu:%s,%d;",
                           strlen(module), module,
                           qdict_get_try_bool(profile, "reservations_known", false));
    for (size_t pin = 0; pin < 49; ++pin) {
        if (reserved[pin]) {
            g_string_append_printf(signature, "reserved:%zu;", pin);
        }
    }
    if (g_strcmp0(driver, "s3-explicit-finite-v1")) {
        error_setg(errp, "Declare runtime.electrical.driver_profile=s3-explicit-finite-v1; no inferred impedance");
        return false;
    }
    const char *mode = qdict_get_try_str(electrical, "mode");
    if (mode && strcmp(mode, "dc") && strcmp(mode, "rc")) {
        error_setg(errp, "Electrical mode must be dc or rc");
        return false;
    }
    p->rc = mode && !strcmp(mode, "rc");
    const char *policy = qdict_get_try_str(electrical, "edit_charge");
    if ((p->rc && !policy) || (policy && strcmp(policy, "keep") && strcmp(policy, "reset"))) {
        error_setg(errp, "RC Apply requires declared edit_charge keep/reset");
        return false;
    }
    p->charge_policy = policy && !strcmp(policy, "keep") ? EN_RC_KEEP_CHARGE : EN_RC_RESET_CHARGE;
    g_string_append_printf(signature, "mode:%d;", p->rc);
    g_autoptr(GPtrArray) components = objects(qobject_to(QList, qdict_get(root, "components")),
                                             EN_ADPT_MAX_COMPONENTS, errp);
    if (!components) {
        return false;
    }
    unsigned mcu_count = 0;
    for (size_t ci = 0; ci < components->len; ++ci) {
        QDict *c = components->pdata[ci];
        EnAdptComponent *component = &p->components[ci];
        p->component_documents[ci] = c;
        const char *cid = text(c, "id", errp);
        const char *kind = text(c, "kind", errp);
        if (!cid || !kind || !identity(&component->id, cid, errp) ||
            !text(c, "name", errp) || !text(c, "type", errp)) {
            return false;
        }
        int k = choice(kind, kinds, G_N_ELEMENTS(kinds));
        if (k < 0) {
            error_setg(errp, "Unsupported component kind '%s'", kind);
            return false;
        }
        component->kind = k;
        mcu_count += k == EN_ADPT_KIND_MCU;
        g_string_append_printf(signature, "c:%s,%d;", cid, k);
        QDict *parameters = qobject_to(QDict, qdict_get(c, "parameters"));
        if (!parameters || qdict_size(parameters) > 8) {
            error_setg(errp, "Component '%s' requires bounded parameter object", cid);
            return false;
        }
        size_t np = 0;
        for (const QDictEntry *entry = qdict_first(parameters); entry; entry = qdict_next(parameters, entry)) {
            EnAdptParameter *param = &p->parameters[ci][np++];
            param->name = qdict_entry_key(entry);
            if (!quantity(qobject_to(QDict, qdict_entry_value(entry)), &param->quantity, errp)) {
                return false;
            }
        }
        qsort(p->parameters[ci], np, sizeof(EnAdptParameter), parameter_cmp);
        component->parameters = p->parameters[ci];
        component->parameter_count = np;
        for (size_t i = 0; i < np; ++i) {
            EnAdptParameter *q = &p->parameters[ci][i];
            g_string_append_printf(signature, "q:%s,%d,%.17g;", q->name, q->quantity.unit, q->quantity.value);
        }
        g_autoptr(GPtrArray) terminals = objects(qobject_to(QList, qdict_get(c, "terminals")),
                                                EN_ADPT_MAX_TERMINALS - p->terminal_count, errp);
        if (!terminals) {
            return false;
        }
        component->terminals = &p->terminals[p->terminal_count];
        component->terminal_count = terminals->len;
        for (size_t ti = 0; ti < terminals->len; ++ti) {
            QDict *t = terminals->pdata[ti];
            size_t flat = p->terminal_count++;
            EnAdptTerminal *terminal = &p->terminals[flat];
            const char *tid = text(t, "id", errp);
            const char *role = text(t, "role", errp);
            int domain = choice(qdict_get_try_str(t, "domain"), domains, G_N_ELEMENTS(domains));
            int direction = choice(qdict_get_try_str(t, "direction"), directions, G_N_ELEMENTS(directions));
            if (!tid || !role || !identity(&terminal->id, tid, errp) || !text(t, "name", errp)) {
                return false;
            }
            if (domain < 0 || direction < 0) {
                error_setg(errp, "Terminal '%s' has invalid domain/direction", tid);
                return false;
            }
            terminal->role = role;
            terminal->domain = domain;
            terminal->direction = direction;
            int64_t gpio = -1;
            if (qdict_haskey(t, "gpio")) {
                QNum *num = qobject_to(QNum, qdict_get(t, "gpio"));
                if (k != EN_ADPT_KIND_MCU || !num || !qnum_get_try_int(num, &gpio) || gpio < 0 || gpio > 48 ||
                    (gpio >= 22 && gpio <= 25) || (domain != EN_ADPT_DOMAIN_DIGITAL && domain != EN_ADPT_DOMAIN_ANALOG) ||
                    p->pad_terminal[gpio] >= 0) {
                    error_setg(errp, "Terminal '%s' has invalid/duplicate S3 MCU gpio/domain", tid);
                    return false;
                }
                p->pad_terminal[gpio] = flat;
            } else if (k == EN_ADPT_KIND_MCU) {
                if (domain == EN_ADPT_DOMAIN_POWER && !strcmp(role, "vdd") && p->vdd_terminal < 0) {
                    p->vdd_terminal = flat;
                } else if (domain == EN_ADPT_DOMAIN_GROUND && !strcmp(role, "gnd") && p->gnd_terminal < 0) {
                    p->gnd_terminal = flat;
                } else {
                    error_setg(errp, "MCU terminal '%s' requires gpio signal or unique explicit vdd/gnd power role", tid);
                    return false;
                }
            }
            g_string_append_printf(signature, "t:%s,%zu:%s,%d,%d,%" PRId64 ";",
                                   tid, strlen(role), role, domain, direction, gpio);
        }
        if (k == EN_ADPT_KIND_DEVICE) {
            const char *type = qdict_get_str(c, "type");
            EnAdptNativeModel model;
            if (!en_adpt_model_from_type(type, &model) ||
                p->device_count == ESP32S3_PROJECT_MAX_MODELS) {
                error_setg(errp, "Device '%s' type '%s' has no supported native model or registry capacity", cid, type);
                return false;
            }
            const char *config_key = model == EN_ADPT_MODEL_I2S_SAMPLE_PEER ? "native_i2s_peer" :
                model == EN_ADPT_MODEL_I2C_SCRIPTED_MASTER ? "native_i2c_script" :
                model == EN_ADPT_MODEL_ST7789_I80 || model == EN_ADPT_MODEL_RGB_PANEL ?
                "native_lcd_panel" : model == EN_ADPT_MODEL_OV2640_DVP ? "native_camera" : NULL;
            if (config_key) {
                QDict *attributes = qobject_to(QDict, qdict_get(c, "attributes"));
                if (!attributes || !qobject_to(QDict, qdict_get(attributes, config_key))) {
                    error_setg(errp, "Device '%s' requires attributes.%s config", cid, config_key);
                    return false;
                }
            }
            size_t di = p->device_count++;
            p->models[di] = (EnAdptModelBinding){ci, model};
            p->devices[di] = (Esp32S3ProjectDevice){
                .component_index = ci, .model = model,
                .sda_terminal = flat_role(p, component, "sda"),
                .scl_terminal = flat_role(p, component, "scl"),
                .mosi_terminal = flat_role(p, component, "mosi"),
                .miso_terminal = flat_role(p, component, "miso"),
                .sclk_terminal = flat_role(p, component, "sclk"),
                .cs_terminal = flat_role(p, component, "cs"),
                .bclk_terminal = flat_role(p, component, "bclk"),
                .ws_terminal = flat_role(p, component, "ws"),
                .din_terminal = flat_role(p, component, "din"),
                .dout_terminal = flat_role(p, component, "dout"),
                .vdd_terminal = flat_role(p, component, "vdd"),
                .gnd_terminal = flat_role(p, component, "gnd")
            };
            Esp32S3ProjectDevice *device = &p->devices[di];
            for (size_t ti = 0; ti < component->terminal_count; ++ti) {
                int open_drain;
                const EnAdptTerminal *terminal = &component->terminals[ti];
                if (en_adpt_model_terminal_driver(model, terminal->role, &open_drain)) {
                    if (device->drive_count == ESP32S3_PROJECT_MAX_MODEL_DRIVES) {
                        error_setg(errp, "Device '%s' exceeds bounded native output capacity", cid);
                        return false;
                    }
                    device->drives[device->drive_count++] = (Esp32S3ProjectDrive){
                        .flat_terminal = terminal - p->terminals,
                        .open_drain = open_drain
                    };
                }
            }
            for (size_t i = 0; i < np; ++i) {
                if (!strcmp(component->parameters[i].name, "address") &&
                    component->parameters[i].quantity.unit == EN_ADPT_UNIT_COUNT &&
                    component->parameters[i].quantity.value >= 1 &&
                    component->parameters[i].quantity.value <= 127) {
                    p->devices[di].address = component->parameters[i].quantity.value;
                }
            }
            if (model == EN_ADPT_MODEL_OV2640_DVP) {
                device->address = 0x30;
            }
            g_string_append_printf(signature, "model:%d;", model);
        }
    }
    if (mcu_count != 1 || p->vdd_terminal < 0 || p->gnd_terminal < 0) {
        error_setg(errp, "Native circuit requires exactly one MCU and explicit vdd/gnd terminals");
        return false;
    }
    g_autoptr(GPtrArray) nets = objects(qobject_to(QList, qdict_get(root, "nets")), EN_ADPT_MAX_NETS, errp);
    if (!nets) {
        return false;
    }
    size_t ne = 0;
    for (size_t ni = 0; ni < nets->len; ++ni) {
        QDict *n = nets->pdata[ni];
        EnAdptNet *net = &p->nets[ni];
        const char *nid = text(n, "id", errp);
        if (!nid || !identity(&net->id, nid, errp) || !text(n, "name", errp)) {
            return false;
        }
        QList *endpoints = qobject_to(QList, qdict_get(n, "endpoints"));
        if (!endpoints || qlist_size(endpoints) > EN_ADPT_MAX_TERMINALS - ne) {
            error_setg(errp, "Net '%s' has missing/unbounded endpoint array", nid);
            return false;
        }
        net->endpoints = &p->endpoints[ne];
        const QListEntry *entry;
        QLIST_FOREACH_ENTRY(endpoints, entry) {
            QString *s = qobject_to(QString, qlist_entry_obj(entry));
            if (!s || !identity(&p->endpoints[ne++], qstring_get_str(s), errp)) {
                if (!s) {
                    error_setg(errp, "Endpoint requires an actual global terminal ID string");
                }
                return false;
            }
            ++net->endpoint_count;
        }
        qsort((void *)net->endpoints, net->endpoint_count, sizeof(EnAdptId), endpoint_cmp);
        g_string_append_printf(signature, "n:%s;", nid);
        for (size_t i = 0; i < net->endpoint_count; ++i) {
            g_string_append_printf(signature, "e:%s;", net->endpoints[i].text);
        }
    }
    p->graph = (EnAdptGraph){EN_ADPT_ABI_VERSION, p->components, components->len, p->nets, nets->len};
    g_autofree EnAdptResult *result = g_new(EnAdptResult, 1);
    if (en_adpt_project_models(&p->graph, p->rc, p->models, p->device_count,
                              &p->projection, result) != EN_ADPT_OK) {
        error_setg(errp, "v3 electrical validation: %s (offender '%s')", result->diagnostic, result->offender);
        return false;
    }
    for (size_t pin = 0; pin < 49; ++pin) {
        int terminal = p->pad_terminal[pin];
        if (!reserved[pin] || terminal < 0) {
            continue;
        }
        for (size_t ni = 0; ni < p->graph.net_count; ++ni) {
            for (size_t ei = 0; ei < p->nets[ni].endpoint_count; ++ei) {
                if (!strcmp(p->terminals[terminal].id.text, p->nets[ni].endpoints[ei].text)) {
                    error_setg(errp, "GPIO%zu is reserved in the selected module/profile", pin);
                    return false;
                }
            }
        }
    }
    p->identity = g_string_free(g_steal_pointer(&signature), false);
    return true;
}

Esp32S3Project *esp32s3_project_parse(const char *json, Error **errp)
{
    if (!json || strlen(json) > 1024 * 1024) {
        error_setg(errp, "Project JSON exceeds the 1 MiB native boundary");
        return NULL;
    }
    QObject *root = qobject_from_json(json, errp);
    if (!root) {
        return NULL;
    }
    Esp32S3Project *p = g_new0(Esp32S3Project, 1);
    p->document = root;
    p->vdd_terminal = p->gnd_terminal = -1;
    for (size_t i = 0; i < G_N_ELEMENTS(p->pad_terminal); ++i) {
        p->pad_terminal[i] = -1;
    }
    QDict *d = qobject_to(QDict, root);
    if (!d) {
        error_setg(errp, "Project root requires an object");
    }
    if (!d || !parse(p, d, errp)) {
        esp32s3_project_free(p);
        return NULL;
    }
    return p;
}

void esp32s3_project_free(Esp32S3Project *p)
{
    if (p) {
        for (size_t i = 0; i < p->device_count; ++i) {
            g_free(p->devices[i].factory_identity);
        }
        qobject_unref(p->document);
        g_free(p->identity);
        g_free(p);
    }
}
