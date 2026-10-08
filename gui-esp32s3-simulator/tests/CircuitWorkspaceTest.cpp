#include <QtTest/QtTest>
#include <QComboBox>
#include <QTableWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QProcess>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include "ThemeManager.h"
#include <algorithm>
#include "BoardWorkspace.h"
#include "ProjectDocument.h"
#include "PeripheralManager.h"

namespace {
bool allRunning(PeripheralManager &manager) {
    const auto devices = manager.devicesSnapshot();
    for (const auto &value : devices) if (value.toObject().value("status").toString() != "running") return false;
    return !devices.isEmpty();
}
QList<qint64> pids(PeripheralManager &manager) {
    QList<qint64> result;
    for (auto *process : manager.findChildren<QProcess *>()) if (process->state() == QProcess::Running) result.append(process->processId());
    std::sort(result.begin(), result.end()); return result;
}
QComboBox *gpioFor(BoardWorkspace &workspace, const QString &signal) {
    auto *table = workspace.findChild<QTableWidget *>("connectionTable");
    for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 0)->text() == signal) return qobject_cast<QComboBox *>(table->cellWidget(row, 1));
    return nullptr;
}
QGraphicsItem *cardFor(QGraphicsView *view, const QString &id) {
    for (auto *item : view->scene()->items()) if (item->data(Qt::UserRole + 3).toString() == id) return item;
    return nullptr;
}
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
}

class CircuitWorkspaceTest : public QObject {
    Q_OBJECT
private slots:
    void analogQuantityConversionRepairAndNativeApplyBoundary() {
        BoardWorkspace workspace; workspace.resize(1400, 1000); workspace.show();
        QString error;
        QVERIFY(workspace.openCircuit(QStringLiteral(SIMULATOR_SOURCE_DIR) + "/peripherals/project.analog-divider-button.example.json", &error));
        auto *model = workspace.projectModel(); QVERIFY(model->isStructurallyValid());
        auto *selected = workspace.findChild<QComboBox *>("selectedComponent");
        selected->setCurrentIndex(selected->findData("upper"));
        auto q = QJsonObject{{"value", 10}, {"unit", "kohm"}, {"vendor", QJsonObject{{"preserve", 7}}}};
        QVERIFY(model->setParameter("upper", "resistance", q)); QCoreApplication::processEvents();
        auto *units = workspace.findChild<QComboBox *>("quantityUnit_resistance"); QVERIFY(units);
        units->setCurrentText("ohm"); QCoreApplication::processEvents();
        auto resistance = [&]() {
            for (const auto &entry : model->toJson().value("components").toArray())
                if (entry.toObject().value("id") == "upper") return entry.toObject().value("parameters").toObject().value("resistance").toObject();
            return QJsonObject{};
        };
        QCOMPARE(resistance().value("value").toDouble(), 10000.0);
        QCOMPARE(resistance().value("vendor"), q.value("vendor"));
        auto *value = workspace.findChild<QLineEdit *>("quantityValue_resistance"); QVERIFY(value);
        value->setText("not a resistance"); QTest::keyClick(value, Qt::Key_Return); QCoreApplication::processEvents();
        QVERIFY(!model->isStructurallyValid()); QCOMPARE(resistance().value("value").toString(), QString("not a resistance"));
        QSignalSpy applies(&workspace, &BoardWorkspace::nativeApplyRequested);
        RuntimeStatus stopped; stopped.phase = RuntimePhase::Stopped; workspace.setRuntimeStatus(stopped);
        QVERIFY(!workspace.requestNativeApply(&error)); QCOMPARE(applies.count(), 0);
        workspace.setNativeCircuitAvailable(true);
        RuntimeStatus paused; paused.phase = RuntimePhase::Paused; workspace.setRuntimeStatus(paused);
        QVERIFY(!workspace.requestNativeApply(&error)); QVERIFY(!error.isEmpty()); QCOMPARE(applies.count(), 0);
        QTemporaryDir output; QVERIFY(workspace.saveCircuit(output.filePath("invalid.json")));
        QCOMPARE(applies.count(), 0);
        QVERIFY(workspace.openCircuit(output.filePath("invalid.json"), &error)); QVERIFY(!model->isStructurallyValid());
        selected->setCurrentIndex(selected->findData("upper"));
        value = workspace.findChild<QLineEdit *>("quantityValue_resistance"); QVERIFY(value);
        value->setText("10000"); QTest::keyClick(value, Qt::Key_Return); QCoreApplication::processEvents();
        QVERIFY(model->isStructurallyValid()); QCOMPARE(resistance().value("vendor"), q.value("vendor"));
        RuntimeStatus running; running.phase = RuntimePhase::Running; workspace.setRuntimeStatus(running);
        QVERIFY(!workspace.requestNativeApply(&error)); QCOMPARE(applies.count(), 0);
        workspace.setRuntimeStatus(stopped); QVERIFY(!workspace.requestNativeApply(&error));
        workspace.setRuntimeStatus(paused); workspace.setNativeCircuitAvailable(true); QVERIFY(workspace.requestNativeApply(&error));
        QCOMPARE(applies.count(), 1); QCOMPARE(applies[0][0].toJsonObject(), workspace.circuitDocument());
        QVERIFY(!workspace.requestNativeApply(&error)); QCOMPARE(applies.count(), 1); QVERIFY(error.contains("flight"));
        workspace.setNativeApplyInFlight(false, "Backend accepted circuit.");
        QVERIFY(workspace.saveCircuit(output.filePath("repaired.json"))); QCOMPARE(applies.count(), 1);
        QVERIFY(workspace.openCircuit(output.filePath("repaired.json"), &error)); QCOMPARE(applies.count(), 1);
        QVERIFY(!workspace.applyCircuit(&error)); // analog never falls through the address exporter
        const auto accepted = workspace.circuitDocument();
        const QJsonObject snapshot{{"abi", 1}, {"generation", 7}, {"timestamp_ns", 1234},
            {"status", "resolved"}, {"nets", QJsonArray{QJsonObject{{"id", "divider"}, {"valid", false}, {"floating", true}, {"voltage_v", QJsonValue::Null}}}}};
        workspace.setNativeCircuitSnapshot(snapshot, accepted);
        auto *nets = workspace.findChild<QComboBox *>("netSelector"); nets->setCurrentIndex(nets->findData("divider"));
        auto *info = workspace.findChild<QLabel *>("netInfo"); QVERIFY(info->text().contains("floating")); QVERIFY(!info->text().contains("0 V"));
        QVERIFY(model->setParameter("upper", "resistance", QJsonObject{{"value", 20000}, {"unit", "ohm"}})); QCoreApplication::processEvents();
        workspace.setNativeCircuitSnapshot(snapshot, accepted); QVERIFY(info->text().contains("stale"));
        model->undoStack()->undo(); QCoreApplication::processEvents();
        QVERIFY(model->moveComponent("upper", QPointF(325, 48))); QCoreApplication::processEvents();
        workspace.setNativeCircuitSnapshot(snapshot, accepted); QVERIFY(info->text().contains("floating")); QVERIFY(!info->text().contains("stale"));
        workspace.setNativeCircuitAvailable(false); QVERIFY(info->text().contains("unavailable"));
    }

