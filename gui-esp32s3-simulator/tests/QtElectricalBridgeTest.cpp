#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>

#include "QemuController.h"

// Independent scripted QMP peer: these tests qualify asynchronous host state,
// not the electrical solver, firmware behavior or any synthetic voltage value.
class ElectricalPeer : public QObject {
public:
    QTcpServer server;
    QTcpSocket *socket = nullptr;
    QByteArray input;
    QList<QJsonObject> commands;
    bool running = false;
    bool holdStatus = false;
    bool holdDiscovery = false;
    bool exposeProject = true;
    int abi = 1;
    int snapshotQueries = 0;

    ElectricalPeer() {
        if (!server.listen(QHostAddress::LocalHost, 0))
            qFatal("Cannot start independent electrical QMP test peer");
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this]() {
                input += socket->readAll();
                while (input.contains('\n')) {
                    const auto end = input.indexOf('\n');
                    const auto command = QJsonDocument::fromJson(input.left(end)).object();
                    input.remove(0, end + 1);
                    commands.append(command);
                    answer(command);
                }
            });
            send({{"QMP", QJsonObject{{"version", QJsonObject{
                {"qemu", QJsonObject{{"major", 9}, {"minor", 2}, {"micro", 2}}},
                {"package", "adversarial-electrical-transport"}}}, {"capabilities", QJsonArray{}}}}});
        });
    }
    quint16 port() const { return server.serverPort(); }
    void send(const QJsonObject &message) {
        if (socket) {
            socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
        }
    }
    void ack(int id, const QJsonValue &value = QJsonObject{}) { send({{"id", id}, {"return", value}}); }
    void reject(int id, const QString &reason) {
        send({{"id", id}, {"error", QJsonObject{{"class", "GenericError"}, {"desc", reason}}}});
    }
    void event(const QString &name) { send({{"event", name}}); }
    QJsonObject status() const { return {{"running", running}, {"status", running ? "running" : "paused"}}; }
    static QJsonObject snapshot(const QString &generation = "0", bool configured = false) {
        return {{"abi", 1}, {"support", QJsonObject{{"dc", true}, {"rc", true}, {"adc", true},
                    {"profile", "s3-explicit-finite-v1"}}},
                {"generation", generation}, {"timestamp_ns", "9007199254740993"},
                {"status", configured ? "ok" : "unconfigured"}, {"diagnostic", "scripted transport fixture"},
                {"nets", configured ? QJsonArray{QJsonObject{{"id", "node"}, {"voltage_v", QJsonValue::Null},
                         {"valid", false}, {"floating", true}}} : QJsonArray{}}};
    }
    void snapshotAck(int id, const QJsonObject &value) {
        ack(id, QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    }
    int lastId(const QString &execute, const QString &property = {}) const {
        for (auto it = commands.crbegin(); it != commands.crend(); ++it)
            if (it->value("execute") == execute && (property.isEmpty()
                || it->value("arguments").toObject().value("property") == property))
                return it->value("id").toInt();
        return -1;
    }
    int count(const QString &execute, const QString &property = {}) const {
        int result = 0;
        for (const auto &command : commands)
            if (command.value("execute") == execute && (property.isEmpty()
                || command.value("arguments").toObject().value("property") == property)) ++result;
        return result;
    }
    void answer(const QJsonObject &command) {
        const auto execute = command.value("execute").toString();
        const auto args = command.value("arguments").toObject();
        const auto id = command.value("id").toInt();
        if (execute == "qmp_capabilities") ack(id);
        else if (execute == "query-status") { if (!holdStatus) ack(id, status()); }
        else if (args.value("path") == "/machine/soc/electrical") {
            if (execute == "qom-list") {
                QJsonArray properties{QJsonObject{{"name", "snapshot-json"}, {"type", "string"}}};
                if (exposeProject) properties.append(QJsonObject{{"name", "project-json"}, {"type", "string"}});
                ack(id, properties);
            } else if (execute == "qom-get" && args.value("property") == "snapshot-json") {
                ++snapshotQueries;
                if (snapshotQueries == 1 && !holdDiscovery) {
                    auto value = snapshot(); value["abi"] = abi; snapshotAck(id, value);
                }
            } // Apply and later snapshot replies are intentionally withheld.
        } else if (execute == "qom-list" || execute == "qom-get") reject(id, "No legacy bridge object");
    }
};

