#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <cstdio>

#include "QemuController.h"
#include "RuntimeContract.h"

// An independent peer drives the public QMP seam. It can delay/error replies and
// send unsolicited events; no real firmware/peripheral qualification is inferred.
class FakeQmp : public QObject {
public:
    QTcpServer server;
    QTcpSocket *peer = nullptr;
    QByteArray incoming;
    QList<QJsonObject> commands;
    bool holdCapabilities = false;
    bool holdStatus = false;
    bool holdSettings = false;
    bool automaticControl = false;
    bool running = true;
    bool wrongI2c0Type = false;
    bool spiTx = false;

    FakeQmp() {
        const bool listening = server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(listening);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, this, [this]() {
                incoming += peer->readAll();
                while (incoming.contains('\n')) {
                    const int end = incoming.indexOf('\n');
                    const auto message = QJsonDocument::fromJson(incoming.left(end)).object();
                    incoming.remove(0, end + 1);
                    commands.append(message);
                    answer(message);
                }
            });
            send({{"QMP", QJsonObject{{"version", QJsonObject{
                {"qemu", QJsonObject{{"major", 9}, {"minor", 2}, {"micro", 2}}},
                {"package", "fake-contract-peer"}}}, {"capabilities", QJsonArray{}}}}});
        });
    }
    quint16 port() const { return server.serverPort(); }
    void send(const QJsonObject &message) {
        if (peer) { peer->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n'); peer->flush(); }
    }
    void reply(int id, const QJsonValue &value = QJsonObject{}) { send({{"id", id}, {"return", value}}); }
    void error(int id, const QString &description) {
        send({{"id", id}, {"error", QJsonObject{{"class", "GenericError"}, {"desc", description}}}});
    }
    void event(const QString &name) { send({{"event", name}, {"data", QJsonObject{}}}); }
    int count(const QString &execute) const {
        int found = 0;
        for (const auto &command : commands) if (command.value("execute").toString() == execute) ++found;
        return found;
    }
    int lastId(const QString &execute) const {
        for (auto it = commands.crbegin(); it != commands.crend(); ++it)
            if (it->value("execute").toString() == execute) return it->value("id").toInt();
        return -1;
    }
    QJsonObject observedStatus() const {
        return {{"running", running}, {"singlestep", false}, {"status", running ? "running" : "paused"}};
    }
    void answer(const QJsonObject &command) {
        const QString execute = command.value("execute").toString();
        const int id = command.value("id").toInt(-1);
        const auto args = command.value("arguments").toObject();
        const QString path = args.value("path").toString();
        if (execute == "qmp_capabilities") { if (!holdCapabilities) reply(id); }
        else if (execute == "query-status") { if (!holdStatus) reply(id, observedStatus()); }
        else if (execute == "qom-get") {
            if (args.value("property") == "type") {
                if (path.contains("/i2c"))
                    reply(id, wrongI2c0Type && path.contains("/i2c0/") ? "unrelated.sensor" : "esp32s3.i2c-bridge");
                else if (spiTx) reply(id, "esp32s3.gpspi");
                else error(id, "Object not found");
            } else if (args.value("property") == "bridge-enabled" && spiTx) reply(id, true);
            else error(id, "Property not found");
        } else if (execute == "qom-list") {
            QJsonArray properties;
            if (path.contains("/i2c")) {
                properties.append(QJsonObject{{"name", "registered-addrs"}, {"type", "str"}});
                properties.append(QJsonObject{{"name", "read-response-map"}, {"type", "str"}});
                reply(id, properties);
            } else if (spiTx) {
                properties.append(QJsonObject{{"name", "bridge-dc-gpio"}, {"type", "str"}});
                reply(id, properties);
            } else error(id, "Object not found");
        } else if (execute == "qom-set") { if (!holdSettings) reply(id); }
        else if (execute == "stop" || execute == "cont") {
            if (automaticControl) { running = execute == "cont"; reply(id); }
        } else reply(id);
    }
};

static RuntimeCapability capability(const QemuController &controller, const QString &id)
{
    for (const auto &item : controller.runtimeCapabilities()) if (item.id == id) return item;
    return {};
}

class ScopedSyntheticQemu {
    QByteArray previous = qgetenv("ESP32S3_QEMU_BIN");
public:
    ScopedSyntheticQemu() { qputenv("ESP32S3_QEMU_BIN", QCoreApplication::applicationFilePath().toUtf8()); }
    ~ScopedSyntheticQemu() {
        if (previous.isNull()) qunsetenv("ESP32S3_QEMU_BIN");
        else qputenv("ESP32S3_QEMU_BIN", previous);
    }
};

