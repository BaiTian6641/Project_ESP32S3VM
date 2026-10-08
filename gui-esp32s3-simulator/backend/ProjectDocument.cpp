#include "ProjectDocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QRegularExpression>
#include <QUndoCommand>
#include <QUuid>
#include <QtMath>
#include <algorithm>
#include <functional>

namespace {
QString newId(const QString &prefix) { return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool physicalGpio(int gpio) { return gpio >= 0 && gpio <= 48 && (gpio < 22 || gpio > 25); }
bool integer(const QJsonValue &value) { return value.isDouble() && qIsFinite(value.toDouble()) && qFloor(value.toDouble()) == value.toDouble(); }
int findId(const QJsonArray &array, const QString &id) {
    for (int i = 0; i < array.size(); ++i) if (array[i].toObject().value("id").toString() == id) return i;
    return -1;
}
QString baseFor(const QString &path) { return path.isEmpty() ? QDir::currentPath() : QFileInfo(path).absolutePath(); }
bool networkPath(const QString &path) {
    // Backslash UNC is unambiguous Windows syntax. A leading // is ambiguous
    // between UNC and implementation-defined POSIX/network paths: preserve it
    // verbatim and require explicit host mapping rather than guessing.
    return path.startsWith("\\\\") || path.startsWith("//");
}
struct Unit { QString dimension; double scale = 1; double offset = 0; };
const QMap<QString, Unit> &units() {
    static const QMap<QString, Unit> table {
        {"V", {"voltage"}}, {"mV", {"voltage", .001}},
        {"A", {"current"}}, {"mA", {"current", .001}},
        {"ohm", {"resistance"}}, {"kohm", {"resistance", 1000}}, {"Mohm", {"resistance", 1000000}},
        {"F", {"capacitance"}}, {"uF", {"capacitance", .000001}},
        {"nF", {"capacitance", .000000001}}, {"pF", {"capacitance", .000000000001}},
        {"ratio", {"ratio"}}, {"%", {"ratio", .01}}, {"Hz", {"frequency"}},
        {"s", {"time"}}, {"ms", {"time", .001}}, {"degC", {"temperature", 1, 273.15}},
        {"Pa", {"pressure"}}, {"count", {"count"}}, {"bool", {"boolean"}}, {"state", {"switch-state"}}
    };
    return table;
}
std::optional<double> siValue(const QJsonObject &quantity) {
    const auto unit = units().constFind(quantity.value("unit").toString());
    if (unit == units().cend() || unit->dimension == "boolean" || unit->dimension == "switch-state" || !quantity.value("value").isDouble()
        || !qIsFinite(quantity.value("value").toDouble())) return {};
    const double normalized = quantity.value("value").toDouble() * unit->scale + unit->offset;
    return qIsFinite(normalized) ? std::optional<double>(normalized) : std::nullopt;
}

using PathMapper = std::function<QString(const QString &, bool)>;
void mapField(QJsonObject &object, const QString &key, bool filePath, const PathMapper &map) {
    if (object.value(key).isString() && !object.value(key).toString().isEmpty()) object[key] = map(object[key].toString(), filePath);
}
void mapDevice(QJsonObject &device, const PathMapper &map) {
    auto simulator = device.value("simulator").toObject();
    mapField(simulator, "exec", false, map);
    if (device.value("simulator").isObject()) device["simulator"] = simulator;
    auto properties = device.value("properties").toObject();
    for (const auto &key : {"csv_example", "csv_path"}) mapField(properties, key, true, map);
    if (device.value("properties").isObject()) device["properties"] = properties;
}
void mapPaths(QJsonObject &json, const PathMapper &map) {
    auto firmware = json.value("firmware").toObject();
    mapField(firmware, "path", true, map);
    auto artifacts = firmware.value("artifacts").toArray();
    for (int i = 0; i < artifacts.size(); ++i) {
        if (!artifacts[i].isObject()) continue;
        auto artifact = artifacts[i].toObject(); mapField(artifact, "path", true, map); artifacts[i] = artifact;
    }
    if (firmware.value("artifacts").isArray()) firmware["artifacts"] = artifacts;
    if (json.value("firmware").isObject()) json["firmware"] = firmware;
    auto runtime = json.value("runtime").toObject();
    mapField(runtime, "qemu_executable", false, map);
    for (const auto &key : {"rom_path", "working_directory"}) mapField(runtime, key, true, map);
    if (json.value("runtime").isObject()) json["runtime"] = runtime;
    auto components = json.value("components").toArray();
    for (int i = 0; i < components.size(); ++i) {
        if (!components[i].isObject()) continue;
        auto component = components[i].toObject();
        mapDevice(component, map);
        auto legacy = component.value("legacy").toObject(); mapDevice(legacy, map);
        if (component.value("legacy").isObject()) component["legacy"] = legacy;
        auto catalogue = component.value("catalogue").toObject();
        mapField(catalogue, "source_project_path", true, map);
        auto templateDevice = catalogue.value("template_device").toObject();
        mapDevice(templateDevice, map);
        if (catalogue.value("template_device").isObject()) catalogue["template_device"] = templateDevice;
        if (component.value("catalogue").isObject()) component["catalogue"] = catalogue;
        auto attributes = component.value("attributes").toObject();
        mapDevice(attributes, map);
        if (component.value("attributes").isObject()) component["attributes"] = attributes;
        auto resources = component.value("resources").toObject();
        for (auto it = resources.begin(); it != resources.end(); ++it) {
            if (!it.value().isObject()) continue;
            auto resource = it.value().toObject(); mapField(resource, "path", true, map); it.value() = resource;
        }
        if (component.value("resources").isObject()) component["resources"] = resources;
        components[i] = component;
    }
    if (json.value("components").isArray()) json["components"] = components;
    auto devices = json.value("devices").toArray();
    for (int i = 0; i < devices.size(); ++i) { if (!devices[i].isObject()) continue; auto device = devices[i].toObject(); mapDevice(device, map); devices[i] = device; }
    if (json.value("devices").isArray()) json["devices"] = devices;
    auto migration = json.value("migration").toObject();
    if (migration.value("legacy_source").isObject()) {
        auto source = migration.value("legacy_source").toObject(); mapPaths(source, map); migration["legacy_source"] = source;
        json["migration"] = migration;
    }
}

QJsonObject migrateV2(const QJsonObject &source) {
    QJsonArray report;
    auto note = [&report](const QString &code, const QString &message, bool error = false, const QString &owner = QString()) {
        QJsonObject entry{{"severity", error ? "error" : "warning"}, {"code", code}, {"message", message}};
        if (!owner.isEmpty()) entry["original_component_id"] = owner;
        report.append(entry);
    };
    if (!source.value("board").isObject() || source.value("board").toObject().value("name").toString().isEmpty())
        note("legacy-board-invalid", "Legacy board metadata is missing or malformed.", true);
    if (!source.value("devices").isArray()) note("legacy-devices-invalid", "Legacy devices must be an array; the original value is retained.", true);
    if (source.contains("buses") && !source.value("buses").isArray()) note("legacy-buses-invalid", "Legacy bus registry must be an array.", true);
    const auto oldBoard = source.value("board").toObject();
    QJsonObject profile{{"chip", oldBoard.value("target").toString("esp32s3")},
        {"board", oldBoard.value("name").toString("unspecified")},
        {"module", oldBoard.value("module").toString("unspecified")},
        {"reservations_known", false}, {"reserved_gpios", QJsonArray{}}, {"legacy_board", oldBoard}};
    QJsonArray components;
    QSet<QString> oldIds;
    for (const auto &value : source.value("devices").toArray()) oldIds.insert(value.toObject().value("id").toString());
    QString mcuId = "mcu";
    while (oldIds.contains(mcuId)) mcuId += "-board";
    QJsonArray pads;
    for (int gpio = 0; gpio <= 48; ++gpio) if (physicalGpio(gpio))
        pads.append(QJsonObject{{"id", mcuId + ".gpio" + QString::number(gpio)},
            {"name", "GPIO " + QString::number(gpio)}, {"role", "gpio"},
            {"domain", "digital"}, {"direction", "inout"}, {"gpio", gpio}});
    components.append(QJsonObject{{"id", mcuId}, {"name", "ESP32-S3"}, {"kind", "mcu"},
        {"type", "esp32s3"}, {"terminals", pads}, {"parameters", QJsonObject{}}});
    QMap<int, QJsonArray> byGpio;
    QMap<QString, QJsonObject> buses;
    for (const auto &value : source.value("buses").toArray()) {
        if (!value.isObject()) { note("legacy-bus-invalid", "Legacy bus entry is not an object.", true); continue; }
        const auto bus = value.toObject(); const auto id = bus.value("id").toString();
        if (buses.contains(id)) note("legacy-bus-id-duplicate", "Legacy bus IDs are duplicated: " + id, true);
        buses[id] = bus;
    }
    for (const auto &value : source.value("devices").toArray()) {
        if (!value.isObject()) { note("legacy-device-invalid", "Legacy device entry is not an object; original data is retained.", true); continue; }
        const auto device = value.toObject(); const auto id = device.value("id").toString();
        if (device.contains("bus") && !device.value("bus").isObject()) note("legacy-bus-invalid", "Legacy device bus is malformed: " + id, true, id);
        if (!device.value("simulator").isObject() || device.value("simulator").toObject().value("exec").toString().isEmpty()) note("legacy-simulator-invalid", "Legacy simulator executable is missing: " + id, true, id);
        auto bus = device.value("bus").toObject();
        const auto busRef = device.value("bus_ref").toString();
        if (!busRef.isEmpty()) {
            if (!buses.contains(busRef)) note("legacy-bus-reference", "Missing legacy bus registry entry: " + busRef, true, id);
            else { auto merged = buses[busRef]; for (auto it = bus.begin(); it != bus.end(); ++it) merged[it.key()] = it.value(); bus = merged; }
        }
        if (!device.contains("bus") && busRef.isEmpty()) note("legacy-bus-missing", "Legacy device has no bus declaration: " + id, true, id);
        if (bus.contains("pins") && !bus.value("pins").isObject()) note("legacy-pins-invalid", "Legacy pin assignments must be an object: " + id, true, id);
        QJsonArray terminals;
        const auto pins = bus.value("pins").toObject();
        for (auto it = pins.begin(); it != pins.end(); ++it) {
            const auto terminal = id + "." + it.key();
            terminals.append(QJsonObject{{"id", terminal}, {"name", it.key().toUpper()}, {"role", "signal"},
                {"domain", "digital"}, {"direction", "unspecified"}, {"attributes", QJsonObject{{"legacy_signal", it.key()}}}});
            if (integer(it.value()) && it.value().toInt() == -1) continue;
            if (!integer(it.value()) || !physicalGpio(it.value().toInt())) {
                note("legacy-gpio-invalid", "Invalid GPIO assignment retained for " + terminal, true, id); continue;
            }
            const int gpio = it.value().toInt();
            if (!byGpio.contains(gpio)) byGpio[gpio].append(mcuId + ".gpio" + QString::number(gpio));
            byGpio[gpio].append(terminal);
        }
        components.append(QJsonObject{{"id", id}, {"name", id}, {"kind", "device"}, {"type", device.value("type")},
            {"terminals", terminals}, {"parameters", QJsonObject{}}, {"simulator", device.value("simulator")},
            {"attributes", QJsonObject{{"decoder", bus}, {"properties", device.value("properties").toObject()}}},
            {"legacy", device}});
    }
    QJsonArray nets;
    for (auto it = byGpio.begin(); it != byGpio.end(); ++it)
        nets.append(QJsonObject{{"id", "gpio-" + QString::number(it.key())}, {"name", "GPIO " + QString::number(it.key())},
            {"endpoints", it.value()}, {"attributes", QJsonObject{{"provenance", "legacy-pin-assignment"}}}});
    note("legacy-connectivity-assumed", "Nets reflect declared GPIO assignments only; controller/address metadata is not electrical evidence.");
    note("legacy-direction-unspecified", "Legacy signal names are retained as labels; device drive direction was not inferred.");
    note("legacy-power-unmodeled", "No power, ground, voltage source or pull-up was invented. Add explicit components before physical simulation.");
    note("legacy-profile-unqualified", "Legacy module variant and pin reservations require review.");
    auto layout = source.value("ui_layout").toObject();
    return QJsonObject{{"version", 3}, {"id", newId("project-")}, {"name", oldBoard.value("name").toString("Migrated project")},
        {"profile", profile}, {"firmware", source.value("firmware").toObject()}, {"runtime", source.value("runtime").toObject()},
        {"components", components}, {"nets", nets},
        {"geometry", QJsonObject{{"components", layout}, {"nets", QJsonObject{}}}},
        {"migration", QJsonObject{{"from_version", 2}, {"report", report}, {"legacy_source", source}}}};
}

QList<QStringList> endpointSets(const QJsonArray &nets) {
    QList<QStringList> sets;
    for (const auto &value : nets) {
        QStringList endpoints;
        for (const auto &endpoint : value.toObject().value("endpoints").toArray()) endpoints.append(endpoint.toString());
        endpoints.sort(); sets.append(endpoints);
    }
    std::sort(sets.begin(), sets.end()); return sets;
}
}

class ProjectEditCommand : public QUndoCommand {
public:
    ProjectEditCommand(ProjectDocument *document, QJsonObject before, QJsonObject after, const QString &label)
        : QUndoCommand(label), m_document(document), m_before(std::move(before)), m_after(std::move(after)) {}
    void undo() override { m_document->replaceState(m_before); }
    void redo() override { m_document->replaceState(m_after); }
private:
    ProjectDocument *m_document;
    QJsonObject m_before, m_after;
};

ProjectDocument::ProjectDocument(QObject *parent) : QObject(parent), m_undo(this) {
    m_json = QJsonObject{{"version", 3}, {"id", newId("project-")}, {"name", "Untitled"},
        {"profile", QJsonObject{{"chip", "esp32s3"}, {"board", "unspecified"}, {"module", "unspecified"},
            {"reservations_known", false}, {"reserved_gpios", QJsonArray{}}}},
        {"firmware", QJsonObject{{"artifacts", QJsonArray{}}}}, {"runtime", QJsonObject{}},
        {"components", QJsonArray{}}, {"nets", QJsonArray{}},
        {"geometry", QJsonObject{{"components", QJsonObject{}}, {"nets", QJsonObject{}}}}};
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this]() { emit dirtyChanged(isDirty()); });
}
bool ProjectDocument::isValidIdentifier(const QString &id) {
    if (id.isEmpty() || id.size() > MaximumIdentifierLength) return false;
    static const QRegularExpression pattern("\\A[A-Za-z0-9][A-Za-z0-9_.:/-]{0,63}\\z");
    return pattern.match(id).hasMatch();
}
QStringList ProjectDocument::parameterUnits() { return units().keys(); }
QJsonObject ProjectDocument::quantityForUnit(QJsonObject quantity, const QString &unitName) {
    const auto from = units().constFind(quantity.value("unit").toString());
    const auto to = units().constFind(unitName);
    if (from != units().cend() && to != units().cend() && from->dimension == to->dimension
        && from->dimension != "boolean" && from->dimension != "switch-state") {
        const auto canonical = siValue(quantity);
        if (canonical) {
            const double value = (*canonical - to->offset) / to->scale;
            if (qIsFinite(value)) quantity["value"] = value;
        }
    }
    quantity["unit"] = unitName;
    return quantity;
}
void ProjectDocument::setLegacyResourceRoot(const QString &path) { m_legacyRoot = path.isEmpty() ? QString() : QDir(path).absolutePath(); }
bool ProjectDocument::loadFile(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(error, "Project JSON must be an object: " + parse.errorString());
    return loadJson(document.object(), path, error);
}
bool ProjectDocument::loadJson(const QJsonObject &json, const QString &sourcePath, QString *error) {
    if (!integer(json.value("version")) || (json.value("version").toInt() != 2 && json.value("version").toInt() != 3))
        return fail(error, "Only project versions 2 and 3 are supported.");
    const bool migrated = json.value("version").toInt() == 2;
    auto next = migrated ? migrateV2(json) : json;
    next = normalizePaths(next, baseFor(sourcePath), migrated);
    m_path = sourcePath.isEmpty() ? QString() : QFileInfo(sourcePath).absoluteFilePath();
    m_migratedUnsaved = migrated || sourcePath.isEmpty() || !QFileInfo::exists(sourcePath);
    m_undo.clear();
    replaceState(next);
    emit dirtyChanged(isDirty());
    if (error) error->clear();
    return true;
}
QJsonObject ProjectDocument::normalizePaths(QJsonObject json, const QString &base, bool legacySearch) const {
    mapPaths(json, [&](const QString &raw, bool filePath) {
        if (networkPath(raw)) return raw;
        if (QFileInfo(raw).isAbsolute()) return QDir::cleanPath(raw);
        if (raw.contains("://") || QRegularExpression("^[A-Za-z]:[\\\\/]").match(raw).hasMatch()) return raw;
        if (!filePath && !raw.contains('/') && !raw.contains('\\')) return raw; // PATH executable, not a relative file.
        const QString direct = QDir(base).absoluteFilePath(raw);
        if (legacySearch && !QFileInfo::exists(direct)) {
            const auto parent = QDir(base + "/..").absoluteFilePath(raw);
            if (QFileInfo::exists(parent)) return QDir::cleanPath(parent);
            if (!m_legacyRoot.isEmpty()) {
                const auto root = QDir(m_legacyRoot).absoluteFilePath(raw);
                if (QFileInfo::exists(root)) return QDir::cleanPath(root);
            }
        }
        return QDir::cleanPath(direct);
    });
    return json;
}
QJsonObject ProjectDocument::serializedFor(const QString &path) const {
    auto json = m_json;
    mapPaths(json, [&](const QString &raw, bool) {
        if (networkPath(raw)) return raw;
        if (!QFileInfo(raw).isAbsolute()) return raw;
        const auto relative = QDir(baseFor(path)).relativeFilePath(raw);
        return relative.startsWith('.') || QFileInfo(relative).isAbsolute() ? relative : "./" + relative;
    });
    return json;
}
QJsonObject ProjectDocument::toJson() const { return serializedFor(m_path); }
QJsonObject ProjectDocument::profile() const { return m_json.value("profile").toObject(); }
QJsonObject ProjectDocument::firmware() const { return m_json.value("firmware").toObject(); }
QJsonObject ProjectDocument::runtime() const { return m_json.value("runtime").toObject(); }
QJsonObject ProjectDocument::geometry() const { return m_json.value("geometry").toObject(); }
QList<ProjectComponent> ProjectDocument::components() const {
    QList<ProjectComponent> result;
    for (const auto &value : m_json.value("components").toArray()) {
        const auto json = value.toObject(); ProjectComponent component;
        component.id = json.value("id").toString(); component.name = json.value("name").toString();
        component.kind = json.value("kind").toString(); component.type = json.value("type").toString();
        component.simulator = json.value("simulator").toObject(); component.attributes = json.value("attributes").toObject();
        for (const auto &v : json.value("terminals").toArray()) {
            const auto t = v.toObject(); ProjectTerminal terminal;
            terminal.id = t.value("id").toString(); terminal.name = t.value("name").toString(); terminal.role = t.value("role").toString();
            terminal.domain = t.value("domain").toString(); terminal.direction = t.value("direction").toString();
            if (integer(t.value("gpio"))) terminal.gpio = t.value("gpio").toInt();
            terminal.attributes = t.value("attributes").toObject(); component.terminals.append(terminal);
        }
        const auto parameters = json.value("parameters").toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            const auto quantity = it.value().toObject();
            component.parameters[it.key()] = {quantity.value("value"), quantity.value("unit").toString(), siValue(quantity)};
        }
        result.append(component);
    }
    return result;
}
QList<ProjectNet> ProjectDocument::nets() const {
    QList<ProjectNet> result;
    for (const auto &value : m_json.value("nets").toArray()) {
        const auto json = value.toObject(); ProjectNet net;
        net.id = json.value("id").toString(); net.name = json.value("name").toString(); net.attributes = json.value("attributes").toObject();
        for (const auto &endpoint : json.value("endpoints").toArray()) net.endpoints.append(endpoint.toString());
        result.append(net);
    }
    return result;
}

