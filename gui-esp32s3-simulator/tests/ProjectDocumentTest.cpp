#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QRegularExpression>
#include "ProjectDocument.h"
#include "PeripheralTransport.h"

namespace {
QJsonObject quantity(const QJsonValue &value, const QString &unit) { return {{"value", value}, {"unit", unit}}; }
QJsonObject terminal(const QString &id, const QString &role, int gpio = -1) {
    QJsonObject result{{"id", id}, {"name", role}, {"role", role}, {"domain", "passive"}, {"direction", "passive"}};
    if (gpio >= 0) { result["gpio"] = gpio; result["role"] = "gpio"; result["domain"] = "digital"; result["direction"] = "inout"; }
    return result;
}
QJsonObject component(const QString &id, const QString &kind, const QStringList &roles, QJsonObject params = {}) {
    QJsonArray terminals; for (const auto &role : roles) terminals.append(terminal(id + "." + role, role));
    return {{"id", id}, {"name", id}, {"kind", kind}, {"type", kind}, {"terminals", terminals}, {"parameters", params}};
}
QJsonObject resistor(const QString &id = "r") { return component(id, "resistor", {"a", "b"}, {{"resistance", quantity(1000, "ohm")}}); }
bool hasCode(const ProjectDocument &document, const QString &code) {
    for (const auto &diagnostic : document.diagnostics()) if (diagnostic.code == code) return true;
    return false;
}
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool write(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QString resolve(const QString &documentPath, const QString &resource) { return QDir::cleanPath(QDir(QFileInfo(documentPath).absolutePath()).absoluteFilePath(resource)); }
QJsonObject legacy() {
    const QJsonObject bus{{"id", "i2c-main"}, {"kind", "i2c"}, {"controller", "i2c0"},
        {"clock_hz", 400000}, {"pins", QJsonObject{{"sda", 8}, {"scl", 9}}},
        {"electrical", QJsonObject{{"pullup_ohms", 4700}, {"voltage", 3.3}}}};
    QJsonArray devices;
    for (const auto &id : {"sensor0", "sensor1"}) devices.append(QJsonObject{{"id", id}, {"type", "sht21"}, {"bus_ref", "i2c-main"},
        {"simulator", QJsonObject{{"exec", "./device.py"}, {"args", QJsonArray{"--opaque", "./untouched-arg"}},
            {"protocol", "jsonrpc-i2c-v1"}, {"vendor", QJsonObject{{"flag", 7}}}}},
        {"properties", QJsonObject{{"initial_celsius", 27.5}, {"csv_example", "./samples.csv"}}},
        {"vendor_extra", QJsonObject{{"look_like_path", "./opaque-text"}}}});
    return {{"version", 2}, {"board", QJsonObject{{"name", "esp32s3-devkitc-1"}, {"target", "esp32s3"}, {"custom", true}}},
        {"buses", QJsonArray{bus}}, {"devices", devices}, {"ui_layout", QJsonObject{{"sensor0", QJsonObject{{"x", 40}, {"y", 60}}}}},
        {"vendor_root", QJsonArray{1, "opaque", QJsonObject{{"preserve", true}}}}};
}
class FailingDocument : public ProjectDocument {
public:
    bool failWrites = false;
protected:
    bool writePayload(QIODevice &device, const QByteArray &bytes, QString *error) override {
        if (!failWrites) return ProjectDocument::writePayload(device, bytes, error);
        device.write(bytes.left(bytes.size() / 2));
        if (error) *error = "Injected partial-write failure";
        return false;
    }
};
}

class ProjectDocumentTest : public QObject {
    Q_OBJECT
private slots:
    void displayUnitConversionKeepsPhysicalQuantityAndExtensions() {
        const QJsonObject extension{{"origin", "user"}, {"opaque", QJsonArray{1, 2}}};
        for (const auto &pair : {qMakePair(QString("V"), QString("mV")), qMakePair(QString("kohm"), QString("ohm")),
                                 qMakePair(QString("uF"), QString("nF")), qMakePair(QString("%"), QString("ratio"))}) {
            auto before = quantity(2, pair.first); before["vendor"] = extension;
            const auto converted = ProjectDocument::quantityForUnit(before, pair.second);
            QCOMPARE(converted.value("vendor"), QJsonValue(extension));
            QCOMPARE(converted.value("unit").toString(), pair.second);
            const auto roundTrip = ProjectDocument::quantityForUnit(converted, pair.first);
            QVERIFY(qAbs(roundTrip.value("value").toDouble() - 2) < 1e-12);
        }
        auto invalid = quantity("repair me", "ohm"); invalid["vendor"] = extension;
        const auto retained = ProjectDocument::quantityForUnit(invalid, "kohm");
        QCOMPARE(retained.value("value"), invalid.value("value")); QCOMPARE(retained.value("vendor"), invalid.value("vendor"));
        const auto dimensionMismatch = ProjectDocument::quantityForUnit(quantity(3.3, "V"), "ohm");
        QCOMPARE(dimensionMismatch.value("value").toDouble(), 3.3);
        QCOMPARE(dimensionMismatch.value("unit").toString(), QString("ohm"));
    }
    void migratesWithoutInventingElectricalComponents() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QVERIFY(QDir(dir.path()).mkdir("peripherals"));
        QVERIFY(write(dir.filePath("device.py"), "# simulator")); QVERIFY(write(dir.filePath("samples.csv"), "time,value"));
        const auto sourcePath = dir.filePath("peripherals/legacy.json");
        const auto source = legacy();
        ProjectDocument document; QVERIFY(document.loadJson(source, sourcePath));
        QVERIFY(document.isStructurallyValid()); QVERIFY(document.isDirty());
        QCOMPARE(document.toJson().value("version").toInt(), 3);
        QCOMPARE(document.components().size(), 3);
        QCOMPARE(document.components().first().terminals.size(), 45);
        const auto migratedComponents = document.components();
        for (const auto &pad : migratedComponents.first().terminals) QVERIFY(!pad.gpio || *pad.gpio < 22 || *pad.gpio > 25);
        QCOMPARE(document.nets().size(), 2);
        QCOMPARE(document.nets().first().endpoints.size(), 3); // branch, not one wire per device
        for (const auto &part : document.components()) QVERIFY(part.kind == "mcu" || part.kind == "device");
        QVERIFY(hasCode(document, "legacy-power-unmodeled")); QVERIFY(hasCode(document, "legacy-direction-unspecified"));
        const auto retained = document.toJson().value("migration").toObject().value("legacy_source").toObject();
        QCOMPARE(retained.value("vendor_root"), source.value("vendor_root"));
        QCOMPARE(retained.value("buses"), source.value("buses"));
        QCOMPARE(retained.value("board"), source.value("board"));
        const auto device = retained.value("devices").toArray().first().toObject();
        QCOMPARE(device.value("vendor_extra"), source.value("devices").toArray().first().toObject().value("vendor_extra"));
        QCOMPARE(device.value("simulator").toObject().value("args"), QJsonValue(QJsonArray{"--opaque", "./untouched-arg"}));
        QCOMPARE(resolve(sourcePath, device.value("simulator").toObject().value("exec").toString()), dir.filePath("device.py"));
        QCOMPARE(resolve(sourcePath, device.value("properties").toObject().value("csv_example").toString()), dir.filePath("samples.csv"));
        QString error;
        const auto exported = document.legacyRuntimeJson(&error); QVERIFY2(!exported.isEmpty(), qPrintable(error));
        QCOMPARE(exported.value("version").toInt(), 2); QCOMPARE(exported.value("buses"), source.value("buses"));
        const auto destination = dir.filePath("session/device-preview.json");
        const auto relocated = document.legacyRuntimeJsonForDestination(destination, &error); QVERIFY2(!relocated.isEmpty(), qPrintable(error));
        QCOMPARE(resolve(destination, relocated.value("devices").toArray().first().toObject().value("simulator").toObject().value("exec").toString()), dir.filePath("device.py"));
        const auto absolute = document.legacyRuntimeJsonWithAbsoluteResources(&error); QVERIFY(!absolute.isEmpty());
        QCOMPARE(absolute.value("devices").toArray().first().toObject().value("simulator").toObject().value("exec").toString(), dir.filePath("device.py"));
        QVERIFY(document.disconnectTerminal("gpio-8", "sensor0.sda")); QVERIFY(document.isStructurallyValid());
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("topology"));
        document.undoStack()->undo(); QVERIFY(!document.legacyRuntimeJson(&error).isEmpty());
        QVERIFY(document.setParameter("sensor0", "offset", quantity(2, "degC")));
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("typed parameters"));
        document.undoStack()->undo();
        QVERIFY(document.saveAs(dir.filePath("migrated.json")));
        ProjectDocument reloaded; QVERIFY(reloaded.loadFile(document.filePath())); QCOMPARE(reloaded.toJson(), document.toJson());
        QCOMPARE(reloaded.nets().first().id, document.nets().first().id);
        QCOMPARE(reloaded.components()[1].terminals.first().id, document.components()[1].terminals.first().id);
    }

    void undoRedoBranchesDeletionAndCleanState() {
        ProjectDocument document;
        QCOMPARE(document.addComponent(resistor()), QString("r"));
        QCOMPARE(document.addComponent(component("d1", "device", {"io"})), QString("d1"));
        QCOMPARE(document.addComponent(component("d2", "device", {"io"})), QString("d2"));
        QCOMPARE(document.addNet({{"id", "branch"}}), QString("branch"));
        QVERIFY(document.connectTerminal("branch", "r.a")); QVERIFY(document.connectTerminal("branch", "d1.io")); QVERIFY(document.connectTerminal("branch", "d2.io"));
        QCOMPARE(document.addNet({{"id", "other"}}), QString("other"));
        const int commands = document.undoStack()->count();
        QVERIFY(!document.connectTerminal("other", "d1.io")); QCOMPARE(document.undoStack()->count(), commands);
        QTemporaryDir dir; QVERIFY(document.saveAs(dir.filePath("circuit.json"))); QVERIFY(!document.isDirty());
        const auto original = document.toJson();
        QVERIFY(document.renameTerminal("r.a", "Left terminal"));
        QVERIFY(document.renameNet("branch", "Shared signal"));
        QCOMPARE(document.components().first().terminals.first().id, QString("r.a"));
        QCOMPARE(document.components().first().terminals.first().role, QString("a"));
        QCOMPARE(document.nets().first().id, QString("branch"));
        document.undoStack()->undo(); document.undoStack()->undo(); QCOMPARE(document.toJson(), original);
        QVERIFY(document.renameComponent("r", "Volume control"));
        const int beforeDrag = document.undoStack()->count();
        QVERIFY(document.moveComponent("r", {11, 27})); QCOMPARE(document.undoStack()->count(), beforeDrag + 1);
        QCOMPARE(document.components().first().id, QString("r")); QCOMPARE(document.components().first().terminals.first().id, QString("r.a"));
        QCOMPARE(document.nets().first().id, QString("branch")); QCOMPARE(document.nets().first().endpoints.size(), 3);
        QVERIFY(document.setParameter("r", "resistance", quantity(2, "kohm")));
        QCOMPARE(*document.components().first().parameters["resistance"].siValue, 2000.0);
        const auto beforeRemove = document.toJson();
        QVERIFY(document.removeComponent("r")); QCOMPARE(document.nets().first().id, QString("branch")); QCOMPARE(document.nets().first().endpoints.size(), 2);
        QVERIFY(!document.geometry().value("components").toObject().contains("r"));
        document.undoStack()->undo(); QCOMPARE(document.toJson(), beforeRemove);
        document.undoStack()->redo(); QCOMPARE(document.components().size(), 2);
        document.undoStack()->undo(); document.undoStack()->undo(); document.undoStack()->undo(); document.undoStack()->undo();
        QCOMPARE(document.toJson(), original); QVERIFY(!document.isDirty());
        QVERIFY(document.removeNet("branch")); document.undoStack()->undo(); QCOMPARE(document.toJson(), original);
        QVERIFY(document.disconnectTerminal("branch", "d1.io")); document.undoStack()->undo(); QCOMPARE(document.toJson(), original);
        auto generatedPart = component("", "device", {"io"}); generatedPart.remove("name");
        auto generatedTerminals = generatedPart.value("terminals").toArray(); auto generatedTerminal = generatedTerminals.first().toObject(); generatedTerminal.remove("id"); generatedTerminals[0] = generatedTerminal; generatedPart["terminals"] = generatedTerminals;
        const auto generated = document.addComponent(generatedPart); QVERIFY(!generated.isEmpty());
        const auto terminalId = document.components().last().terminals.first().id; QVERIFY(!terminalId.isEmpty());
        document.undoStack()->undo(); document.undoStack()->redo(); QCOMPARE(document.components().last().id, generated); QCOMPARE(document.components().last().terminals.first().id, terminalId);
    }

    void saveAsRebasesResourcesAndUndoKeepsTheirReferents() {
        QTemporaryDir dir; QVERIFY(QDir(dir.path()).mkdir("old")); QVERIFY(QDir(dir.path()).mkdir("new"));
        const auto oldPath = dir.filePath("old/project.json"), newPath = dir.filePath("new/project.json");
        QVERIFY(write(dir.filePath("old/device.py"), "# fixture")); QVERIFY(write(dir.filePath("old/fw.bin"), "firmware"));
        ProjectDocument document; auto json = document.toJson(); auto device = component("sensor", "device", {"io"});
        device["simulator"] = QJsonObject{{"exec", "./device.py"}, {"args", QJsonArray{"./keep-argument"}}};
        json["components"] = QJsonArray{device};
        json["firmware"] = QJsonObject{{"artifacts", QJsonArray{QJsonObject{{"id", "flash"}, {"kind", "flash"}, {"path", "./fw.bin"}}}}};
        QVERIFY(document.loadJson(json, oldPath)); QVERIFY(document.save()); const auto oldBytes = read(oldPath);
        QVERIFY(document.moveComponent("sensor", {19, 22})); QVERIFY(document.renameComponent("sensor", "My sensor"));
        QVERIFY(document.saveAs(newPath)); QVERIFY(!document.isDirty()); QCOMPARE(read(oldPath), oldBytes);
        auto saved = QJsonDocument::fromJson(read(newPath)).object();
        QCOMPARE(resolve(newPath, saved.value("components").toArray().first().toObject().value("simulator").toObject().value("exec").toString()), dir.filePath("old/device.py"));
        QCOMPARE(resolve(newPath, saved.value("firmware").toObject().value("artifacts").toArray().first().toObject().value("path").toString()), dir.filePath("old/fw.bin"));
        document.undoStack()->undo(); QVERIFY(document.isDirty());
        QCOMPARE(document.components().first().simulator.value("exec").toString(), dir.filePath("old/device.py"));
        document.undoStack()->redo(); QVERIFY(!document.isDirty());
        ProjectDocument loaded; QVERIFY(loaded.loadFile(newPath)); QCOMPARE(loaded.toJson(), document.toJson());
        QCOMPARE(loaded.components().first().id, QString("sensor"));
        QCOMPARE(loaded.components().first().simulator.value("args"), QJsonValue(QJsonArray{"./keep-argument"}));
    }

    void partialWriteAndSaveAsFailurePreserveFilesAndDirtyState() {
        QTemporaryDir dir; FailingDocument document; document.addComponent(resistor());
        const auto originalPath = dir.filePath("original.json"), destination = dir.filePath("destination.json");
        QVERIFY(document.saveAs(originalPath)); const auto originalBytes = read(originalPath);
        QVERIFY(write(destination, "existing-destination"));
        QVERIFY(document.renameComponent("r", "Changed")); QVERIFY(document.isDirty());
        const auto before = document.toJson(); const int index = document.undoStack()->index();
        document.failWrites = true; QString error;
        QVERIFY(!document.save(&error)); QVERIFY(error.contains("partial-write")); QCOMPARE(read(originalPath), originalBytes);
        QVERIFY(!document.saveAs(destination, &error)); QCOMPARE(read(destination), QByteArray("existing-destination"));
        QCOMPARE(document.filePath(), originalPath); QCOMPARE(document.toJson(), before); QCOMPARE(document.undoStack()->index(), index); QVERIFY(document.isDirty());
        document.failWrites = false; QVERIFY(!document.saveAs(dir.filePath("missing/child.json"), &error));
        QCOMPARE(read(originalPath), originalBytes); QCOMPARE(document.filePath(), originalPath); QVERIFY(document.isDirty());
        QVERIFY(document.save()); QVERIFY(!document.isDirty());
    }

    void invalidDraftIsRetainedSavedAndRepairableThroughUndo() {
        QTemporaryDir dir; ProjectDocument document; document.addComponent(resistor()); QVERIFY(document.saveAs(dir.filePath("draft.json")));
        const auto valid = document.toJson();
        QVERIFY(document.setParameter("r", "resistance", quantity(0, "ohm"))); QVERIFY(!document.isStructurallyValid()); QVERIFY(hasCode(document, "value-nonpositive"));
        QVERIFY(document.save()); ProjectDocument reloaded; QVERIFY(reloaded.loadFile(document.filePath())); QVERIFY(!reloaded.isStructurallyValid());
        document.undoStack()->undo(); QVERIFY(document.isStructurallyValid()); QVERIFY(document.isDirty()); QCOMPARE(document.toJson(), valid);
        auto malformed = valid; malformed["components"] = "keep-invalid-array"; malformed["firmware"] = QJsonArray{1, "retain"};
        QVERIFY(document.loadJson(malformed, dir.filePath("invalid.json"))); QVERIFY(!document.isStructurallyValid()); QCOMPARE(document.toJson(), malformed);
        QVERIFY(document.save()); QCOMPARE(QJsonDocument::fromJson(read(document.filePath())).object(), malformed);
        QString error; const auto before = document.toJson();
        QVERIFY(!document.loadJson({{"version", 4}}, {}, &error)); QCOMPARE(document.toJson(), before);
        QVERIFY(write(dir.filePath("parse-error.json"), "{broken")); QVERIFY(!document.loadFile(dir.filePath("parse-error.json"), &error)); QCOMPARE(document.toJson(), before);
        auto resourceDraft = valid;
        resourceDraft["firmware"] = QJsonObject{{"artifacts", QJsonArray{QJsonObject{{"id", "flash"}, {"kind", "flash"}, {"path", "https://example.invalid/firmware.bin"}, {"sha256", "invalid-hash"}}}}};
        QVERIFY(document.loadJson(resourceDraft, dir.filePath("resource-draft.json")));
        QVERIFY(hasCode(document, "resource-uri-unsupported")); QVERIFY(hasCode(document, "artifact-hash-invalid")); QVERIFY(!document.isStructurallyValid());
        QCOMPARE(document.toJson(), resourceDraft); // no URL rewritten into a local path
    }

    void rejectsBadReferencesUnitsPadsAndProfileReservations() {
        ProjectDocument document; auto json = document.toJson(); auto mcu = component("mcu", "mcu", {});
        mcu["terminals"] = QJsonArray{terminal("mcu.gpio8", "gpio", 8), terminal("mcu.gpio22", "gpio", 22)};
        auto r = resistor(); auto terminals = r.value("terminals").toArray(); auto t = terminals.first().toObject(); t["id"] = "mcu.gpio8"; terminals[0] = t; r["terminals"] = terminals;
        r["parameters"] = QJsonObject{{"resistance", quantity(-1, "unknown-unit")}};
        json["components"] = QJsonArray{mcu, r};
        json["nets"] = QJsonArray{QJsonObject{{"id", "n1"}, {"name", "one"}, {"endpoints", QJsonArray{"mcu.gpio8", "mcu.gpio8", "missing.pin"}}},
            QJsonObject{{"id", "n2"}, {"name", "two"}, {"endpoints", QJsonArray{"mcu.gpio8", "r.b"}}}};
        QVERIFY(document.loadJson(json)); QVERIFY(!document.isStructurallyValid());
        for (const auto &code : {"gpio-nonexistent", "terminal-id-duplicate", "endpoint-duplicate", "terminal-reference-unknown", "terminal-multiple-nets", "unit-unknown"}) QVERIFY2(hasCode(document, code), code);
        json = ProjectDocument().toJson(); mcu["terminals"] = QJsonArray{terminal("mcu.gpio35", "gpio", 35)};
        json["components"] = QJsonArray{mcu, component("sensor", "device", {"io"})};
        json["nets"] = QJsonArray{QJsonObject{{"id", "signal"}, {"name", "Signal"}, {"endpoints", QJsonArray{"mcu.gpio35", "sensor.io"}}}};
        QVERIFY(document.loadJson(json)); QVERIFY(document.isStructurallyValid());
        auto profile = document.profile(); profile["module"] = "ESP32-S3-WROOM-1-N8R8"; document.setProfile(profile);
        QVERIFY(!document.isStructurallyValid()); QVERIFY(hasCode(document, "gpio-reserved")); document.undoStack()->undo(); QVERIFY(document.isStructurallyValid());
        profile = document.profile(); profile["reserved_gpios"] = QJsonArray{QJsonObject{{"gpio", 35}, {"reason", "Custom module connection"}}}; document.setProfile(profile);
        QVERIFY(hasCode(document, "gpio-reserved"));
    }

    void analogQuantitiesHaveExplicitRolesAndDimensions() {
        ProjectDocument document;
        document.addComponent(component("supply", "voltage-source", {"p", "n"}, {{"voltage", quantity(3300, "mV")}}));
        document.addComponent(component("current", "current-source", {"p", "n"}, {{"current", quantity(-2, "mA")}}));
        document.addComponent(component("cap", "capacitor", {"p", "n"}, {{"capacitance", quantity(10, "uF")}, {"initial_voltage", quantity(0, "V")}}));
        document.addComponent(component("pot", "potentiometer", {"a", "w", "b"}, {{"resistance", quantity(10, "kohm")}, {"position", quantity(50, "%")}}));
        document.addComponent(component("switch", "switch", {"a", "b"}, {{"state", quantity("closed", "state")}, {"on_resistance", quantity(1, "ohm")}}));
        document.addComponent(component("ground", "ground", {"ref"}));
        QVERIFY(document.isStructurallyValid());
        QVERIFY(qAbs(*document.components()[0].parameters["voltage"].siValue - 3.3) < 1e-12);
        QCOMPARE(*document.components()[1].parameters["current"].siValue, -.002);
        QVERIFY(qAbs(*document.components()[2].parameters["capacitance"].siValue - .00001) < 1e-12);
        QCOMPARE(*document.components()[3].parameters["position"].siValue, .5);
        QVERIFY(!document.components()[4].parameters["state"].siValue);
        QVERIFY(document.setParameter("pot", "position", quantity(101, "%"))); QVERIFY(hasCode(document, "position-out-of-range")); document.undoStack()->undo();
        QVERIFY(document.setParameter("cap", "initial_voltage", quantity(3, "ohm"))); QVERIFY(hasCode(document, "parameter-dimension-invalid")); document.undoStack()->undo();
        QVERIFY(document.setParameter("cap", "capacitance", quantity(-1, "uF"))); QVERIFY(hasCode(document, "value-nonpositive")); document.undoStack()->undo(); QVERIFY(document.isStructurallyValid());
    }

    void legacyAdapterDoesNotIgnoreChangedMetadataOrInvalidSource() {
        ProjectDocument document; QVERIFY(document.loadJson(legacy()));
        auto modified = document.toJson(); auto components = modified.value("components").toArray();
        auto sensor = components[1].toObject(); auto attributes = sensor.value("attributes").toObject();
        auto decoder = attributes.value("decoder").toObject(); decoder["controller"] = "i2c1"; attributes["decoder"] = decoder; sensor["attributes"] = attributes; components[1] = sensor; modified["components"] = components;
        QVERIFY(document.loadJson(modified)); QVERIFY(document.isStructurallyValid()); QString error;
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("decoder"));
        auto badSource = legacy(); auto devices = badSource.value("devices").toArray(); auto oldSensor = devices[0].toObject();
        oldSensor["bus"] = QJsonObject{{"kind", "i2c"}, {"controller", "i2c0"}, {"pins", QJsonObject{{"sda", 22}, {"scl", 9}}}}; devices[0] = oldSensor; badSource["devices"] = devices;
        QVERIFY(document.loadJson(badSource)); QVERIFY(!document.isStructurallyValid()); QVERIFY(hasCode(document, "legacy-gpio-invalid"));
        auto manuallyAcknowledged = document.toJson(); auto migration = manuallyAcknowledged.value("migration").toObject(); migration["report"] = QJsonArray{}; manuallyAcknowledged["migration"] = migration;
        QVERIFY(document.loadJson(manuallyAcknowledged)); QVERIFY(!document.isStructurallyValid());
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("Invalid project draft"));
        badSource = legacy(); badSource.remove("ui_layout"); badSource["devices"] = "retain-malformed-value";
        QVERIFY(document.loadJson(badSource)); QVERIFY(document.isStructurallyValid()); QVERIFY(hasCode(document, "legacy-devices-invalid"));
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("source remains invalid"));
        QCOMPARE(document.toJson().value("migration").toObject().value("legacy_source").toObject().value("devices"), QJsonValue("retain-malformed-value"));
    }

    void bundledLegacyExamplesPreserveEverySourceField() {
        const QStringList examples{QFINDTESTDATA("../peripherals/peripherals.example.json"),
                                   QFINDTESTDATA("../peripherals/peripherals.ssd1331_sht21.example.json")};
        for (const auto &path : examples) {
            QVERIFY(!path.isEmpty()); auto original = QJsonDocument::fromJson(read(path)).object();
            ProjectDocument document; QVERIFY(document.loadFile(path)); QVERIFY(document.isStructurallyValid());
            auto retained = document.toJson().value("migration").toObject().value("legacy_source").toObject();
            auto oldDevices = original.value("devices").toArray(), newDevices = retained.value("devices").toArray();
            QCOMPARE(newDevices.size(), oldDevices.size());
            for (int i = 0; i < oldDevices.size(); ++i) {
                auto oldDevice = oldDevices[i].toObject(), newDevice = newDevices[i].toObject();
                auto oldSim = oldDevice.value("simulator").toObject(), newSim = newDevice.value("simulator").toObject();
                auto originalExec = resolve(path, oldSim.value("exec").toString());
                if (!QFileInfo::exists(originalExec)) originalExec = QDir::cleanPath(QDir(QFileInfo(path).absolutePath() + "/..").absoluteFilePath(oldSim.value("exec").toString()));
                QCOMPARE(resolve(path, newSim.value("exec").toString()), originalExec);
                oldSim["exec"] = "<same-resource>"; newSim["exec"] = "<same-resource>"; oldDevice["simulator"] = oldSim; newDevice["simulator"] = newSim;
                auto oldProps = oldDevice.value("properties").toObject(), newProps = newDevice.value("properties").toObject();
                for (const auto &key : {"csv_example", "csv_path"}) if (oldProps.value(key).isString()) {
                    auto originalCsv = resolve(path, oldProps.value(key).toString());
                    if (!QFileInfo::exists(originalCsv)) originalCsv = QDir::cleanPath(QDir(QFileInfo(path).absolutePath() + "/..").absoluteFilePath(oldProps.value(key).toString()));
                    QCOMPARE(resolve(path, newProps.value(key).toString()), originalCsv);
                    oldProps[key] = "<same-resource>"; newProps[key] = "<same-resource>";
                }
                if (oldDevice.contains("properties")) oldDevice["properties"] = oldProps;
                if (newDevice.contains("properties")) newDevice["properties"] = newProps;
                oldDevices[i] = oldDevice; newDevices[i] = newDevice;
            }
            original["devices"] = oldDevices; retained["devices"] = newDevices; QCOMPARE(retained, original);
        }
    }

    void generatedOpaqueIdsRoundTripThroughRealTransportCodec() {
        ProjectDocument document; auto part = component("", "device", {"sda", "scl"});
        part.remove("id"); part["name"] = QString::fromUtf8("温湿度传感器");
        auto terminals = part.value("terminals").toArray();
        for (int i = 0; i < terminals.size(); ++i) { auto t = terminals[i].toObject(); t.remove("id"); t["name"] = QString::fromUtf8("信号") + QString::number(i); terminals[i] = t; }
        part["terminals"] = terminals;
        const auto componentId = document.addComponent(part), netId = document.addNet();
        QVERIFY(ProjectDocument::isValidIdentifier(document.toJson().value("id").toString()));
        QVERIFY(ProjectDocument::isValidIdentifier(componentId)); QVERIFY(ProjectDocument::isValidIdentifier(netId));
        QJsonArray endpointIds;
        const auto generatedComponents = document.components();
        for (const auto &terminal : generatedComponents.first().terminals) {
            QVERIFY(ProjectDocument::isValidIdentifier(terminal.id)); QVERIFY(terminal.id.startsWith("terminal-")); QVERIFY(terminal.id.size() <= 64);
            QVERIFY(document.connectTerminal(netId, terminal.id)); endpointIds.append(terminal.id);
        }
        QVERIFY(document.isStructurallyValid());
        QCOMPARE(document.components().first().name, QString::fromUtf8("温湿度传感器"));
        BusEnvelope request; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sessionId = "11111111-2222-3333-4444-555555555555"; request.connectionId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        request.sequence = 1; request.requestId = "1";
        request.data = {{"bus", "gpio"}, {"controller_id", "gpio0"}, {"endpoint_ids", endpointIds}, {"net_ids", QJsonArray{netId}},
            {"phases", QJsonArray{QJsonObject{{"kind", "sample"}}}}, {"bit_length", "0"}, {"bit_order", "msb-first"},
            {"mode", QJsonValue(QJsonValue::Null)}, {"chip_select", QJsonValue(QJsonValue::Null)}, {"read_length", 0},
            {"virtual_deadline_ns", "1"}, {"payload_encoding", "base64"}, {"payload", ""}};
        QString error; const auto frame = PeripheralFrameCodec::encode(request, error); QVERIFY2(!frame.isEmpty(), qPrintable(error));
        BusEnvelope decoded; QVERIFY2(PeripheralFrameCodec::decode(frame.mid(4), decoded, error), qPrintable(error));
        QCOMPARE(decoded.data.value("endpoint_ids"), QJsonValue(endpointIds)); QCOMPARE(decoded.data.value("net_ids"), QJsonValue(QJsonArray{netId}));
        // The codec treats IDs as opaque: checking component IDs too does not
        // assert a terminal/component namespace or infer ownership by prefix.
        request.data["endpoint_ids"] = QJsonArray{componentId}; QVERIFY(!PeripheralFrameCodec::encode(request, error).isEmpty());
        const auto boundary = QString(64, 'a'); QVERIFY(ProjectDocument::isValidIdentifier(boundary));
        request.data["endpoint_ids"] = QJsonArray{boundary}; QVERIFY(!PeripheralFrameCodec::encode(request, error).isEmpty());
        for (const auto &bad : QStringList{QString(65, 'a'), "with space", "trailing\n", "_first", QString::fromUtf8("端点"), ""}) {
            QVERIFY(!ProjectDocument::isValidIdentifier(bad)); request.data["endpoint_ids"] = QJsonArray{bad};
            QVERIFY2(PeripheralFrameCodec::encode(request, error).isEmpty(), qPrintable(bad));
        }
        const auto snapshot = document.toJson(); document.undoStack()->undo(); document.undoStack()->redo(); QCOMPARE(document.toJson(), snapshot);
    }

    void unsupportedIdsRemainEditableAndLegacySourceIsPreserved() {
        ProjectDocument document; auto draft = document.toJson(); auto part = component(QString::fromUtf8("元件"), "device", {"io"});
        auto terminals = part.value("terminals").toArray(); auto t = terminals.first().toObject(); t["id"] = QString(65, 't'); terminals[0] = t; part["terminals"] = terminals;
        draft["components"] = QJsonArray{part};
        draft["nets"] = QJsonArray{QJsonObject{{"id", ".invalid"}, {"name", QString::fromUtf8("共享信号")}, {"endpoints", QJsonArray{QString(65, 't'), "unknown-valid-id"}}}};
        QVERIFY(document.loadJson(draft)); QVERIFY(!document.isStructurallyValid()); QVERIFY(hasCode(document, "identifier-invalid")); QVERIFY(hasCode(document, "terminal-reference-unknown"));
        QCOMPARE(document.toJson(), draft);
        auto old = legacy(); auto devices = old.value("devices").toArray(); auto sensor = devices[0].toObject(); sensor["id"] = QString::fromUtf8("旧传感器"); devices[0] = sensor; old["devices"] = devices;
        QVERIFY(document.loadJson(old)); QVERIFY(!document.isStructurallyValid()); QVERIFY(hasCode(document, "identifier-invalid"));
        QCOMPARE(document.components()[1].id, QString::fromUtf8("旧传感器"));
        QCOMPARE(document.toJson().value("migration").toObject().value("legacy_source").toObject().value("devices").toArray()[0].toObject().value("id"), sensor.value("id"));
        QString error; QVERIFY(document.legacyRuntimeJson(&error).isEmpty());
    }

    void repairedOrRemovedLegacyFaultsBecomeHistoryAndUndoRestoresValidity() {
        auto old = legacy(); auto devices = old.value("devices").toArray(); auto sensor = devices[0].toObject();
        sensor["bus"] = QJsonObject{{"kind", "i2c"}, {"controller", "i2c0"}, {"pins", QJsonObject{{"sda", 22}, {"scl", 9}}}};
        auto simulator = sensor.value("simulator").toObject(); simulator.remove("exec"); sensor["simulator"] = simulator; devices[0] = sensor; old["devices"] = devices;
        ProjectDocument document; QVERIFY(document.loadJson(old)); QVERIFY(!document.isStructurallyValid());
        const auto provenance = document.toJson().value("migration");
        QVERIFY(document.removeComponent("sensor0")); QVERIFY(document.isStructurallyValid()); QCOMPARE(document.toJson().value("migration"), provenance);
        bool resolvedNote = false;
        for (const auto &diagnostic : document.diagnostics()) if (diagnostic.code == "legacy-gpio-invalid" && diagnostic.historical) {
            QVERIFY(diagnostic.resolved); QCOMPARE(diagnostic.severity, ProjectDiagnostic::Severity::Warning); resolvedNote = true;
        }
        QVERIFY(resolvedNote); QString error; QVERIFY2(!document.legacyRuntimeJson(&error).isEmpty(), qPrintable(error));
        document.undoStack()->undo(); QVERIFY(!document.isStructurallyValid());
        simulator["exec"] = "python3"; QVERIFY(document.setSimulator("sensor0", simulator)); QVERIFY(!document.isStructurallyValid());
        auto attributes = document.components()[1].attributes; auto decoder = attributes.value("decoder").toObject();
        auto pins = decoder.value("pins").toObject(); pins["sda"] = 8; decoder["pins"] = pins; attributes["decoder"] = decoder;
        QVERIFY(document.setComponentAttributes("sensor0", attributes)); QVERIFY(document.isStructurallyValid());
        QVERIFY(document.connectTerminal("gpio-8", "sensor0.sda")); QCOMPARE(document.toJson().value("migration"), provenance);
        QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); // original raw decoder is not silently rewritten
        document.undoStack()->undo(); document.undoStack()->undo(); QVERIFY(!document.isStructurallyValid());
        document.undoStack()->redo(); QVERIFY(document.isStructurallyValid()); QCOMPARE(document.toJson().value("migration"), provenance);
    }

    void networkPathIdentitySurvivesLoadSaveAsUndoAndLegacyMigration() {
        QTemporaryDir dir; QVERIFY(QDir(dir.path()).mkdir("old")); QVERIFY(QDir(dir.path()).mkdir("new"));
        const QStringList paths{QStringLiteral("\\\\server\\share\\firmware.bin"), QStringLiteral("//server/share/firmware.bin")};
        for (int index = 0; index < paths.size(); ++index) {
            const auto remote = paths[index];
            const auto oldPath = dir.filePath("old/project-" + QString::number(index) + ".json");
            const auto newPath = dir.filePath("new/project-" + QString::number(index) + ".json");
            ProjectDocument document; auto json = document.toJson(); auto device = component("sensor", "device", {"io"});
            device["simulator"] = QJsonObject{{"exec", remote}};
            device["resources"] = QJsonObject{{"samples", QJsonObject{{"path", remote}}}};
            json["components"] = QJsonArray{device};
            json["firmware"] = QJsonObject{{"path", remote}, {"artifacts", QJsonArray{QJsonObject{{"id", "flash"}, {"kind", "flash"}, {"path", remote}}}}};
            json["runtime"] = QJsonObject{{"qemu_executable", remote}, {"rom_path", remote}, {"working_directory", remote}};
            QVERIFY(document.loadJson(json, oldPath)); QCOMPARE(document.toJson(), json);
#if !defined(Q_OS_WIN)
            QVERIFY(hasCode(document, "resource-host-mapping-required"));
#else
            QVERIFY(hasCode(document, index == 0 ? "resource-network-access-unverified" : "resource-host-mapping-required"));
#endif
            QVERIFY(document.save()); const auto oldBytes = read(oldPath);
            QVERIFY(document.renameComponent("sensor", "Changed name")); QVERIFY(document.saveAs(newPath));
            QCOMPARE(read(oldPath), oldBytes);
            const auto saved = QJsonDocument::fromJson(read(newPath)).object();
            QCOMPARE(saved.value("firmware"), json.value("firmware")); QCOMPARE(saved.value("runtime"), json.value("runtime"));
            QCOMPARE(saved.value("components").toArray()[0].toObject().value("simulator"), device.value("simulator"));
            QCOMPARE(saved.value("components").toArray()[0].toObject().value("resources"), device.value("resources"));
            document.undoStack()->undo(); QCOMPARE(document.toJson(), json);
            ProjectDocument reloaded; QVERIFY(reloaded.loadFile(newPath));
            QCOMPARE(reloaded.firmware().value("path").toString(), remote);
            QCOMPARE(reloaded.components().first().simulator.value("exec").toString(), remote);
            auto v2 = legacy(); auto devices = v2.value("devices").toArray(); auto oldDevice = devices[0].toObject();
            auto simulator = oldDevice.value("simulator").toObject(); simulator["exec"] = remote; oldDevice["simulator"] = simulator;
            auto properties = oldDevice.value("properties").toObject(); properties["csv_example"] = remote; oldDevice["properties"] = properties;
            devices[0] = oldDevice; v2["devices"] = devices;
            QVERIFY(document.loadJson(v2, oldPath));
            const auto retained = document.toJson().value("migration").toObject().value("legacy_source").toObject().value("devices").toArray()[0].toObject();
            QCOMPARE(retained.value("simulator"), oldDevice.value("simulator")); QCOMPARE(retained.value("properties"), oldDevice.value("properties"));
            QVERIFY(document.renameComponent("sensor0", "Renamed legacy sensor")); QVERIFY(document.saveAs(newPath));
            document.undoStack()->undo(); QCOMPARE(document.components()[1].simulator.value("exec").toString(), remote);
            QString error; const auto exported = document.legacyRuntimeJson(&error); QVERIFY2(!exported.isEmpty(), qPrintable(error));
            QCOMPARE(exported.value("devices").toArray()[0].toObject().value("simulator").toObject().value("exec").toString(), remote);
            // Only local project files are opened. No test queries/opens/runs
            // any network resource; normalization short-circuits UNC first.
        }
    }

    void legacyExportRejectsUnsupportedMetadataInsteadOfDroppingIt() {
        ProjectDocument document; QVERIFY(document.loadJson(legacy())); QString error;
        auto changed = document.toJson(); auto components = changed.value("components").toArray(); auto sensor = components[1].toObject();
        auto terminals = sensor.value("terminals").toArray(); auto terminal = terminals[0].toObject(); terminal["domain"] = "analog"; terminals[0] = terminal; sensor["terminals"] = terminals; components[1] = sensor; changed["components"] = components;
        QVERIFY(document.loadJson(changed)); QVERIFY(document.isStructurallyValid()); QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("semantics"));
        QVERIFY(document.loadJson(legacy())); auto profile = document.profile(); profile["reservations_known"] = true; document.setProfile(profile);
        QVERIFY(document.isStructurallyValid()); QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("metadata"));
        document.undoStack()->undo(); document.setFirmware({{"artifacts", QJsonArray{QJsonObject{{"id", "app"}, {"kind", "app"}, {"path", "./app.bin"}}}}});
        QVERIFY(document.isStructurallyValid()); QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("metadata"));
        document.undoStack()->undo(); QVERIFY(!document.legacyRuntimeJson(&error).isEmpty());
        changed = document.toJson(); auto nets = changed.value("nets").toArray(); auto net = nets[0].toObject(); auto attributes = net.value("attributes").toObject(); attributes["drive_voltage"] = 3.3; net["attributes"] = attributes; nets[0] = net; changed["nets"] = nets;
        QVERIFY(document.loadJson(changed)); QVERIFY(document.isStructurallyValid()); QVERIFY(document.legacyRuntimeJson(&error).isEmpty()); QVERIFY(error.contains("attributes"));
    }

    void importedV2SaveAsRelocateReopenYieldsPureV3WithStableReferents() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("origin"));
        const auto base = dir.filePath("origin");
        QVERIFY(QDir(base).mkdir("peripherals")); QVERIFY(QDir(base).mkdir("session"));
        QVERIFY(write(base + "/" + "peripherals/device.py", "# simulator"));
        QVERIFY(write(base + "/" + "peripherals/samples.csv", "time,value"));
        auto source = legacy();
        auto devices = source.value("devices").toArray();
        auto foreignDevice = devices[1].toObject();
        foreignDevice["simulator"] = QJsonObject{{"exec", "C:\\tools\\legacy_probe.py"}, {"protocol", "jsonrpc-i2c-v1"}};
        devices[1] = foreignDevice; source["devices"] = devices;
        const auto sourcePath = base + "/" + "peripherals/legacy.json";

        ProjectDocument document;
        QVERIFY(document.loadJson(source, sourcePath));
        QVERIFY(document.isStructurallyValid()); // foreign path is an advisory, not an invalid draft
        QVERIFY(hasCode(document, "resource-host-mapping-required"));
        QCOMPARE(document.toJson().value("version").toInt(), 3);
        const auto expectedNets = document.nets();

        // Import defaults to Save As: the file it writes is pure v3.
        const auto savedPath = base + "/" + "session/project.json";
        QVERIFY(document.saveAs(savedPath));
        QVERIFY(!document.isDirty()); QVERIFY(document.undoStack()->isClean());
        QCOMPARE(document.undoStack()->count(), 0); QCOMPARE(document.undoStack()->index(), 0);
        const auto saved = QJsonDocument::fromJson(read(savedPath)).object();
        QCOMPARE(saved.value("version").toInt(), 3);
        QVERIFY(!saved.contains("devices")); QVERIFY(!saved.contains("buses")); QVERIFY(!saved.contains("board"));
        const auto savedComponents = saved.value("components").toArray();
        QCOMPARE(savedComponents.size(), 3);
        QCOMPARE(resolve(savedPath, savedComponents[1].toObject().value("simulator").toObject().value("exec").toString()), base + "/" + "peripherals/device.py");
        QCOMPARE(savedComponents[2].toObject().value("simulator").toObject().value("exec").toString(), QStringLiteral("C:\\tools\\legacy_probe.py"));

        // Relocate the whole project tree; the referent moves with it.
        const auto moved = dir.filePath("relocated");
        QVERIFY(QDir(dir.path()).rename(QStringLiteral("origin"), QStringLiteral("relocated")));
        const auto movedPath = moved + "/" + "session/project.json";
        ProjectDocument reloaded; QVERIFY(reloaded.loadFile(movedPath));
        QVERIFY(!reloaded.isDirty()); QVERIFY(reloaded.undoStack()->isClean());
        QCOMPARE(reloaded.undoStack()->count(), 0); QCOMPARE(reloaded.undoStack()->index(), 0);
        QCOMPARE(reloaded.toJson(), saved); // reopen is pure v3 and byte-identical
        QCOMPARE(reloaded.nets().size(), expectedNets.size());
        for (int index = 0; index < expectedNets.size(); ++index) {
            QCOMPARE(reloaded.nets()[index].id, expectedNets[index].id);
            QCOMPARE(reloaded.nets()[index].endpoints, expectedNets[index].endpoints);
        }
        QCOMPARE(resolve(reloaded.filePath(), reloaded.components()[1].simulator.value("exec").toString()), moved + "/" + "peripherals/device.py");
        QVERIFY(QFileInfo::exists(moved + "/" + "peripherals/device.py"));
        QCOMPARE(reloaded.components()[2].simulator.value("exec").toString(), QStringLiteral("C:\\tools\\legacy_probe.py"));
        QVERIFY(hasCode(reloaded, "resource-host-mapping-required")); // diagnosed, never guessed into a local path

        // An edit/undo cycle after relocation keeps the same referents.
        QVERIFY(reloaded.moveComponent(reloaded.components()[2].id, {5, 6}));
        QCOMPARE(reloaded.undoStack()->count(), 1);
        reloaded.undoStack()->undo();
        QVERIFY(reloaded.undoStack()->isClean());
        QCOMPARE(reloaded.toJson(), saved);
    }
};

QTEST_GUILESS_MAIN(ProjectDocumentTest)
#include "ProjectDocumentTest.moc"