    void catalogAndKeyboardNetWorkflowPreserveEndpointIdentities() {
        BoardWorkspace workspace; workspace.resize(1400, 1000); workspace.show(); QCoreApplication::processEvents();
        auto *catalogue = workspace.findChild<QComboBox *>("componentCatalogue");
        auto *add = workspace.findChild<QPushButton *>("addComponent");
        for (int index : {6, 7, 8, 9, 10, 11, 12, 13, 14}) { catalogue->setCurrentIndex(index); add->click(); }
        auto *model = workspace.projectModel(); QVERIFY(model->isStructurallyValid()); QCOMPARE(model->components().size(), 9);
        QSet<QString> ids; QString a, b, button;
        for (const auto &component : model->components()) {
            for (const auto &terminal : component.terminals) { QVERIFY(!ids.contains(terminal.id)); ids.insert(terminal.id); }
            if (component.kind == "mcu") {
                bool vdd = false, gnd = false;
                for (const auto &terminal : component.terminals) {
                    vdd |= terminal.role == "vdd" && terminal.domain == "power";
                    gnd |= terminal.role == "gnd" && terminal.domain == "ground";
                }
                QVERIFY(vdd); QVERIFY(gnd);
            }
            if (component.kind == "resistor") { a = component.terminals[0].id; b = component.terminals[1].id; }
            if (component.type == "button") button = component.terminals[0].id;
        }
        auto *tabs = workspace.findChild<QTabWidget *>("inspectorTabs"); tabs->setCurrentIndex(1);
        workspace.findChild<QPushButton *>("createNet")->click(); QCoreApplication::processEvents();
        auto *nets = workspace.findChild<QComboBox *>("netSelector"); const auto netId = nets->currentData().toString();
        auto *terminals = workspace.findChild<QComboBox *>("terminalSelector");
        auto *connect = workspace.findChild<QPushButton *>("connectTerminal");
        for (const auto &id : {a, b, button}) { terminals->setCurrentIndex(terminals->findData(id)); connect->click(); QCoreApplication::processEvents(); }
        auto endpoints = [&]() { for (const auto &net : model->nets()) if (net.id == netId) return net.endpoints; return QStringList{}; };
        QCOMPARE(endpoints().size(), 3);
        auto *name = workspace.findChild<QLineEdit *>("terminalName"); terminals->setCurrentIndex(terminals->findData(button));
        name->setText("Renamed button terminal"); QTest::keyClick(name, Qt::Key_Return); QCoreApplication::processEvents();
        QVERIFY(endpoints().contains(button)); QCOMPARE(terminals->currentData().toString(), button);
        auto *members = workspace.findChild<QListWidget *>("netMembers"); members->setFocus();
        QTest::keyClick(members, Qt::Key_Delete); QCoreApplication::processEvents();
        QCOMPARE(endpoints().size(), 2); QVERIFY(endpoints().contains(a)); QVERIFY(endpoints().contains(b)); QVERIFY(!endpoints().contains(button));
        model->undoStack()->undo(); QCoreApplication::processEvents(); QCOMPARE(endpoints().size(), 3); QVERIFY(endpoints().contains(button));
        QTemporaryDir output; QVERIFY(workspace.saveCircuit(output.filePath("branches.json")));
        const auto before = workspace.circuitDocument();
        QString error; QVERIFY(workspace.openCircuit(output.filePath("branches.json"), &error));
        QCOMPARE(workspace.circuitDocument(), before); QCOMPARE(nets->currentData().toString(), netId);
        workspace.findChild<QPushButton *>("removeNet")->click(); QCoreApplication::processEvents(); QVERIFY(endpoints().isEmpty());
        model->undoStack()->undo(); QCoreApplication::processEvents(); QCOMPARE(endpoints().size(), 3);
    }