class RuntimeContractTest : public QObject {
    Q_OBJECT
private slots:
    void canonicalContract() {
        QCOMPARE(capabilityMaturityKey(CapabilityMaturity::NativeTested), QString("native-tested"));
        QCOMPARE(runtimePhaseKey(RuntimePhase::WaitingForDevice), QString("waiting-for-device"));
        RuntimeStatus status{RuntimePhase::Paused, "acknowledged", "session", 9007199254740993ULL};
        QCOMPARE(runtimeStatusJson(status).value("reset_epoch").toString(), QString("9007199254740993"));
        RuntimeCapability cap{"i2c0.cached-read", "Cached reads", CapabilityMaturity::Implemented,
                              false, "runtime missing", {"property probe"}};
        const auto json = runtimeCapabilityJson(cap);
        QVERIFY(!json.value("available").toBool());
        QCOMPARE(json.value("maturity").toString(), QString("implemented"));
        QCOMPARE(json.value("evidence").toArray().size(), 1);
    }
    void handshakeRequiresMatchingAcknowledgement() {
        FakeQmp fake;
        fake.holdCapabilities = true;
        QemuController controller;
        QSignalSpy states(&controller, &QemuController::runtimeStatusChanged);
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(fake.count("qmp_capabilities"), 1);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Initializing);
        QVERIFY(!capability(controller, "qmp.control").available);
        fake.reply(987654); // An unrelated return must not enable QMP.
        QTest::qWait(30);
        QCOMPARE(fake.count("qom-list"), 0);
        fake.reply(fake.lastId("qmp_capabilities"));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        QVERIFY(capability(controller, "qmp.control").available);
        QVERIFY(capability(controller, "qemu.identity").reason.contains("fake-contract-peer"));
        QVERIFY(!states.isEmpty());
        QVERIFY(!states.last().at(0).value<RuntimeStatus>().sessionId.isEmpty());
    }
    void pauseAndContinueWaitForAcknowledgedStatus() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        controller.pauseExecution();
        QTRY_COMPARE(fake.count("stop"), 1);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        fake.running = false;
        fake.reply(fake.lastId("stop"));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        controller.continueExecution();
        QTRY_COMPARE(fake.count("cont"), 1);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        fake.running = true;
        fake.reply(fake.lastId("cont"));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
    }
    void rejectedHandshakeCannotInitializeRuntime() {
        FakeQmp fake;
        fake.holdCapabilities = true;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(fake.count("qmp_capabilities"), 1);
        fake.error(fake.lastId("qmp_capabilities"), "negotiation refused");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Error);
        QCOMPARE(fake.count("qom-list"), 0);
        QVERIFY(!capability(controller, "qmp.control").available);
    }
    void staleStatusCannotOverrideStopEvent() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        const int before = fake.count("query-status");
        fake.holdStatus = true;
        controller.startLiveUpdates(true);
        QTRY_VERIFY(fake.count("query-status") > before);
        const int staleId = fake.lastId("query-status");
        controller.startLiveUpdates(false);
        fake.event("STOP");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        fake.reply(staleId, fake.observedStatus());
        QTest::qWait(30);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        fake.event("RESUME");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
    }
    void commandErrorIsNotSuccess() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        controller.pauseExecution();
        QTRY_COMPARE(fake.count("stop"), 1);
        fake.error(fake.lastId("stop"), "control rejected by peer");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Error);
        QVERIFY(controller.runtimeStatus().message.contains("control rejected"));
        QVERIFY(!capability(controller, "qmp.control").available);
    }
    void resetChangesEpochAndRejectsOldReplies() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        const QString session = controller.runtimeStatus().sessionId;
        controller.pauseExecution();
        QTRY_COMPARE(fake.count("stop"), 1);
        const int oldStop = fake.lastId("stop");
        fake.holdStatus = true;
        const int before = fake.count("query-status");
        fake.event("RESET");
        QTRY_COMPARE(controller.runtimeStatus().resetEpoch, quint64(1));
        QTRY_VERIFY(fake.count("query-status") > before);
        QCOMPARE(controller.runtimeStatus().sessionId, session);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Initializing);
        fake.reply(oldStop);
        QTest::qWait(30);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Initializing);
        fake.running = false;
        fake.reply(fake.lastId("query-status"), fake.observedStatus());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
    }
    void exactBridgeSubtypeAndFeatureLimits() {
        FakeQmp fake;
        fake.wrongI2c0Type = true;
        fake.spiTx = true;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        QVERIFY(!capability(controller, "i2c0.cached-read").available);
        QVERIFY(capability(controller, "i2c1.cached-read").available);
        QVERIFY(capability(controller, "spi2.tx").available);
        QCOMPARE(capability(controller, "spi2.tx").maturity, CapabilityMaturity::Implemented);
        QVERIFY(capability(controller, "spi2.tx").reason.contains("MISO"));
        QVERIFY(!capability(controller, "debug.step").available);
        const int commandsBefore = fake.commands.size();
        controller.stepInstruction();
        controller.addBreakpoint("0x40000000");
        controller.clearBreakpoints();
        QTest::qWait(30);
        QCOMPARE(fake.commands.size(), commandsBefore);
    }
    void cachedSettingsMustBeAcknowledgedBeforeInitialization() {
        FakeQmp fake;
        fake.holdSettings = true;
        QemuController controller;
        controller.registerI2cBridgeAddress(0, "40");
        controller.setI2cBridgeResponseMap(0, "40.e3:60c6ba");
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(fake.count("qom-set"), 2);
        QCOMPARE(fake.count("query-status"), 0);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Initializing);
        QList<int> ids;
        for (const auto &command : fake.commands)
            if (command.value("execute") == "qom-set") ids.append(command.value("id").toInt());
        fake.reply(ids.at(0));
        QTest::qWait(30);
        QCOMPARE(fake.count("query-status"), 0);
        fake.reply(ids.at(1));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
    }
    void debuggerWaitIsObservedAndShutdownCannotBeReversedByLateStatus() {
        FakeQmp fake;
        fake.running = false;
        QemuController controller;
        controller.setGdbServerConfig(true, 1234, true);
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::WaitingForDebugger);
        fake.holdStatus = true;
        const int before = fake.count("query-status");
        controller.startLiveUpdates(true);
        QTRY_VERIFY(fake.count("query-status") > before);
        controller.startLiveUpdates(false);
        const int oldStatus = fake.lastId("query-status");
        fake.event("SHUTDOWN");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopping);
        QVERIFY(!capability(controller, "qmp.control").available);
        fake.running = true;
        fake.reply(oldStatus, fake.observedStatus());
        fake.event("RESUME");
        QTest::qWait(30);
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopping);
        controller.stopSimulation();
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopped);
    }
    void lostQmpConnectionIsAnError() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        fake.peer->disconnectFromHost();
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Error);
        QVERIFY(!capability(controller, "qmp.control").available);
    }
    void connectedPeerCannotLeaveControlPendingForever() {
        FakeQmp fake;
        QemuController controller;
        controller.attachQmpEndpointForTesting(fake.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        controller.pauseExecution();
        QTRY_COMPARE(fake.count("stop"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(controller.runtimeStatus().phase, RuntimePhase::Error, 13000);
        QVERIFY(controller.runtimeStatus().message.contains("acknowledgement deadline"));
        QVERIFY(!capability(controller, "qmp.control").available);
    }
    void missingFirmwareHasStructuredFailure() {
        QemuController controller;
        controller.loadFirmware("/path/that/does/not/exist/runtime-contract.bin");
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Error);
        QVERIFY(controller.runtimeStatus().message.contains("not found"));
        QVERIFY(!controller.runtimeStatus().sessionId.isEmpty());
        controller.stopSimulation();
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopped);
    }
    void downloadProcessStartDoesNotClaimCpuRunning() {
        ScopedSyntheticQemu environment;
        QTemporaryDir directory;
        QFile firmware(directory.filePath("process-only.elf"));
        QVERIFY(firmware.open(QIODevice::WriteOnly));
        firmware.write("synthetic process lifecycle fixture, not real firmware");
        firmware.close();
        {
            QemuController controller;
            controller.setBootMode(1);
            controller.loadFirmware(firmware.fileName());
            QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::WaitingForDevice);
            QVERIFY(!capability(controller, "qmp.control").available);
            QVERIFY(capability(controller, "qmp.control").reason.contains("disabled"));
            controller.stopSimulation();
            QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopped);
        }
    }
    void connectionExhaustionStopsUnobservableProcess() {
        ScopedSyntheticQemu environment;
        QTemporaryDir directory;
        QFile firmware(directory.filePath("process-only.elf"));
        QVERIFY(firmware.open(QIODevice::WriteOnly));
        firmware.write("synthetic process lifecycle fixture, not real firmware");
        firmware.close();
        QemuController controller;
        QSignalSpy stopped(&controller, &QemuController::qemuStopped);
        controller.loadFirmware(firmware.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(controller.runtimeStatus().phase, RuntimePhase::Error, 15000);
        QVERIFY(controller.runtimeStatus().message.contains("deadline"));
        QVERIFY(!capability(controller, "qmp.control").available);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 2000);
        controller.stopSimulation();
        QCOMPARE(controller.runtimeStatus().phase, RuntimePhase::Stopped);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("esp32s3,help")) {
        std::puts("boot-mode=<string>");
        return 0;
    }
    if (app.arguments().contains("-serial")) return app.exec(); // Process-only helper, intentionally no QMP.
    RuntimeContractTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "RuntimeContractTest.moc"