static QJsonObject graph(const QString &id = "graph-a")
{
    return {{"version", 3}, {"id", id}, {"runtime", QJsonObject{{"electrical", QJsonObject{
                {"driver_profile", "s3-explicit-finite-v1"}, {"mode", "dc"}}}}},
            {"components", QJsonArray{}}, {"nets", QJsonArray{QJsonObject{{"id", "node"}, {"endpoints", QJsonArray{}}}}}};
}

static int readingCount(const QSignalSpy &spy)
{
    int count = 0;
    for (const auto &signal : spy) if (!signal.at(0).toJsonObject().isEmpty()) ++count;
    return count;
}

class QtElectricalBridgeTest : public QObject {
    Q_OBJECT
private slots:
    void discoveryRequiresMatchingAbiAndPropertyAcknowledgements() {
        ElectricalPeer peer;
        peer.holdDiscovery = true;
        QemuController controller;
        QVERIFY(!controller.nativeCircuitAvailable());
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(peer.snapshotQueries, 1);
        QVERIFY(!controller.nativeCircuitAvailable());
        QVERIFY(!controller.applyNativeCircuit(graph()));
        peer.snapshotAck(987654, ElectricalPeer::snapshot());
        QTest::qWait(30);
        QVERIFY(!controller.nativeCircuitAvailable());
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), ElectricalPeer::snapshot());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.nativeCircuitAvailable());
        QVERIFY(controller.nativeCircuitApplyUnavailableReason().isEmpty());
        QCOMPARE(peer.count("qom-set", "project-json"), 0);
    }
    void incompatibleAbiAndMissingProjectRemainUnavailable_data() {
        QTest::addColumn<int>("abi"); QTest::addColumn<bool>("project");
        QTest::newRow("wrong-abi") << 2 << true;
        QTest::newRow("missing-apply-property") << 1 << false;
    }
    void incompatibleAbiAndMissingProjectRemainUnavailable() {
        QFETCH(int, abi); QFETCH(bool, project);
        ElectricalPeer peer; peer.abi = abi; peer.exposeProject = project;
        QemuController controller; controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(!controller.nativeCircuitAvailable());
        QVERIFY(!controller.applyNativeCircuit(graph()));
        QCOMPARE(peer.count("qom-set", "project-json"), 0);
    }
    void runningUnknownAndPendingControlCannotApply() {
        ElectricalPeer peer; peer.running = true;
        QemuController controller; controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Running);
        QVERIFY(controller.nativeCircuitAvailable());
        QVERIFY(!controller.applyNativeCircuit(graph()));
        controller.pauseExecution();
        QTRY_COMPARE(peer.count("stop"), 1);
        QVERIFY(!controller.applyNativeCircuit(graph()));
        peer.running = false; peer.ack(peer.lastId("stop"));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        controller.continueExecution();
        QTRY_COMPARE(peer.count("cont"), 1);
        QVERIFY(!controller.applyNativeCircuit(graph()));
        QCOMPARE(peer.count("qom-set", "project-json"), 0);
        peer.holdStatus = true; peer.event("RESET");
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Initializing);
        QVERIFY(!controller.applyNativeCircuit(graph()));
    }
    void onlyMatchingAckAcceptsImmutableSubmittedGraphAndFailurePreservesIt() {
        ElectricalPeer peer;
        QemuController controller;
        QSignalSpy snapshots(&controller, &QemuController::nativeCircuitSnapshotUpdated);
        QSignalSpy applies(&controller, &QemuController::nativeCircuitApplyInFlightChanged);
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        auto draft = graph(); const auto submitted = draft;
        QVERIFY(controller.applyNativeCircuit(draft));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 1);
        const auto applyId = peer.lastId("qom-set", "project-json");
        draft["id"] = "edited-later";
        const auto applySignals = applies.count();
        QVERIFY(!controller.applyNativeCircuit(draft));
        QCOMPARE(applies.count(), applySignals);
        controller.continueExecution();
        QTest::qWait(30); QCOMPARE(peer.count("cont"), 0);
        peer.ack(applyId + 10000);
        QTest::qWait(30); QCOMPARE(peer.snapshotQueries, 1);
        QVERIFY(snapshots.last().at(1).toJsonObject().isEmpty());
        peer.ack(applyId);
        QTRY_COMPARE(peer.snapshotQueries, 2);
        QCOMPARE(snapshots.last().at(1).toJsonObject(), submitted);
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), ElectricalPeer::snapshot("1", true));
        QTRY_COMPARE(readingCount(snapshots), 1);
        QCOMPARE(snapshots.last().at(1).toJsonObject(), submitted);
        QVERIFY(controller.applyNativeCircuit(draft));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 2);
        peer.reject(peer.lastId("qom-set", "project-json"), "invalid resistor quantity");
        QTRY_VERIFY(applies.last().at(1).toString().contains("invalid resistor quantity"));
        QCOMPARE(snapshots.last().at(1).toJsonObject(), submitted);
        QVERIFY(controller.nativeCircuitAvailable());
    }
    void pendingSnapshotsCoalesceAndCannotCrossTopologyApply() {
        ElectricalPeer peer; QemuController controller;
        QSignalSpy snapshots(&controller, &QemuController::nativeCircuitSnapshotUpdated);
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.applyNativeCircuit(graph()));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 1);
        peer.ack(peer.lastId("qom-set", "project-json"));
        QTRY_COMPARE(peer.snapshotQueries, 2);
        const auto staleId = peer.lastId("qom-get", "snapshot-json");
        for (int i = 0; i < 50; ++i) controller.requestNativeCircuitSnapshot();
        QTest::qWait(30); QCOMPARE(peer.snapshotQueries, 2);
        const auto replacement = graph("graph-b");
        QVERIFY(controller.applyNativeCircuit(replacement));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 2);
        peer.snapshotAck(staleId, ElectricalPeer::snapshot("1", true));
        QTest::qWait(30); QCOMPARE(readingCount(snapshots), 0);
        peer.ack(peer.lastId("qom-set", "project-json"));
        QTRY_COMPARE(peer.snapshotQueries, 3);
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), ElectricalPeer::snapshot("2", true));
        QTRY_COMPARE(readingCount(snapshots), 1);
        QCOMPARE(snapshots.last().at(1).toJsonObject(), replacement);
    }
    void delayedApplyAckCannotCrossResetOrNewSession() {
        ElectricalPeer peer; QemuController controller;
        QSignalSpy snapshots(&controller, &QemuController::nativeCircuitSnapshotUpdated);
        QSignalSpy applies(&controller, &QemuController::nativeCircuitApplyInFlightChanged);
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.applyNativeCircuit(graph()));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 1);
        const auto staleId = peer.lastId("qom-set", "project-json");
        peer.event("RESET");
        QTRY_COMPARE(controller.runtimeStatus().resetEpoch, quint64(1));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(!applies.last().at(0).toBool());
        peer.ack(staleId); QTest::qWait(30);
        QCOMPARE(peer.snapshotQueries, 1);
        QVERIFY(snapshots.last().at(1).toJsonObject().isEmpty());
        QVERIFY(controller.applyNativeCircuit(graph("after-reset")));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 2);
        const auto previousId = peer.lastId("qom-set", "project-json");
        const auto previousSession = controller.runtimeStatus().sessionId;
        ElectricalPeer replacement; controller.attachQmpEndpointForTesting(replacement.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.runtimeStatus().sessionId != previousSession);
        replacement.ack(previousId); QTest::qWait(30);
        QCOMPARE(replacement.snapshotQueries, 1);
        QVERIFY(snapshots.last().at(1).toJsonObject().isEmpty());
    }
    void delayedSnapshotCannotCrossResetStopOrReconnect() {
        ElectricalPeer peer; QemuController controller;
        QSignalSpy snapshots(&controller, &QemuController::nativeCircuitSnapshotUpdated);
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.applyNativeCircuit(graph()));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 1);
        peer.ack(peer.lastId("qom-set", "project-json"));
        QTRY_COMPARE(peer.snapshotQueries, 2);
        const auto oldSnapshot = peer.lastId("qom-get", "snapshot-json");
        peer.event("RESET");
        QTRY_COMPARE(controller.runtimeStatus().resetEpoch, quint64(1));
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        peer.snapshotAck(oldSnapshot, ElectricalPeer::snapshot("1", true));
        QTest::qWait(30); QCOMPARE(readingCount(snapshots), 0);
        controller.requestNativeCircuitSnapshot();
        QTRY_COMPARE(peer.snapshotQueries, 3);
        const auto beforeStop = peer.lastId("qom-get", "snapshot-json");
        controller.stopSimulation();
        QVERIFY(!controller.nativeCircuitAvailable());
        QVERIFY(snapshots.last().at(0).toJsonObject().isEmpty());
        QVERIFY(snapshots.last().at(1).toJsonObject().isEmpty());
        ElectricalPeer replacement; controller.attachQmpEndpointForTesting(replacement.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        replacement.snapshotAck(beforeStop, ElectricalPeer::snapshot("1", true));
        QTest::qWait(30); QCOMPARE(readingCount(snapshots), 0);
        QVERIFY(snapshots.last().at(1).toJsonObject().isEmpty());
    }
    void malformedOrUnacceptedGenerationCannotBecomeReadings() {
        ElectricalPeer peer; QemuController controller;
        QSignalSpy snapshots(&controller, &QemuController::nativeCircuitSnapshotUpdated);
        QSignalSpy errors(&controller, &QemuController::nativeCircuitSnapshotUnavailable);
        controller.attachQmpEndpointForTesting(peer.port());
        QTRY_COMPARE(controller.runtimeStatus().phase, RuntimePhase::Paused);
        QVERIFY(controller.applyNativeCircuit(graph()));
        QTRY_COMPARE(peer.count("qom-set", "project-json"), 1);
        peer.ack(peer.lastId("qom-set", "project-json"));
        QTRY_COMPARE(peer.snapshotQueries, 2);
        auto malformed = ElectricalPeer::snapshot("1", true); malformed["timestamp_ns"] = 9007199254740992.0;
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), malformed);
        QTRY_COMPARE(errors.count(), 1); QCOMPARE(readingCount(snapshots), 0);
        QVERIFY(controller.nativeCircuitAvailable());
        controller.requestNativeCircuitSnapshot(); QTRY_COMPARE(peer.snapshotQueries, 3);
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), ElectricalPeer::snapshot("1", true));
        QTRY_COMPARE(readingCount(snapshots), 1);
        controller.requestNativeCircuitSnapshot(); QTRY_COMPARE(peer.snapshotQueries, 4);
        peer.snapshotAck(peer.lastId("qom-get", "snapshot-json"), ElectricalPeer::snapshot("2", true));
        QTRY_COMPARE(errors.count(), 2); QCOMPARE(readingCount(snapshots), 1);
        QVERIFY(snapshots.last().at(0).toJsonObject().isEmpty());
    }
};

QTEST_GUILESS_MAIN(QtElectricalBridgeTest)
#include "QtElectricalBridgeTest.moc"