QList<ProjectDiagnostic> ProjectDocument::diagnostics() const {
    QList<ProjectDiagnostic> result;
    auto issue = [&](const QString &code, const QString &path, const QString &message, bool warning = false) {
        result.append({warning ? ProjectDiagnostic::Severity::Warning : ProjectDiagnostic::Severity::Error, code, path, message});
    };
    auto nonempty = [&](const QJsonObject &object, const QString &key, const QString &path) {
        if (!object.value(key).isString() || object.value(key).toString().trimmed().isEmpty()) issue("required-string", path + "/" + key, "A nonempty string is required.");
    };
    auto identifier = [&](const QJsonValue &value, const QString &path) {
        if (!value.isString() || !isValidIdentifier(value.toString())) issue("identifier-invalid", path, "IDs require 1-64 ASCII characters: first alphanumeric, then alphanumeric or _ . : / -.");
    };
    if (m_json.value("version").toInt() != 3) issue("schema-version", "/version", "The in-memory schema must be version 3.");
    nonempty(m_json, "id", ""); nonempty(m_json, "name", "");
    identifier(m_json.value("id"), "/id");
    for (const auto &key : {"profile", "firmware", "runtime", "geometry"}) if (!m_json.value(key).isObject()) issue("object-required", "/" + QString(key), "An object is required.");
    const auto board = profile();
    for (const auto &key : {"chip", "board", "module"}) nonempty(board, key, "/profile");
    if (board.value("chip").toString() != "esp32s3") issue("chip-unsupported", "/profile/chip", "This document implementation supports the ESP32-S3 pin map.");
    QSet<int> reserved;
    if (!board.value("reserved_gpios").isUndefined() && !board.value("reserved_gpios").isArray()) issue("array-required", "/profile/reserved_gpios", "Reservations must be an array.");
    for (const auto &value : board.value("reserved_gpios").toArray()) {
        const auto pin = value.isObject() ? value.toObject().value("gpio") : value;
        if (!integer(pin) || !physicalGpio(pin.toInt())) issue("reservation-invalid", "/profile/reserved_gpios", "A reservation must refer to a physical ESP32-S3 GPIO.");
        else reserved.insert(pin.toInt());
    }
    // Explicitly documented DevKitC Octal-memory variants, not a wildcard
    // inference from a board family name. A full profile catalog comes later.
    const auto module = board.value("module").toString().toLower();
    if (QSet<QString>{"esp32-s3-wroom-1-n8r8", "esp32-s3-wroom-1u-n8r8", "esp32-s3-wroom-2-n32r16v"}.contains(module))
        reserved.unite(QSet<int>{35, 36, 37});
    if (!board.value("reservations_known").toBool()) issue("profile-unqualified", "/profile", "Module/exposed-pin reservations have not been fully qualified.", true);
    if (board.contains("reservations_known") && !board.value("reservations_known").isBool()) issue("boolean-required", "/profile/reservations_known", "Reservation qualification must be a boolean.");
    for (const auto &key : {"components", "nets"}) if (!m_json.value(key).isArray()) issue("array-required", "/" + QString(key), "An array is required.");
    QSet<QString> componentIds, terminalIds, netIds;
    QMap<QString, int> gpioByTerminal;
    const QSet<QString> kinds{"mcu", "device", "resistor", "capacitor", "voltage-source", "current-source", "potentiometer", "switch", "ground"};
    const QSet<QString> domains{"digital", "analog", "power", "ground", "passive", "unspecified"};
    const QSet<QString> directions{"input", "output", "inout", "passive", "unspecified"};
    QSet<QString> currentLegacyFaults;
    const auto legacySource = m_json.value("migration").toObject().value("legacy_source").toObject();
    auto legacyIssue = [&](const QString &code, const QString &owner, const QString &path, const QString &message) {
        currentLegacyFaults.insert(code + "\n" + owner); issue(code, path, message);
    };
    int componentIndex = 0;
    for (const auto &value : m_json.value("components").toArray()) {
        const auto path = "/components/" + QString::number(componentIndex++);
        if (!value.isObject()) { issue("object-required", path, "A component must be an object."); continue; }
        const auto component = value.toObject(); const auto id = component.value("id").toString();
        for (const auto &key : {"simulator", "attributes", "legacy", "resources", "catalogue"}) if (component.contains(key) && !component.value(key).isObject()) issue("object-required", path + "/" + key, "An object is required.");
        if (component.contains("catalogue")) {
            const auto catalogue = component.value("catalogue").toObject();
            nonempty(catalogue, "source_project_path", path + "/catalogue");
            if (!catalogue.value("template_device").isObject()) issue("object-required", path + "/catalogue/template_device", "Full template provenance must be an object.");
        }
        const auto resources = component.value("resources").toObject();
        for (auto it = resources.begin(); it != resources.end(); ++it) {
            if (!it.value().isObject()) issue("object-required", path + "/resources/" + it.key(), "A declared resource requires an object.");
            else nonempty(it.value().toObject(), "path", path + "/resources/" + it.key());
        }
        for (const auto &key : {"id", "name", "kind", "type"}) nonempty(component, key, path);
        identifier(component.value("id"), path + "/id");
        if (componentIds.contains(id)) issue("component-id-duplicate", path + "/id", "Component IDs must be unique.");
        componentIds.insert(id);
        const auto kind = component.value("kind").toString();
        if (!kinds.contains(kind)) issue("component-kind-unknown", path + "/kind", "Unknown component kind.");
        if (!component.value("terminals").isArray()) issue("array-required", path + "/terminals", "Typed terminals must be an array.");
        QSet<QString> roles;
        QSet<int> componentGpios;
        int terminalIndex = 0;
        for (const auto &v : component.value("terminals").toArray()) {
            const auto terminalPath = path + "/terminals/" + QString::number(terminalIndex++);
            if (!v.isObject()) { issue("object-required", terminalPath, "A terminal must be an object."); continue; }
            const auto terminal = v.toObject(); const auto terminalId = terminal.value("id").toString();
            if (terminal.contains("attributes") && !terminal.value("attributes").isObject()) issue("object-required", terminalPath + "/attributes", "An object is required.");
            for (const auto &key : {"id", "name", "role"}) nonempty(terminal, key, terminalPath);
            identifier(terminal.value("id"), terminalPath + "/id");
            if (terminalIds.contains(terminalId)) issue("terminal-id-duplicate", terminalPath + "/id", "Terminal IDs are globally unique.");
            terminalIds.insert(terminalId); roles.insert(terminal.value("role").toString());
            if (!domains.contains(terminal.value("domain").toString())) issue("terminal-domain-unknown", terminalPath + "/domain", "Unknown terminal domain.");
            if (!directions.contains(terminal.value("direction").toString())) issue("terminal-direction-unknown", terminalPath + "/direction", "Unknown terminal direction.");
            if (terminal.contains("gpio")) {
                const auto gpio = terminal.value("gpio");
                if (kind != "mcu") issue("gpio-owner-invalid", terminalPath + "/gpio", "GPIO identity belongs to an MCU terminal.");
                if (!integer(gpio) || !physicalGpio(gpio.toInt())) issue("gpio-nonexistent", terminalPath + "/gpio", "GPIO22-25 do not exist; valid S3 GPIOs are 0-21 and 26-48.");
                else {
                    if (componentGpios.contains(gpio.toInt())) issue("gpio-identity-duplicate", terminalPath + "/gpio", "One MCU pad cannot have multiple terminal identities.");
                    componentGpios.insert(gpio.toInt()); gpioByTerminal[terminalId] = gpio.toInt();
                }
            }
        }
        QSet<QString> requiredRoles;
        if (kind == "resistor" || kind == "switch") requiredRoles = {"a", "b"};
        else if (kind == "capacitor" || kind == "voltage-source" || kind == "current-source") requiredRoles = {"p", "n"};
        else if (kind == "potentiometer") requiredRoles = {"a", "w", "b"};
        else if (kind == "ground") requiredRoles = {"ref"};
        if (!requiredRoles.isEmpty() && (roles != requiredRoles || terminalIndex != requiredRoles.size()))
            issue("terminal-roles-invalid", path + "/terminals", "This analog component requires exactly its documented electrical terminal roles.");
        if (!component.value("parameters").isObject()) issue("object-required", path + "/parameters", "Parameters must be named quantities.");
        const auto parameters = component.value("parameters").toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            const auto parameterPath = path + "/parameters/" + it.key();
            const auto quantity = it.value().toObject(); const auto unitName = quantity.value("unit").toString();
            const auto unit = units().constFind(unitName);
            if (!it.value().isObject() || !quantity.contains("value")) issue("quantity-required", parameterPath, "A parameter requires value and unit.");
            if (unit == units().cend()) { issue("unit-unknown", parameterPath + "/unit", "Unknown unit."); continue; }
            const auto raw = quantity.value("value");
            if (unit->dimension == "boolean") { if (!raw.isBool()) issue("value-invalid", parameterPath, "A bool quantity requires a boolean."); }
            else if (unit->dimension == "switch-state") { if (!raw.isString() || (raw.toString() != "open" && raw.toString() != "closed")) issue("value-invalid", parameterPath, "Switch state must be open or closed."); }
            else {
                const auto number = siValue(quantity);
                if (!number) issue("value-invalid", parameterPath, "A finite numeric value is required.");
                else if ((unit->dimension == "resistance" || unit->dimension == "capacitance") && *number <= 0)
                    issue("value-nonpositive", parameterPath, "Resistance and capacitance must be positive.");
            }
        }
        QMap<QString, QString> required;
        if (kind == "resistor" || kind == "potentiometer") required["resistance"] = "resistance";
        if (kind == "capacitor") required["capacitance"] = "capacitance";
        if (kind == "voltage-source") required["voltage"] = "voltage";
        if (kind == "current-source") required["current"] = "current";
        if (kind == "potentiometer") required["position"] = "ratio";
        if (kind == "switch") required["state"] = "switch-state";
        if (parameters.contains("initial_voltage") && kind == "capacitor") required["initial_voltage"] = "voltage";
        if (parameters.contains("series_resistance") && (kind == "voltage-source" || kind == "current-source")) required["series_resistance"] = "resistance";
        if (kind == "switch") for (const auto &name : {"on_resistance", "off_resistance"}) if (parameters.contains(name)) required[name] = "resistance";
        for (auto it = required.begin(); it != required.end(); ++it) {
            const auto q = parameters.value(it.key()).toObject(); const auto u = units().constFind(q.value("unit").toString());
            if (!parameters.contains(it.key()) || u == units().cend() || u->dimension != it.value()) issue("parameter-dimension-invalid", path + "/parameters/" + it.key(), "Missing parameter or incorrect unit dimension.");
        }
        if (kind == "potentiometer") {
            const auto position = siValue(parameters.value("position").toObject());
            if (position && (*position < 0 || *position > 1)) issue("position-out-of-range", path + "/parameters/position", "Wiper position must be between 0 and 1.");
        }
        if (component.value("legacy").isObject()) {
            const auto simulator = component.value("simulator").toObject();
            if (!simulator.value("exec").isString() || simulator.value("exec").toString().trimmed().isEmpty())
                legacyIssue("legacy-simulator-invalid", id, path + "/simulator/exec", "Current migrated device needs a simulator executable.");
            const auto attributes = component.value("attributes").toObject(); const auto decoder = attributes.value("decoder").toObject();
            if (!attributes.value("decoder").isObject() || decoder.value("kind").toString().isEmpty() || decoder.value("controller").toString().isEmpty())
                legacyIssue(component.value("legacy").toObject().contains("bus") ? "legacy-bus-invalid" : "legacy-bus-missing", id, path + "/attributes/decoder", "Current migrated device needs explicit decoder metadata.");
            if (decoder.contains("pins") && !decoder.value("pins").isObject())
                legacyIssue("legacy-pins-invalid", id, path + "/attributes/decoder/pins", "Current cached GPIO assignments must be an object.");
            const auto pins = decoder.value("pins").toObject();
            for (auto it = pins.begin(); it != pins.end(); ++it) if (!integer(it.value()) || (it.value().toInt() != -1 && !physicalGpio(it.value().toInt())))
                legacyIssue("legacy-gpio-invalid", id, path + "/attributes/decoder/pins/" + it.key(), "Current cached assignment is not a physical GPIO or disconnected (-1).");
            const auto busRef = component.value("legacy").toObject().value("bus_ref").toString();
            if (!busRef.isEmpty()) {
                int matches = 0;
                for (const auto &bus : legacySource.value("buses").toArray()) if (bus.toObject().value("id").toString() == busRef) ++matches;
                if (matches == 0) legacyIssue("legacy-bus-reference", id, path + "/legacy/bus_ref", "Current compatibility metadata references a missing retained bus.");
                if (matches > 1) legacyIssue("legacy-bus-id-duplicate", id, path + "/legacy/bus_ref", "Current compatibility metadata references an ambiguous retained bus ID.");
            }
        }
    }
    QMap<QString, QString> connected;
    int netIndex = 0;
    for (const auto &value : m_json.value("nets").toArray()) {
        const auto path = "/nets/" + QString::number(netIndex++);
        if (!value.isObject()) { issue("object-required", path, "A net must be an object."); continue; }
        const auto net = value.toObject(); const auto id = net.value("id").toString(); nonempty(net, "id", path); nonempty(net, "name", path);
        identifier(net.value("id"), path + "/id");
        if (net.contains("attributes") && !net.value("attributes").isObject()) issue("object-required", path + "/attributes", "An object is required.");
        if (netIds.contains(id)) issue("net-id-duplicate", path + "/id", "Net IDs must be unique.");
        netIds.insert(id);
        if (!net.value("endpoints").isArray()) issue("array-required", path + "/endpoints", "A net uses terminal IDs as endpoints.");
        QSet<QString> seen;
        for (const auto &v : net.value("endpoints").toArray()) {
            const auto endpoint = v.toString();
            identifier(v, path + "/endpoints");
            if (!v.isString() || !terminalIds.contains(endpoint)) issue("terminal-reference-unknown", path + "/endpoints", "Unknown terminal reference: " + endpoint);
            if (seen.contains(endpoint)) issue("endpoint-duplicate", path + "/endpoints", "An endpoint appears twice in a net.");
            seen.insert(endpoint);
            if (connected.contains(endpoint) && connected[endpoint] != id) issue("terminal-multiple-nets", path + "/endpoints", "A terminal can belong to only one net.");
            connected[endpoint] = id;
            if (gpioByTerminal.contains(endpoint) && reserved.contains(gpioByTerminal[endpoint])) issue("gpio-reserved", path + "/endpoints", "This GPIO is reserved in the selected module/profile.");
        }
        if (seen.size() < 2) issue("net-dangling", path, "This net has fewer than two endpoints.", true);
    }
    const auto layout = geometry();
    for (const auto &group : {"components", "nets"}) {
        if (!layout.value(group).isObject()) { issue("object-required", "/geometry/" + QString(group), "Geometry is stored in a separate keyed object."); continue; }
        const auto entries = layout.value(group).toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const auto path = "/geometry/" + QString(group) + "/" + it.key();
            identifier(it.key(), path);
            if (!(QString(group) == "components" ? componentIds : netIds).contains(it.key())) issue("geometry-reference-unknown", path, "Unknown geometry owner.");
            if (!it.value().isObject()) { issue("object-required", path, "Geometry must be an object."); continue; }
            if (QString(group) == "components") for (const auto &coordinate : {"x", "y"})
                if (!it.value().toObject().value(coordinate).isDouble() || !qIsFinite(it.value().toObject().value(coordinate).toDouble())) issue("geometry-coordinate-invalid", path + "/" + coordinate, "Coordinates must be finite numbers.");
            if (QString(group) == "nets" && it.value().toObject().contains("points")) {
                const auto points = it.value().toObject().value("points");
                if (!points.isArray()) issue("array-required", path + "/points", "Wire geometry points must be an array.");
                for (const auto &point : points.toArray()) for (const auto &coordinate : {"x", "y"})
                    if (!point.isObject() || !point.toObject().value(coordinate).isDouble() || !qIsFinite(point.toObject().value(coordinate).toDouble())) issue("geometry-coordinate-invalid", path + "/points", "Coordinates must be finite numbers.");
            }
        }
    }
    for (const auto &value : m_json.value("migration").toObject().value("report").toArray()) {
        const auto entry = value.toObject(); const auto code = entry.value("code").toString();
        const auto owner = entry.value("original_component_id").toString();
        bool unresolved = currentLegacyFaults.contains(code + "\n" + owner);
        if (owner.isEmpty()) for (const auto &fault : currentLegacyFaults) if (fault.startsWith(code + "\n")) unresolved = true;
        ProjectDiagnostic history{ProjectDiagnostic::Severity::Warning, code, "/migration/report", entry.value("message").toString()};
        history.historical = true; history.resolved = entry.value("severity").toString() == "error" && !unresolved;
        if (entry.value("severity").toString() == "error") history.message.prepend(history.resolved ? "Resolved migration history: " : "Migration history; current repair required: ");
        result.append(history);
    }
    if (m_json.contains("migration")) {
        const auto migration = m_json.value("migration").toObject();
        if (!m_json.value("migration").isObject() || !integer(migration.value("from_version")) || migration.value("from_version").toInt() != 2
            || !migration.value("legacy_source").isObject() || !migration.value("report").isArray()) issue("migration-record-invalid", "/migration", "Migration requires a v2 source and a diagnostic report.");
        for (const auto &value : migration.value("report").toArray()) {
            const auto record = value.toObject();
            if (!value.isObject() || !QSet<QString>{"warning", "error"}.contains(record.value("severity").toString())) issue("migration-record-invalid", "/migration/report", "Unknown migration severity.");
            nonempty(record, "code", "/migration/report"); nonempty(record, "message", "/migration/report");
        }
    }
    auto resourceCheck = [&](QJsonObject object, const QString &key, const QString &path) {
        if (!object.contains(key)) return;
        nonempty(object, key, path);
        const auto raw = object.value(key).toString();
        if (raw.contains("://")) issue("resource-uri-unsupported", path + "/" + key, "Only local file paths are supported; the URI is retained for repair.");
        if (networkPath(raw)) {
#if defined(Q_OS_WIN)
            if (raw.startsWith("\\\\") && QFileInfo(raw).isAbsolute())
                issue("resource-network-access-unverified", path + "/" + key, "Native Windows UNC path retained; network access has not been verified.", true);
            else
#endif
                issue("resource-host-mapping-required", path + "/" + key, raw.startsWith("//")
                    ? "Leading // path is ambiguous; select an explicit host/local mapping before runtime use."
                    : "Foreign Windows UNC path retained; map it to this host before runtime use.", true);
        }
        if (!QFileInfo(raw).isAbsolute() && QRegularExpression("^[A-Za-z]:[\\\\/]").match(raw).hasMatch()) issue("resource-host-mapping-required", path + "/" + key, "This other-host drive path needs explicit mapping before runtime use.", true);
    };
    resourceCheck(firmware(), "path", "/firmware");
    for (const auto &key : {"qemu_executable", "rom_path", "working_directory"}) resourceCheck(runtime(), key, "/runtime");
    for (const auto &value : m_json.value("components").toArray()) {
        const auto component = value.toObject(); resourceCheck(component.value("simulator").toObject(), "exec", "/components/" + component.value("id").toString() + "/simulator");
        const auto resources = component.value("resources").toObject();
        for (auto it = resources.begin(); it != resources.end(); ++it) resourceCheck(it.value().toObject(), "path", "/components/" + component.value("id").toString() + "/resources/" + it.key());
    }
    const auto artifacts = firmware().value("artifacts");
    if (!artifacts.isUndefined() && !artifacts.isArray()) issue("array-required", "/firmware/artifacts", "Artifacts must be an array.");
    QSet<QString> artifactIds;
    for (const auto &value : artifacts.toArray()) {
        if (!value.isObject()) { issue("object-required", "/firmware/artifacts", "An artifact must be an object."); continue; }
        const auto artifact = value.toObject();
        for (const auto &key : {"id", "kind", "path"}) nonempty(artifact, key, "/firmware/artifacts");
        resourceCheck(artifact, "path", "/firmware/artifacts");
        const auto id = artifact.value("id").toString();
        identifier(artifact.value("id"), "/firmware/artifacts/id");
        if (artifactIds.contains(id)) issue("artifact-id-duplicate", "/firmware/artifacts", "Artifact IDs must be unique.");
        artifactIds.insert(id);
        if (artifact.contains("offset") && (!integer(artifact.value("offset")) || artifact.value("offset").toDouble() < 0)) issue("artifact-offset-invalid", "/firmware/artifacts", "Flash offsets must be nonnegative integers.");
        if (artifact.contains("sha256") && (!artifact.value("sha256").isString() || !QRegularExpression("^[0-9a-fA-F]{64}$").match(artifact.value("sha256").toString()).hasMatch())) issue("artifact-hash-invalid", "/firmware/artifacts", "SHA-256 metadata must contain 64 hexadecimal characters.");
    }
    return result;
}
bool ProjectDocument::isStructurallyValid() const {
    for (const auto &diagnostic : diagnostics()) if (diagnostic.severity == ProjectDiagnostic::Severity::Error) return false;
    return true;
}
bool ProjectDocument::isDirty() const { return m_migratedUnsaved || !m_undo.isClean(); }
bool ProjectDocument::writePayload(QIODevice &device, const QByteArray &bytes, QString *error) {
    if (device.write(bytes) != bytes.size()) return fail(error, device.errorString());
    return true;
}
bool ProjectDocument::save(QString *error) { if (m_path.isEmpty()) return fail(error, "Save As is required for an unnamed document."); return saveAs(m_path, error); }
bool ProjectDocument::saveAs(const QString &path, QString *error) {
    if (path.isEmpty()) return fail(error, "A destination path is required.");
    const auto destination = QFileInfo(path).absoluteFilePath();
    QSaveFile file(destination); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    const auto bytes = QJsonDocument(serializedFor(destination)).toJson(QJsonDocument::Indented);
    if (!writePayload(file, bytes, error)) { file.cancelWriting(); return false; }
    if (!file.commit()) return fail(error, file.errorString());
    m_path = destination; m_migratedUnsaved = false; m_undo.setClean();
    emit dirtyChanged(false); emit documentChanged();
    if (error) error->clear();
    return true;
}
void ProjectDocument::replaceState(const QJsonObject &json) { m_json = json; emit documentChanged(); emit diagnosticsChanged(); }
void ProjectDocument::pushEdit(const QJsonObject &next, const QString &label) {
    const auto normalized = normalizePaths(next, baseFor(m_path), false);
    if (normalized != m_json) m_undo.push(new ProjectEditCommand(this, m_json, normalized, label));
}
QString ProjectDocument::addComponent(QJsonObject component, QString *error) {
    const auto id = component.value("id").toString().isEmpty() ? newId("component-") : component.value("id").toString();
    auto components = m_json.value("components").toArray();
    if (findId(components, id) >= 0) { fail(error, "Component ID already exists."); return {}; }
    component["id"] = id; if (!component.contains("name")) component["name"] = id;
    if (!component.contains("kind")) component["kind"] = "device";
    if (!component.contains("parameters")) component["parameters"] = QJsonObject{};
    auto terminals = component.value("terminals").toArray();
    for (int i = 0; i < terminals.size(); ++i) {
        if (!terminals[i].isObject()) continue;
        auto terminal = terminals[i].toObject();
        if (terminal.value("id").toString().isEmpty()) terminal["id"] = newId("terminal-");
        terminals[i] = terminal;
    }
    component["terminals"] = terminals; components.append(component);
    auto next = m_json; next["components"] = components; pushEdit(next, "Add component"); if (error) error->clear(); return id;
}
bool ProjectDocument::removeComponent(const QString &id, QString *error) {
    auto components = m_json.value("components").toArray(); const int index = findId(components, id);
    if (index < 0) return fail(error, "Unknown component ID.");
    QSet<QString> terminals; for (const auto &v : components[index].toObject().value("terminals").toArray()) terminals.insert(v.toObject().value("id").toString());
    components.removeAt(index); auto next = m_json; next["components"] = components;
    auto nets = next.value("nets").toArray();
    for (int n = 0; n < nets.size(); ++n) {
        auto net = nets[n].toObject(); QJsonArray endpoints;
        for (const auto &v : net.value("endpoints").toArray()) if (!terminals.contains(v.toString())) endpoints.append(v);
        net["endpoints"] = endpoints; nets[n] = net;
    }
    next["nets"] = nets;
    auto layout = next.value("geometry").toObject(); auto positions = layout.value("components").toObject(); positions.remove(id); layout["components"] = positions; next["geometry"] = layout;
    pushEdit(next, "Remove component and branches"); if (error) error->clear(); return true;
}
bool ProjectDocument::renameComponent(const QString &id, const QString &name, QString *error) {
    auto components = m_json.value("components").toArray(); const int index = findId(components, id);
    if (index < 0) return fail(error, "Unknown component ID.");
    auto component = components[index].toObject(); component["name"] = name; components[index] = component;
    auto next = m_json; next["components"] = components; pushEdit(next, "Rename component"); if (error) error->clear(); return true;
}
bool ProjectDocument::renameTerminal(const QString &id, const QString &name, QString *error) {
    auto components = m_json.value("components").toArray();
    for (int i = 0; i < components.size(); ++i) {
        auto component = components[i].toObject(); auto terminals = component.value("terminals").toArray(); const int index = findId(terminals, id);
        if (index < 0) continue;
        auto terminal = terminals[index].toObject(); terminal["name"] = name; terminals[index] = terminal; component["terminals"] = terminals; components[i] = component;
        auto next = m_json; next["components"] = components; pushEdit(next, "Rename terminal"); if (error) error->clear(); return true;
    }
    return fail(error, "Unknown terminal ID.");
}
bool ProjectDocument::renameNet(const QString &id, const QString &name, QString *error) {
    auto nets = m_json.value("nets").toArray(); const int index = findId(nets, id);
    if (index < 0) return fail(error, "Unknown net ID.");
    auto net = nets[index].toObject(); net["name"] = name; nets[index] = net;
    auto next = m_json; next["nets"] = nets; pushEdit(next, "Rename net"); if (error) error->clear(); return true;
}
bool ProjectDocument::moveComponent(const QString &id, const QPointF &position, QString *error) {
    if (findId(m_json.value("components").toArray(), id) < 0) return fail(error, "Unknown component ID.");
    if (!qIsFinite(position.x()) || !qIsFinite(position.y())) return fail(error, "Position must be finite.");
    auto next = m_json; auto layout = next.value("geometry").toObject(); auto positions = layout.value("components").toObject();
    auto location = positions.value(id).toObject(); location["x"] = position.x(); location["y"] = position.y(); positions[id] = location;
    layout["components"] = positions; next["geometry"] = layout; pushEdit(next, "Move component"); if (error) error->clear(); return true;
}
QString ProjectDocument::addNet(QJsonObject net, QString *error) {
    const auto id = net.value("id").toString().isEmpty() ? newId("net-") : net.value("id").toString();
    auto nets = m_json.value("nets").toArray();
    if (findId(nets, id) >= 0) { fail(error, "Net ID already exists."); return {}; }
    net["id"] = id; if (!net.contains("name")) net["name"] = id; if (!net.contains("endpoints")) net["endpoints"] = QJsonArray{};
    nets.append(net); auto next = m_json; next["nets"] = nets; pushEdit(next, "Add net"); if (error) error->clear(); return id;
}
bool ProjectDocument::removeNet(const QString &id, QString *error) {
    auto nets = m_json.value("nets").toArray(); const int index = findId(nets, id);
    if (index < 0) return fail(error, "Unknown net ID.");
    nets.removeAt(index);
    auto next = m_json; next["nets"] = nets;
    auto layout = next.value("geometry").toObject(); auto routes = layout.value("nets").toObject(); routes.remove(id); layout["nets"] = routes; next["geometry"] = layout;
    pushEdit(next, "Remove net"); if (error) error->clear(); return true;
}
bool ProjectDocument::connectTerminal(const QString &netId, const QString &terminalId, QString *error) {
    bool exists = false; for (const auto &component : components()) for (const auto &terminal : component.terminals) if (terminal.id == terminalId) exists = true;
    if (!exists) return fail(error, "Unknown terminal ID.");
    auto nets = m_json.value("nets").toArray(); const int index = findId(nets, netId);
    if (index < 0) return fail(error, "Unknown net ID.");
    for (const auto &v : nets) for (const auto &endpoint : v.toObject().value("endpoints").toArray())
        if (endpoint.toString() == terminalId) return fail(error, "Terminal is already connected; disconnect it first.");
    auto net = nets[index].toObject(); auto endpoints = net.value("endpoints").toArray(); endpoints.append(terminalId); net["endpoints"] = endpoints; nets[index] = net;
    auto next = m_json; next["nets"] = nets; pushEdit(next, "Connect terminal branch"); if (error) error->clear(); return true;
}
bool ProjectDocument::disconnectTerminal(const QString &netId, const QString &terminalId, QString *error) {
    auto nets = m_json.value("nets").toArray(); const int index = findId(nets, netId);
    if (index < 0) return fail(error, "Unknown net ID.");
    auto net = nets[index].toObject(); const auto existing = net.value("endpoints").toArray(); QJsonArray endpoints; bool removed = false;
    for (const auto &endpoint : existing) { if (endpoint.toString() == terminalId) removed = true; else endpoints.append(endpoint); }
    if (!removed) return fail(error, "Terminal is not connected to this net.");
    net["endpoints"] = endpoints; nets[index] = net; auto next = m_json; next["nets"] = nets; pushEdit(next, "Disconnect terminal branch"); if (error) error->clear(); return true;
}
bool ProjectDocument::setParameter(const QString &id, const QString &name, const QJsonObject &quantity, QString *error) {
    auto components = m_json.value("components").toArray(); const int index = findId(components, id);
    if (index < 0 || name.isEmpty()) return fail(error, "A known component and parameter name are required.");
    auto component = components[index].toObject(); auto parameters = component.value("parameters").toObject(); parameters[name] = quantity; component["parameters"] = parameters; components[index] = component;
    auto next = m_json; next["components"] = components; pushEdit(next, "Change component parameter"); if (error) error->clear(); return true;
}
bool ProjectDocument::setSimulator(const QString &id, const QJsonObject &simulator, QString *error) {
    auto components = m_json.value("components").toArray(); const int index = findId(components, id);
    if (index < 0) return fail(error, "Unknown component ID.");
    auto component = components[index].toObject(); component["simulator"] = simulator; components[index] = component;
    auto next = m_json; next["components"] = components; pushEdit(next, "Change device simulator"); if (error) error->clear(); return true;
}
bool ProjectDocument::setComponentAttributes(const QString &id, const QJsonObject &attributes, QString *error) {
    auto components = m_json.value("components").toArray(); const int index = findId(components, id);
    if (index < 0) return fail(error, "Unknown component ID.");
    auto component = components[index].toObject(); component["attributes"] = attributes; components[index] = component;
    auto next = m_json; next["components"] = components; pushEdit(next, "Change device attributes"); if (error) error->clear(); return true;
}
void ProjectDocument::setProfile(const QJsonObject &value) { auto next = m_json; next["profile"] = value; pushEdit(next, "Change board/module profile"); }
void ProjectDocument::setFirmware(const QJsonObject &value) { auto next = m_json; next["firmware"] = value; pushEdit(next, "Change firmware metadata"); }
void ProjectDocument::setRuntime(const QJsonObject &value) { auto next = m_json; next["runtime"] = value; pushEdit(next, "Change runtime metadata"); }