    void pointerWiringBranchesDisconnectAndUndo() {
        BoardWorkspace workspace; workspace.resize(1400, 1000); workspace.show();
        QString error; QVERIFY(workspace.openCircuit(QStringLiteral(SIMULATOR_SOURCE_DIR) + "/peripherals/project.analog-divider-button.example.json", &error));
        QCoreApplication::processEvents();
        auto *model = workspace.projectModel();
        QVERIFY(model->disconnectTerminal("divider", "mcu.adc1")); QCoreApplication::processEvents();
        const int undoBefore = model->undoStack()->index();
        auto *view = workspace.findChild<QGraphicsView *>("circuitCanvas");
        auto clickTerminal = [&](const QString &id, Qt::KeyboardModifiers modifiers = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton) {
            for (auto *item : view->scene()->items()) if (item->data(Qt::UserRole + 4).toString() == id) {
                const QPoint point = view->mapFromScene(item->sceneBoundingRect().center());
                QTest::mouseClick(view->viewport(), button, modifiers, point); QCoreApplication::processEvents(); return true;
            }
            return false;
        };
        workspace.findChild<QPushButton *>("wireTerminals")->click();
        const auto graphBefore = model->toJson().value("nets");
        QVERIFY(clickTerminal("upper.b", Qt::NoModifier, Qt::RightButton));
        QVERIFY(clickTerminal("mcu.adc1", Qt::NoModifier, Qt::MiddleButton));
        QVERIFY(clickTerminal("upper.b", Qt::AltModifier, Qt::RightButton));
        QCOMPARE(model->toJson().value("nets"), graphBefore); QCOMPARE(model->undoStack()->index(), undoBefore);
        QVERIFY(clickTerminal("upper.b")); QVERIFY(clickTerminal("mcu.adc1"));
        QCOMPARE(model->undoStack()->index(), undoBefore + 1);
        auto divider = [&]() { for (const auto &net : model->nets()) if (net.id == "divider") return net.endpoints; return QStringList{}; };
        QVERIFY(divider().contains("mcu.adc1")); QVERIFY(divider().contains("lower.a"));
        const auto transform = view->transform();
        ThemeManager::instance()->setMode(ThemeManager::Mode::Dark); QCoreApplication::processEvents();
        QCOMPARE(view->transform(), transform);
        QVERIFY(clickTerminal("mcu.adc1", Qt::AltModifier)); QVERIFY(!divider().contains("mcu.adc1")); QVERIFY(divider().contains("lower.a"));
        model->undoStack()->undo(); QCoreApplication::processEvents(); QVERIFY(divider().contains("mcu.adc1"));
        QTemporaryDir output; QVERIFY(workspace.saveCircuit(output.filePath("pointer.json")));
        const auto before = workspace.circuitDocument(); QVERIFY(workspace.openCircuit(output.filePath("pointer.json"), &error));
        QCOMPARE(workspace.circuitDocument(), before);
    }

    void editsSharedNetSavesPureV3AndRetainsRuntimeAndCatalogue() {
        const QString root = QStringLiteral(SIMULATOR_SOURCE_DIR);
        PeripheralManager manager; manager.setWorkspaceRoot(root); QVERIFY(manager.loadConfig(root + "/peripherals/peripherals.example.json"));
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(manager), 5000);
        BoardWorkspace workspace; workspace.setManager(&manager);
        RuntimeStatus status; status.phase = RuntimePhase::Stopped; workspace.setRuntimeStatus(status);
        auto *gpio = gpioFor(workspace, "SDA"); QVERIFY(gpio); QCOMPARE(gpio->findData(22), -1);
        const auto before = workspace.circuitDocument(); const auto processes = pids(manager); const auto runtimePath = manager.configPath();
        QSignalSpy restarts(&manager, &PeripheralManager::deviceSetChanged);
        gpio->setCurrentIndex(gpio->findData(7));
        QCOMPARE(workspace.projectModel()->undoStack()->index(), 1);
        const auto document = workspace.circuitDocument(); QCOMPARE(document.value("version").toInt(), 3);
        QVERIFY(!document.contains("devices")); QVERIFY(!document.contains("buses")); QVERIFY(!document.contains("board"));
        bool shared = false;
        for (const auto &net : workspace.projectModel()->nets()) if (net.endpoints.contains("screen0.sda")) {
            QVERIFY(net.endpoints.contains("temp0.sda")); QVERIFY(net.endpoints.contains("mcu.gpio7")); QVERIFY(!net.endpoints.contains("mcu.gpio8")); shared = true;
        }
        QVERIFY(shared);
        for (const auto &component : workspace.projectModel()->components()) if (component.attributes.value("decoder").toObject().value("controller") == "i2c0")
            QCOMPARE(component.attributes.value("decoder").toObject().value("pins").toObject().value("sda").toInt(), 7);
        const auto source = document.value("migration").toObject().value("legacy_source").toObject();
        QCOMPARE(source.value("buses").toArray()[0].toObject().value("pins").toObject().value("sda").toInt(), 8);
        QTemporaryDir output; const auto savedPath = output.filePath("circuit.json"); QVERIFY(workspace.saveCircuit(savedPath));
        QCOMPARE(QJsonDocument::fromJson(read(savedPath)).object(), workspace.circuitDocument());
        QString error; QVERIFY(!workspace.applyCircuit(&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(manager.configPath(), runtimePath); QCOMPARE(pids(manager), processes); QCOMPARE(restarts.count(), 0);
        workspace.projectModel()->undoStack()->undo();
        QVERIFY(workspace.applyCircuit(&error)); QCOMPARE(pids(manager), processes); QCOMPARE(restarts.count(), 0);
        QCOMPARE(workspace.circuitDocument().value("nets"), before.value("nets"));
        QFile bad(output.filePath("bad.json")); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{invalid"); bad.close();
        const auto currentDocument = workspace.circuitDocument(); QVERIFY(!workspace.openCircuit(bad.fileName(), &error));
        QCOMPARE(workspace.circuitDocument(), currentDocument); QCOMPARE(manager.configPath(), runtimePath);
        auto *catalogue = workspace.findChild<QComboBox *>("componentCatalogue"); catalogue->setCurrentIndex(1);
        workspace.findChild<QPushButton *>("addComponent")->click();
        QCOMPARE(workspace.projectModel()->components().size(), 7);
        const auto added = workspace.projectModel()->components().last(); QCOMPARE(added.attributes.value("decoder").toObject().value("address").toString(), QString("0x41"));
        QVERIFY(!workspace.applyCircuit(&error)); QCOMPARE(pids(manager), processes); QCOMPARE(restarts.count(), 0);
    }

    void dragKeyboardUndoAndSaveDoNotRestartDevices() {
        const QString root = QStringLiteral(SIMULATOR_SOURCE_DIR);
        PeripheralManager manager; manager.setWorkspaceRoot(root); QVERIFY(manager.loadConfig(root + "/peripherals/peripherals.example.json")); QTRY_VERIFY_WITH_TIMEOUT(allRunning(manager), 5000);
        BoardWorkspace workspace; workspace.setManager(&manager); workspace.resize(1400, 1000); workspace.show(); workspace.activateWindow(); QCoreApplication::processEvents();
        RuntimeStatus stopped; stopped.phase = RuntimePhase::Stopped; workspace.setRuntimeStatus(stopped);
        QTemporaryDir output; QVERIFY(workspace.saveCircuit(output.filePath("layout.json")));
        const auto processes = pids(manager); QSignalSpy restarts(&manager, &PeripheralManager::deviceSetChanged);
        auto *view = workspace.findChild<QGraphicsView *>("circuitCanvas"); auto *card = cardFor(view, "screen0"); QVERIFY(card);
        const auto initial = workspace.circuitDocument(); const auto press = view->mapFromScene(card->pos() + QPointF(80, 25));
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, press);
        QTest::mouseMove(view->viewport(), press + QPoint(40, 30), 30);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, press + QPoint(40, 30));
        QCoreApplication::processEvents(); QCOMPARE(workspace.projectModel()->undoStack()->index(), 1);
        QVERIFY(workspace.projectModel()->isDirty()); QVERIFY(workspace.circuitDocument().value("geometry") != initial.value("geometry"));
        view->setFocus(); QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier); QCoreApplication::processEvents();
        QCOMPARE(workspace.circuitDocument(), initial); QVERIFY(!workspace.projectModel()->isDirty());
        QTest::keyClick(view, Qt::Key_Y, Qt::ControlModifier); QCoreApplication::processEvents(); QVERIFY(workspace.projectModel()->isDirty());
        QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier); QCoreApplication::processEvents(); QVERIFY(!workspace.projectModel()->isDirty());
        QString error; QVERIFY(workspace.applyCircuit(&error)); QCOMPARE(pids(manager), processes); QCOMPARE(restarts.count(), 0);
        QCOMPARE(QJsonDocument::fromJson(read(output.filePath("layout.json"))).object(), workspace.circuitDocument());
    }

    void openInvalidAndEmptyProjectsWithoutReplacingActiveRuntime() {
        const QString root = QStringLiteral(SIMULATOR_SOURCE_DIR);
        PeripheralManager manager; manager.setWorkspaceRoot(root); QVERIFY(manager.loadConfig(root + "/peripherals/peripherals.example.json")); QTRY_VERIFY_WITH_TIMEOUT(allRunning(manager), 5000);
        BoardWorkspace workspace; workspace.setManager(&manager); const auto processes = pids(manager); const auto runtimePath = manager.configPath();
        RuntimeStatus running; running.phase = RuntimePhase::Running; workspace.setRuntimeStatus(running); QString error;
        QVERIFY(!workspace.applyCircuit(&error)); QVERIFY(error.contains("Stop"));
        ProjectDocument invalid; auto json = invalid.toJson(); json["components"] = "retained invalid data";
        QTemporaryDir output; QFile file(output.filePath("invalid.json")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(json).toJson()); file.close();
        QVERIFY(workspace.openCircuit(file.fileName(), &error)); QVERIFY(!workspace.projectModel()->isStructurallyValid());
        QVERIFY(workspace.saveCircuit(output.filePath("invalid-copy.json"))); QCOMPARE(QJsonDocument::fromJson(read(output.filePath("invalid-copy.json"))).object(), json);
        QCOMPARE(pids(manager), processes); QCOMPARE(manager.configPath(), runtimePath);
        workspace.newCircuit(); QVERIFY(workspace.projectModel()->components().isEmpty());
        auto *catalogue = workspace.findChild<QComboBox *>("componentCatalogue"); catalogue->setCurrentIndex(6); workspace.findChild<QPushButton *>("addComponent")->click();
        QCOMPARE(workspace.projectModel()->components().size(), 1); QCoreApplication::processEvents(); workspace.findChild<QPushButton *>("removeComponent")->click(); QCoreApplication::processEvents();
        QVERIFY(workspace.projectModel()->components().isEmpty()); QVERIFY(workspace.saveCircuit(output.filePath("empty.json")));
        QCOMPARE(pids(manager), processes); QCOMPARE(manager.configPath(), runtimePath);
        QVERIFY(workspace.findChild<QLabel *>("applyStatus")->text().contains("unavailable"));
    }

    void managerPollingDuringDragEditSaveDoesNotDisturbDocumentOrDevices() {
        const QString root = QStringLiteral(SIMULATOR_SOURCE_DIR);
        PeripheralManager manager; manager.setWorkspaceRoot(root); QVERIFY(manager.loadConfig(root + "/peripherals/peripherals.example.json"));
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(manager), 5000);
        BoardWorkspace workspace; workspace.setManager(&manager); workspace.resize(1400, 1000); workspace.show(); workspace.activateWindow(); QCoreApplication::processEvents();
        RuntimeStatus stopped; stopped.phase = RuntimePhase::Stopped; workspace.setRuntimeStatus(stopped);
        QTemporaryDir output; QVERIFY(workspace.saveCircuit(output.filePath("baseline.json")));
        const auto processes = pids(manager); const auto runtimePath = manager.configPath();
        QSignalSpy restarts(&manager, &PeripheralManager::deviceSetChanged);
        QSignalSpy polls(&manager, &PeripheralManager::devicesChanged);
        // Confirm status polling is actually firing while the storm below runs.
        QTRY_VERIFY_WITH_TIMEOUT(polls.count() > 0, 5000);
        const int pollsBefore = polls.count();
        const auto selection = workspace.findChild<QComboBox *>("selectedComponent"); QVERIFY(selection);
        QString sensorId;
        for (const auto &component : workspace.projectModel()->components())
            if (component.kind == "device" && component.type == "sht21" && component.name == "temp0") sensorId = component.id;
        QVERIFY(!sensorId.isEmpty());
        selection->setCurrentIndex(selection->findData(sensorId));
        // Drag screen0 while the manager polls device state (the drag itself
        // legitimately selects the dragged card), then select temp0 and hold it.
        auto *view = workspace.findChild<QGraphicsView *>("circuitCanvas"); auto *card = cardFor(view, "screen0"); QVERIFY(card);
        const auto press = view->mapFromScene(card->pos() + QPointF(80, 25));
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, press);
        QTest::mouseMove(view->viewport(), press + QPoint(40, 30), 30);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, press + QPoint(40, 30));
        QCoreApplication::processEvents();
        selection->setCurrentIndex(selection->findData(sensorId));
        const auto selectedBefore = selection->currentData().toString();
        const auto before = workspace.circuitDocument();
        int expectedCommands = workspace.projectModel()->undoStack()->count();
        QCOMPARE(expectedCommands, 1); // one drag is exactly one command

        // Save mid-poll; then rename while polling continues.
        QVERIFY(workspace.saveCircuit(output.filePath("mid.json")));
        QVERIFY(workspace.projectModel()->renameComponent(sensorId, "Renamed sensor"));
        ++expectedCommands;
        QCOMPARE(workspace.projectModel()->undoStack()->count(), expectedCommands);

        // Let several poll cycles pass over the edited, saved document.
        QTest::qWait(1200); QCoreApplication::processEvents();
        QVERIFY(polls.count() > pollsBefore);
        QCOMPARE(workspace.projectModel()->undoStack()->count(), expectedCommands); // no stray undo commands
        QCOMPARE(workspace.projectModel()->undoStack()->index(), expectedCommands);
        QCOMPARE(selection->currentData().toString(), selectedBefore); // selection kept its stable ID
        QCOMPARE(pids(manager), processes); QCOMPARE(manager.configPath(), runtimePath);
        QCOMPARE(restarts.count(), 0);
        QCOMPARE(workspace.circuitDocument().value("nets"), before.value("nets")); // net endpoints unchanged
        QVERIFY(workspace.saveCircuit(output.filePath("final.json")));
        QCOMPARE(QJsonDocument::fromJson(read(output.filePath("final.json"))).object(), workspace.circuitDocument());
    }
};
QTEST_MAIN(CircuitWorkspaceTest)
#include "CircuitWorkspaceTest.moc"