QJsonObject ProjectDocument::legacyRuntimeJson(QString *error) const {
    return legacyRuntimeJsonForDestination(m_path, error);
}
QJsonObject ProjectDocument::legacyRuntimeJsonForDestination(const QString &destinationPath, QString *error) const {
    return legacyRuntimeDocument(destinationPath, false, error);
}
QJsonObject ProjectDocument::legacyRuntimeJsonWithAbsoluteResources(QString *error) const {
    return legacyRuntimeDocument({}, true, error);
}
QJsonObject ProjectDocument::legacyRuntimeDocument(const QString &destinationPath, bool absoluteResources, QString *error) const {
    if (!isStructurallyValid()) { fail(error, "Invalid project draft cannot be applied."); return {}; }
    auto legacy = m_json.value("migration").toObject().value("legacy_source").toObject();
    if (legacy.isEmpty()) { fail(error, "This project has no explicit legacy migration source."); return {}; }
    if (!integer(legacy.value("version")) || legacy.value("version").toInt() != 2) { fail(error, "Legacy source must be version 2."); return {}; }
    if (!legacy.value("devices").isArray()) { fail(error, "Legacy source remains invalid; its device list is not representable."); return {}; }
    int mcuCount = 0;
    for (const auto &component : components()) if (component.kind == "mcu") ++mcuCount;
    if (mcuCount != 1) { fail(error, "Legacy runtime requires exactly one migrated MCU."); return {}; }
    QJsonArray devices;
    for (const auto &value : m_json.value("components").toArray()) {
        const auto component = value.toObject();
        if (component.value("kind").toString() == "mcu") continue;
        if (component.value("kind").toString() != "device" || !component.value("legacy").isObject() || !component.value("parameters").toObject().isEmpty()) {
            fail(error, "Native project support is required for new components or typed parameters."); return {};
        }
        auto device = component.value("legacy").toObject();
        if (component.value("id") != device.value("id") || component.value("type") != device.value("type")) { fail(error, "Legacy component identity changed."); return {}; }
        device["simulator"] = component.value("simulator"); devices.append(device);
    }
    legacy["devices"] = devices;
    const auto expected = migrateV2(legacy);
    for (const auto &entry : expected.value("migration").toObject().value("report").toArray()) {
        if (entry.toObject().value("severity").toString() == "error") { fail(error, "Legacy source remains invalid; native project support is required."); return {}; }
    }
    if (profile() != expected.value("profile").toObject() || firmware() != expected.value("firmware").toObject()
        || runtime() != expected.value("runtime").toObject()) {
        fail(error, "Changed board/firmware/runtime metadata is not applied by the legacy device runtime."); return {};
    }
    const auto expectedComponents = expected.value("components").toArray();
    for (const auto &value : m_json.value("components").toArray()) {
        auto current = value.toObject(); const bool mcu = current.value("kind").toString() == "mcu";
        const int index = mcu ? 0 : findId(expectedComponents, current.value("id").toString());
        if (index < 0) { fail(error, "Component semantics are not representable by the legacy runtime."); return {}; }
        auto original = expectedComponents[index].toObject();
        QJsonArray currentTerminals, originalTerminals;
        for (const auto &entry : current.value("terminals").toArray()) { auto terminal = entry.toObject(); terminal.remove("name"); if (mcu) terminal.remove("id"); currentTerminals.append(terminal); }
        for (const auto &entry : original.value("terminals").toArray()) { auto terminal = entry.toObject(); terminal.remove("name"); if (mcu) terminal.remove("id"); originalTerminals.append(terminal); }
        if (currentTerminals != originalTerminals || (mcu && (current.value("type") != original.value("type") || current.value("parameters") != original.value("parameters") || current.value("attributes") != original.value("attributes")))) {
            fail(error, "Changed terminal/chip semantics require native electrical runtime support."); return {};
        }
    }
    for (const auto &value : m_json.value("components").toArray()) {
        const auto component = value.toObject(); if (component.value("kind").toString() == "mcu") continue;
        const auto originals = expected.value("components").toArray(); const auto index = findId(originals, component.value("id").toString());
        if (index < 0) { fail(error, "Legacy component is not representable."); return {}; }
        const auto originalAttributes = originals[index].toObject().value("attributes").toObject(); const auto attributes = component.value("attributes").toObject();
        if (attributes != originalAttributes) {
            fail(error, "Changed decoder/property metadata requires explicit native project support."); return {};
        }
    }
    auto expectedNets = expected.value("nets").toArray();
    const auto expectedMcu = expected.value("components").toArray().first().toObject().value("id").toString();
    QMap<int, QString> padIds;
    for (const auto &component : components()) if (component.kind == "mcu") for (const auto &terminal : component.terminals) if (terminal.gpio) padIds[*terminal.gpio] = terminal.id;
    for (int i = 0; i < expectedNets.size(); ++i) {
        auto net = expectedNets[i].toObject(); auto endpoints = net.value("endpoints").toArray();
        for (int n = 0; n < endpoints.size(); ++n) {
            const auto endpoint = endpoints[n].toString();
            if (endpoint.startsWith(expectedMcu + ".gpio")) {
                const auto gpio = endpoint.mid(expectedMcu.size() + 5).toInt();
                if (padIds.contains(gpio)) endpoints[n] = padIds[gpio];
            }
        }
        net["endpoints"] = endpoints; expectedNets[i] = net;
    }
    if (endpointSets(expectedNets) != endpointSets(m_json.value("nets").toArray())) {
        fail(error, "Changed net topology cannot be represented by legacy controller/address metadata."); return {};
    }
    for (const auto &value : m_json.value("nets").toArray()) {
        const auto current = value.toObject(); const auto endpoints = endpointSets(QJsonArray{current});
        for (const auto &entry : expectedNets) if (endpointSets(QJsonArray{entry}) == endpoints && current.value("attributes") != entry.toObject().value("attributes")) {
            fail(error, "Changed net attributes require native electrical runtime support."); return {};
        }
    }
    legacy["ui_layout"] = geometry().value("components");
    legacy["firmware"] = m_json.value("firmware"); legacy["runtime"] = m_json.value("runtime");
    legacy["version"] = 2;
    // Caller chooses the actual output location; no guessed temporary base and
    // no firmware pin matrix change are hidden in this compatibility export.
    if (!absoluteResources) mapPaths(legacy, [&](const QString &raw, bool) {
        if (networkPath(raw)) return raw;
        if (!QFileInfo(raw).isAbsolute()) return raw;
        const auto relative = QDir(baseFor(destinationPath)).relativeFilePath(raw);
        return relative.startsWith('.') || QFileInfo(relative).isAbsolute() ? relative : "./" + relative;
    });
    if (error) error->clear();
    return legacy;
}
