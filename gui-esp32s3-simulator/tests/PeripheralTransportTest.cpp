#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTimer>
#include <limits>

#include "PeripheralTransport.h"

static QByteArray framedJson(const QJsonObject &object)
{
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame;
    const quint32 size = quint32(json.size());
    for (int shift : {24, 16, 8, 0}) frame.append(char((size >> shift) & 255));
    return frame + json;
}
static QJsonObject requestData(const QByteArray &payload = QByteArray(1, char(0xe3)))
{
    return {{"bus", "i2c"}, {"controller_id", "i2c0"}, {"endpoint_ids", QJsonArray{"sensor0"}},
        {"net_ids", QJsonArray{"net.sda", "net.scl"}},
        {"phases", QJsonArray{QJsonObject{{"kind", "start"}}, QJsonObject{{"kind", "address"}, {"address", 64}, {"direction", "write"}},
            QJsonObject{{"kind", "write"}, {"length", payload.size()}}, QJsonObject{{"kind", "restart"}},
            QJsonObject{{"kind", "address"}, {"address", 64}, {"direction", "read"}},
            QJsonObject{{"kind", "read"}, {"length", 3}}, QJsonObject{{"kind", "stop"}}}},
        {"bit_length", QString::number(payload.size() * 8)}, {"bit_order", "msb-first"},
        {"mode", QJsonValue(QJsonValue::Null)}, {"chip_select", QJsonValue(QJsonValue::Null)},
        {"read_length", 3}, {"virtual_deadline_ns", "4000"},
        {"payload_encoding", "base64"}, {"payload", QString::fromLatin1(payload.toBase64())}};
}
static BusEnvelope fixture()
{
    BusEnvelope message;
    message.kind = BusMessageKind::Request; message.status = BusStatus::Pending;
    message.sessionId = "11111111-2222-3333-4444-555555555555";
    message.connectionId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    message.sequence = 1; message.requestId = "1"; message.virtualTimeNs = 1000;
    message.data = requestData();
    return message;
}
static QJsonObject responseData(const BusEnvelope &request, const QByteArray &payload, quint64 latency = 100)
{
    QJsonObject data;
    for (const QString &name : {QStringLiteral("bus"), QStringLiteral("controller_id"), QStringLiteral("endpoint_ids"), QStringLiteral("net_ids")})
        data[name] = request.data.value(name);
    data["phase_statuses"] = QJsonArray{QJsonObject{{"index", 1}, {"status", "ack"}}};
    data["modeled_latency_ns"] = QString::number(latency);
    data["accepted_length"] = QByteArray::fromBase64(request.data.value("payload").toString().toUtf8()).size();
    data["payload_encoding"] = "base64";
    data["payload"] = QString::fromLatin1(payload.toBase64());
    data["error_message"] = "";
    return data;
}

class DevicePeer : public QObject {
public:
    QTcpSocket socket;
    PeripheralFrameCodec codec;
    QList<BusEnvelope> received;
    BusEnvelope context;
    quint64 sequence = 0;
    bool automaticAck = true;
    std::function<void(QJsonObject &)> modifyAck;

    explicit DevicePeer(quint16 port) {
        connect(&socket, &QTcpSocket::readyRead, this, [this]() {
            QString error;
            QVERIFY2(codec.feed(socket.readAll(), [this](const BusEnvelope &message) {
                received.append(message);
                if (message.kind == BusMessageKind::Hello) {
                    context = message;
                    if (automaticAck) acknowledge();
                } else if (message.kind == BusMessageKind::Reset) context = message;
                return true;
            }, error), qPrintable(error));
        });
        socket.connectToHost(QHostAddress::LocalHost, port);
    }
    void acknowledge() {
        BusEnvelope ack = context; ack.kind = BusMessageKind::HelloAck; ack.sequence = ++sequence;
        QString error;
        const QByteArray frame = PeripheralFrameCodec::encode(ack, error);
        QVERIFY2(!frame.isEmpty(), qPrintable(error));
        QJsonObject object = QJsonDocument::fromJson(frame.mid(4)).object();
        if (modifyAck) modifyAck(object);
        socket.write(framedJson(object));
    }
    void send(BusEnvelope message) {
        if (!message.sequence) message.sequence = ++sequence;
        else sequence = qMax(sequence, message.sequence);
        QString error;
        const QByteArray frame = PeripheralFrameCodec::encode(message, error);
        QVERIFY2(!frame.isEmpty(), qPrintable(error));
        socket.write(frame);
    }
    void respond(const BusEnvelope &request, const QByteArray &bytes = QByteArray::fromHex("112233")) {
        BusEnvelope response = request;
        response.kind = BusMessageKind::Response; response.status = BusStatus::Ok;
        response.sequence = 0; response.virtualTimeNs += 100;
        response.data = responseData(request, bytes);
        send(response);
    }
    int count(BusMessageKind kind) const {
        int result = 0; for (const auto &message : received) if (message.kind == kind) ++result;
        return result;
    }
    BusEnvelope last(BusMessageKind kind) const {
        for (auto it = received.crbegin(); it != received.crend(); ++it) if (it->kind == kind) return *it;
        return {};
    }
};

class PeripheralTransportTest : public QObject {
    Q_OBJECT
private slots:
    void uint64RoundTripWithoutJsonNumberLoss() {
        BusEnvelope message = fixture();
        message.sequence = message.resetEpoch = message.topologyGeneration = std::numeric_limits<quint64>::max();
        message.virtualTimeNs = std::numeric_limits<quint64>::max() - 10;
        message.requestId = "18446744073709551615";
        message.data["virtual_deadline_ns"] = "18446744073709551615";
        QString error;
        const auto frame = PeripheralFrameCodec::encode(message, error);
        QVERIFY2(!frame.isEmpty(), qPrintable(error));
        BusEnvelope decoded;
        QVERIFY(PeripheralFrameCodec::decode(frame.mid(4), decoded, error));
        QCOMPARE(decoded.sequence, message.sequence);
        QCOMPARE(decoded.resetEpoch, message.resetEpoch);
        QCOMPARE(decoded.virtualTimeNs, message.virtualTimeNs);
        QCOMPARE(decoded.requestId, message.requestId);
        QCOMPARE(decoded.data, message.data);
    }
    void fragmentedAndCoalescedFrames() {
        QString error;
        BusEnvelope first = fixture(), second = fixture();
        second.sequence = 2; second.requestId = "2";
        const auto bytes = PeripheralFrameCodec::encode(first, error) + PeripheralFrameCodec::encode(second, error);
        PeripheralFrameCodec codec;
        QList<BusEnvelope> messages;
        for (char byte : bytes) {
            QVERIFY2(codec.feed(QByteArray(1, byte), [&](const BusEnvelope &message) { messages.append(message); return true; }, error), qPrintable(error));
            QVERIFY(codec.bufferedBytes() <= PeripheralTransportLimits::MaximumFrameBytes + 4);
        }
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(1).requestId, QString("2"));
        QVERIFY(codec.finish(error));
        messages.clear();
        QVERIFY(codec.feed(bytes, [&](const BusEnvelope &message) { messages.append(message); return true; }, error));
        QCOMPARE(messages.size(), 2);
        QCOMPARE(codec.bufferedBytes(), qsizetype(0));
    }
    void maximumPayloadAndFrameBoundaries() {
        auto message = fixture(); message.data = requestData(QByteArray(65536, 'x'));
        QString error;
        const auto frame = PeripheralFrameCodec::encode(message, error);
        QVERIFY2(!frame.isEmpty(), qPrintable(error));
        BusEnvelope decoded;
        QVERIFY(PeripheralFrameCodec::decode(frame.mid(4), decoded, error));
        QCOMPARE(QByteArray::fromBase64(decoded.data.value("payload").toString().toUtf8()).size(), qsizetype(65536));
        QByteArray body = frame.mid(4);
        body += QByteArray(PeripheralTransportLimits::MaximumFrameBytes - body.size(), ' ');
        const QByteArray maximumFrame = QByteArray::fromHex("00100000") + body;
        PeripheralFrameCodec codec;
        int delivered = 0;
        for (qsizetype position = 0; position < maximumFrame.size(); position += 65536)
            QVERIFY2(codec.feed(maximumFrame.mid(position, 65536), [&](const BusEnvelope &) { ++delivered; return true; }, error), qPrintable(error));
        QCOMPARE(delivered, 1);
        QVERIFY(codec.finish(error));
    }
    void invalidPrefixAndTruncatedStream() {
        PeripheralFrameCodec codec;
        QString error;
        QVERIFY(!codec.feed(QByteArray::fromHex("00100001"), [](const BusEnvelope &) { return true; }, error));
        QVERIFY(error.contains("prefix"));
        QCOMPARE(codec.bufferedBytes(), qsizetype(0));
        QVERIFY(!codec.feed(QByteArray(4, '\0'), [](const BusEnvelope &) { return true; }, error));
        const auto frame = PeripheralFrameCodec::encode(fixture(), error);
        QVERIFY(codec.feed(frame.left(frame.size() - 1), [](const BusEnvelope &) { return true; }, error));
        QVERIFY(!codec.finish(error));
        QVERIFY(error.contains("Truncated"));
    }
    void invalidFields_data() {
        QTest::addColumn<QByteArray>("body");
        QString error;
        const auto valid = QJsonDocument::fromJson(PeripheralFrameCodec::encode(fixture(), error).mid(4)).object();
        auto row = [&](const char *name, const QString &field, const QJsonValue &value) {
            auto object = valid; object[field] = value;
            QTest::newRow(name) << QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        row("uint64-must-be-string", "sequence", 9007199254740992.0);
        row("uint64-leading-zero", "reset_epoch", "01");
        row("uint64-overflow", "virtual_time_ns", "18446744073709551616");
        row("uint64-negative", "topology_generation", "-1");
        row("zero-sequence", "sequence", "0");
        row("bad-session", "session_id", "not-a-uuid");
        row("null-uuid", "connection_id", "00000000-0000-0000-0000-000000000000");
        row("unknown-top-field", "surprise", true);
        row("unknown-major", "version", QJsonObject{{"major", 2}, {"minor", 0}});
        row("negative-minor", "version", QJsonObject{{"major", 1}, {"minor", -1}});
        row("unknown-kind", "kind", "uart-log");
        row("request-status-not-pending", "status", "ok");
        row("bad-request-ordinal", "request_id", "r1");
        row("lf-sequence", "sequence", "1\n");
        row("lf-request-ordinal", "request_id", "1\n");
        row("lf-epoch", "reset_epoch", "0\n");
        row("lf-topology", "topology_generation", "0\n");
        row("lf-virtual-time", "virtual_time_ns", "1000\n");
        row("lf-session-uuid", "session_id", valid.value("session_id").toString() + '\n');
        row("lf-connection-uuid", "connection_id", valid.value("connection_id").toString() + '\n');
        auto dataRow = [&](const char *name, const QString &field, const QJsonValue &value) {
            auto object = valid; auto data = valid.value("data").toObject(); data[field] = value; object["data"] = data;
            QTest::newRow(name) << QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        dataRow("payload-encoding-required", "payload_encoding", "raw");
        dataRow("bad-base64", "payload", "%%%bad%%%");
        dataRow("noncanonical-pad-bits", "payload", "AB==");
        dataRow("oversized-payload", "payload", QString::fromLatin1(QByteArray(65537, 'x').toBase64()));
        dataRow("excess-read", "read_length", 65537);
        dataRow("empty-nets", "net_ids", QJsonArray{});
        dataRow("duplicate-endpoint", "endpoint_ids", QJsonArray{"sensor0", "sensor0"});
        dataRow("past-deadline", "virtual_deadline_ns", "999");
        dataRow("wrong-mode-for-i2c", "mode", 3);
        dataRow("unknown-data-field", "silent_route", "i2c0");
        dataRow("oversized-parameters", "parameters", QJsonObject{{"huge", QString(16384, 'x')}});
        dataRow("lf-controller", "controller_id", "i2c0\n");
        dataRow("lf-endpoint", "endpoint_ids", QJsonArray{"sensor0\n"});
        dataRow("lf-net", "net_ids", QJsonArray{"net.sda\n", "net.scl"});
        dataRow("lf-bit-length", "bit_length", "8\n");
        dataRow("lf-deadline", "virtual_deadline_ns", "4000\n");
        dataRow("bad-phase-address", "phases", QJsonArray{QJsonObject{{"kind", "address"}, {"address", 1024}}});
        QJsonArray phases; for (int i = 0; i < 65; ++i) phases.append(QJsonObject{{"kind", "start"}});
        dataRow("too-many-phases", "phases", phases);
        QTest::newRow("invalid-utf8") << QByteArray::fromHex("7b22ffc0223a317d");
        QTest::newRow("malformed-json") << QByteArray("{broken");
        auto duplicated = QJsonDocument(valid).toJson(QJsonDocument::Compact);
        duplicated.replace("\"sequence\":\"1\"", "\"sequence\":\"1\",\"sequence\":\"2\"");
        QTest::newRow("duplicate-json-member") << duplicated;
        duplicated = QJsonDocument(valid).toJson(QJsonDocument::Compact);
        duplicated.replace("\"sequence\":\"1\"", "\"sequence\":\"1\",\"\\u0073equence\":\"2\"");
        QTest::newRow("escaped-duplicate-member") << duplicated;
        auto deep = valid; QJsonObject nested{{"leaf", 1}};
        for (int i = 0; i < 65; ++i) nested = QJsonObject{{"node", nested}};
        auto deepData = deep.value("data").toObject(); deepData["parameters"] = nested; deep["data"] = deepData;
        QTest::newRow("excess-json-depth") << QJsonDocument(deep).toJson(QJsonDocument::Compact);
    }
    void invalidFields() {
        QFETCH(QByteArray, body);
        BusEnvelope decoded; QString error;
        QVERIFY(!PeripheralFrameCodec::decode(body, decoded, error));
        QVERIFY(!error.isEmpty());
    }
    void negotiationAndActualResponseBytes() {
        PeripheralTransport transport;
        QVERIFY(transport.listen());
        DevicePeer peer(transport.port());
        QSignalSpy responses(&transport, &PeripheralTransport::responseReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QVERIFY(transport.negotiatedCapabilities().contains("bus.i2c.v1"));
        QCOMPARE(transport.negotiatedLimits().busPayloadBytes, quint32(65536));
        const auto id = transport.sendRequest(requestData(), 1000);
        QCOMPARE(id, QString("1"));
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        peer.respond(peer.last(BusMessageKind::Request));
        QTRY_COMPARE(responses.size(), 1);
        const auto response = responses.first().at(0).value<BusEnvelope>();
        QCOMPARE(QByteArray::fromBase64(response.data.value("payload").toString().toUtf8()), QByteArray::fromHex("112233"));
        QCOMPARE(response.virtualTimeNs, quint64(1100));
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void incomingRequestAndReplyUsesOpaqueContext() {
        PeripheralTransport transport;
        QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy requests(&transport, &PeripheralTransport::requestReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request);
        QTRY_COMPARE(requests.size(), 1);
        const auto original = requests.first().at(0).value<BusEnvelope>();
        QVERIFY(transport.sendResponse(original, responseData(original, QByteArray::fromHex("aabbcc")), BusStatus::Ok, 1100));
        QTRY_COMPARE(peer.count(BusMessageKind::Response), 1);
        QCOMPARE(peer.last(BusMessageKind::Response).data.value("net_ids"), request.data.value("net_ids"));
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void missingCapabilitiesAndUnsupportedMinor() {
        for (int scenario = 0; scenario < 4; ++scenario) {
            PeripheralTransport transport; QVERIFY(transport.listen());
            DevicePeer peer(transport.port());
            peer.modifyAck = [scenario](QJsonObject &ack) {
                if (scenario == 2) { ack["version"] = QJsonObject{{"major", 1}, {"minor", 1}}; return; }
                auto data = ack.value("data").toObject();
                data["required_capabilities"] = QJsonArray{};
                data["capabilities"] = scenario == 0 ? QJsonArray{"bus.i2c.v1"}
                    : QJsonArray{"control.reset-generation", "payload.base64", scenario == 3 ? "bus.i2c.v1\n" : "unoffered.feature"};
                ack["data"] = data;
            };
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
        }
    }
    void localLimitRangesAndNegotiatedReduction() {
        PeripheralTransport transport;
        PeripheralTransportLimits limit;
        for (int scenario = 0; scenario < 12; ++scenario) {
            limit = {};
            switch (scenario) {
            case 0: limit.frameBytes = 511; break;
            case 1: limit.frameBytes = 1048577; break;
            case 2: limit.busPayloadBytes = 0; break;
            case 3: limit.busPayloadBytes = 65537; break;
            case 4: limit.inFlight = 0; break;
            case 5: limit.inFlight = 65; break;
            case 6: limit.queuedBytes = 511; break;
            case 7: limit.queuedBytes = 4194305; break;
            case 8: limit.discardedMessageLimit = 0; break;
            case 9: limit.discardedMessageLimit = 257; break;
            case 10: limit.hostWatchdogMs = 0; break;
            case 11: limit.hostWatchdogMs = 600001; break;
            }
            QVERIFY(!transport.setLimits(limit));
        }
        QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        peer.modifyAck = [](QJsonObject &ack) {
            auto data = ack.value("data").toObject();
            data["limits"] = QJsonObject{{"frame_bytes", 4096}, {"bus_payload_bytes", 1024}, {"in_flight", 2}};
            ack["data"] = data;
        };
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QCOMPARE(transport.negotiatedLimits().inFlight, quint32(2));
        QVERIFY(!transport.setLimits({}));
        QVERIFY(!transport.sendRequest(requestData(QByteArray(1025, 'x')), 1000).size());
    }
    void peerCannotIncreaseLimitsOrAdvanceGenerations() {
        {
            PeripheralTransport transport; PeripheralTransportLimits limit; limit.frameBytes = 4096;
            QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
            peer.modifyAck = [](QJsonObject &ack) {
                auto data = ack.value("data").toObject(); auto limits = data.value("limits").toObject();
                limits["frame_bytes"] = 8192; data["limits"] = limits; ack["data"] = data;
            };
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
        }
        {
            PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
            auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
            request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.resetEpoch = 1; request.data = requestData();
            peer.send(request);
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
        }
    }
    void contradictoryHandshakeConfigurationFailsBeforeListen() {
        PeripheralTransport transport;
        PeripheralTransportLimits limit; limit.frameBytes = limit.queuedBytes = 512;
        QString error;
        QVERIFY(!transport.setLimits(limit, &error));
        QVERIFY(error.contains("Handshake/reset requires"));
        QCOMPARE(transport.port(), quint16(0));
        limit.frameBytes = limit.queuedBytes = 1024;
        QVERIFY(transport.setLimits(limit, &error));
        QStringList many{"control.reset-generation", "payload.base64"};
        for (int i = 0; i < 28; ++i) many.append(QString("feature-%1").arg(i).leftJustified(64, 'x'));
        QVERIFY(!transport.setCapabilities(many, {"control.reset-generation", "payload.base64"}, &error));
        QVERIFY(error.contains("Handshake/reset requires"));
        QVERIFY(transport.beginSession("11111111-2222-3333-4444-555555555555",
            std::numeric_limits<quint64>::max(), std::numeric_limits<quint64>::max(), &error));
        QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QCOMPARE(peer.context.resetEpoch, std::numeric_limits<quint64>::max());
        QVERIFY(transport.negotiatedCapabilities().contains("bus.i2c.v1"));
    }
    void resetDuringHandshakeRequiresFreshNegotiation() {
        PeripheralTransport transport; QVERIFY(transport.listen());
        {
            DevicePeer peer(transport.port()); peer.automaticAck = false;
            QTRY_COMPARE(peer.count(BusMessageKind::Hello), 1);
            QVERIFY(transport.resetGeneration(1, 1, 1000));
            QCOMPARE(transport.state(), PeripheralTransportState::Disconnected);
        }
        DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QCOMPARE(peer.context.resetEpoch, quint64(1));
        QCOMPARE(peer.context.topologyGeneration, quint64(1));
    }
    void outboundInflightAndByteQueueSaturation() {
        {
            PeripheralTransport transport;
            PeripheralTransportLimits limit; limit.inFlight = 2;
            QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
            QVERIFY(!transport.sendRequest(requestData(), 1000).isEmpty());
            QVERIFY(!transport.sendRequest(requestData(), 1000).isEmpty());
            QVERIFY(transport.sendRequest(requestData(), 1000).isEmpty());
            QCOMPARE(transport.inFlightCount(), 2);
            QTRY_COMPARE(peer.count(BusMessageKind::Request), 2);
            peer.respond(peer.received.at(1));
            QTRY_COMPARE(transport.inFlightCount(), 1);
            QCOMPARE(transport.sendRequest(requestData(), 1000), QString("3"));
        }
        {
            PeripheralTransport transport;
            PeripheralTransportLimits limit; limit.frameBytes = 4096; limit.busPayloadBytes = 1024; limit.queuedBytes = 4096;
            QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
            QVERIFY(!transport.sendRequest(requestData(QByteArray(1024, 'x')), 1000).isEmpty());
            QVERIFY(transport.sendRequest(requestData(QByteArray(1024, 'x')), 1000).isEmpty());
            QVERIFY(transport.queuedByteCount() <= 4096);
            QCOMPARE(transport.inFlightCount(), 1);
        }
    }
    void resetDiscardsOldAndDuplicateReplies() {
        PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy responses(&transport, &PeripheralTransport::responseReceived);
        QSignalSpy discarded(&transport, &PeripheralTransport::messageDiscarded);
        QSignalSpy cancelled(&transport, &PeripheralTransport::requestCancelled);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        transport.sendRequest(requestData(), 1000);
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        const auto old = peer.last(BusMessageKind::Request);
        QVERIFY(transport.resetGeneration(1, 1, 1100));
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(transport.inFlightCount(), 0);
        peer.respond(old);
        QTRY_COMPARE(discarded.size(), 1);
        QCOMPARE(responses.size(), 0);
        QCOMPARE(transport.sendRequest(requestData(), 1200), QString("2"));
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 2);
        const auto current = peer.last(BusMessageKind::Request);
        peer.respond(current);
        QTRY_COMPARE(responses.size(), 1);
        BusEnvelope duplicate = current; duplicate.kind = BusMessageKind::Response; duplicate.status = BusStatus::Ok;
        duplicate.sequence = peer.sequence; duplicate.virtualTimeNs += 100; duplicate.data = responseData(current, QByteArray::fromHex("112233"));
        peer.send(duplicate);
        QTRY_COMPARE(discarded.size(), 2);
        duplicate.sequence = 0; peer.send(duplicate);
        QTRY_COMPARE(discarded.size(), 3);
        QCOMPARE(responses.size(), 1);
        QVERIFY(!transport.resetGeneration(0, 0, 1300));
    }
    void reentrantResetCallbacksPreserveResetThenRequestAndNoOrphans() {
        PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QCOMPARE(transport.sendRequest(requestData(), 1000), QString("1"));
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        QString cancellationRetry, generationRetry;
        connect(&transport, &PeripheralTransport::requestCancelled, this, [&](const QString &, bool, const QString &) {
            cancellationRetry = transport.sendRequest(requestData(), 1200);
        });
        connect(&transport, &PeripheralTransport::generationChanged, this, [&](quint64, quint64) {
            generationRetry = transport.sendRequest(requestData(), 1200);
        });
        QVERIFY(transport.resetGeneration(1, 1, 1100));
        QVERIFY(cancellationRetry.isEmpty());
        QCOMPARE(generationRetry, QString("2"));
        QCOMPARE(transport.inFlightCount(), 1);
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 2);
        QTRY_COMPARE(peer.count(BusMessageKind::Reset), 1);
        QCOMPARE(peer.received.at(peer.received.size() - 2).kind, BusMessageKind::Reset);
        QCOMPARE(peer.received.last().kind, BusMessageKind::Request);
        QCOMPARE(peer.received.last().requestId, generationRetry);
        peer.respond(peer.received.last());
        QTRY_COMPARE(transport.inFlightCount(), 0);
    }
    void reconnectUsesNewConnectionAndCancelsOutstandingWork() {
        PeripheralTransport transport; QVERIFY(transport.listen());
        BusEnvelope old;
        {
            DevicePeer peer(transport.port());
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
            transport.sendRequest(requestData(), 1000);
            QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
            old = peer.last(BusMessageKind::Request);
            peer.socket.disconnectFromHost();
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Disconnected);
        }
        QCOMPARE(transport.inFlightCount(), 0);
        DevicePeer peer(transport.port());
        QSignalSpy discarded(&transport, &PeripheralTransport::messageDiscarded);
        QSignalSpy responses(&transport, &PeripheralTransport::responseReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        QVERIFY(transport.connectionId() != old.connectionId);
        QCOMPARE(transport.sendRequest(requestData(), 1000), QString("1"));
        peer.respond(old);
        QTRY_COMPARE(discarded.size(), 1);
        QCOMPARE(responses.size(), 0);
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        peer.respond(peer.last(BusMessageKind::Request));
        QTRY_COMPARE(responses.size(), 1);
    }
    void outOfOrderRequestAndDiscardFloodAreBounded() {
        PeripheralTransport transport; PeripheralTransportLimits limit; limit.discardedMessageLimit = 2;
        QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy requests(&transport, &PeripheralTransport::requestReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "5"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request); QTRY_COMPARE(requests.size(), 1);
        request.requestId = "4"; peer.send(request);
        QTest::qWait(30); QCOMPARE(requests.size(), 1);
        peer.send(request);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void delayedModelCallbackCannotAnswerReusedOrdinalAfterReconnect() {
        PeripheralTransport transport; QVERIFY(transport.listen());
        QSignalSpy requests(&transport, &PeripheralTransport::requestReceived);
        BusEnvelope original;
        {
            DevicePeer peer(transport.port());
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
            auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
            request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.data = requestData();
            peer.send(request); QTRY_COMPARE(requests.size(), 1);
            original = requests.first().at(0).value<BusEnvelope>();
            peer.socket.disconnectFromHost();
            QTRY_COMPARE(transport.state(), PeripheralTransportState::Disconnected);
        }
        DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request); QTRY_COMPARE(requests.size(), 2);
        const auto current = requests.last().at(0).value<BusEnvelope>();
        QCOMPARE(current.requestId, original.requestId);
        QCOMPARE(current.virtualTimeNs, original.virtualTimeNs);
        QCOMPARE(current.data, original.data);
        QVERIFY(current.connectionId != original.connectionId);
        bool callbackDone = false, staleAccepted = true;
        QString error;
        QTimer::singleShot(0, &transport, [&]() {
            staleAccepted = transport.sendResponse(original, responseData(original, QByteArray::fromHex("aabbcc")), BusStatus::Ok, 1100, &error);
            callbackDone = true;
        });
        QTRY_VERIFY(callbackDone);
        QVERIFY(!staleAccepted);
        QVERIFY(error.contains("context"));
        QCOMPARE(transport.inFlightCount(), 1);
        QCOMPARE(peer.count(BusMessageKind::Response), 0);
        QVERIFY(transport.sendResponse(current, responseData(current, QByteArray::fromHex("112233")), BusStatus::Ok, 1100));
        QTRY_COMPARE(peer.count(BusMessageKind::Response), 1);
        QCOMPARE(peer.last(BusMessageKind::Response).data.value("payload").toString(), QString("ESIz"));
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void delayedModelCallbackAfterResetCannotProduceResponse() {
        PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy requests(&transport, &PeripheralTransport::requestReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request); QTRY_COMPARE(requests.size(), 1);
        const auto original = requests.first().at(0).value<BusEnvelope>();
        QVERIFY(transport.resetGeneration(1, 1, 1050));
        QTRY_COMPARE(peer.count(BusMessageKind::Reset), 1);
        request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "2"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request); QTRY_COMPARE(requests.size(), 2);
        const auto current = requests.last().at(0).value<BusEnvelope>();
        bool callbackDone = false, staleAccepted = true;
        QTimer::singleShot(0, &transport, [&]() {
            staleAccepted = transport.sendResponse(original, responseData(original, QByteArray::fromHex("aabbcc")), BusStatus::Ok, 1100);
            callbackDone = true;
        });
        QTRY_VERIFY(callbackDone);
        QVERIFY(!staleAccepted);
        QCOMPARE(transport.inFlightCount(), 1);
        QCOMPARE(peer.count(BusMessageKind::Response), 0);
        for (int field = 0; field < 6; ++field) {
            auto modified = current;
            switch (field) {
            case 0: modified.sessionId = original.sessionId + "modified"; break;
            case 1: modified.connectionId = original.connectionId + "modified"; break;
            case 2: modified.resetEpoch = original.resetEpoch; break;
            case 3: modified.topologyGeneration = original.topologyGeneration; break;
            case 4: ++modified.sequence; break;
            case 5: modified.requestId = original.requestId; break;
            }
            QVERIFY(!transport.sendResponse(modified, responseData(current, QByteArray::fromHex("112233")), BusStatus::Ok, 1100));
        }
        QVERIFY(transport.sendResponse(current, responseData(current, QByteArray::fromHex("112233")), BusStatus::Ok, 1100));
        QTRY_COMPARE(peer.count(BusMessageKind::Response), 1);
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void incompleteFrameAssemblyWatchdogIsBounded() {
        PeripheralTransport transport; PeripheralTransportLimits limit; limit.hostWatchdogMs = 250;
        QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy failed(&transport, &PeripheralTransport::protocolError);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        peer.socket.write(QByteArray::fromHex("00100000")); // Prefix complete, body never starts.
        QTRY_COMPARE_WITH_TIMEOUT(transport.state(), PeripheralTransportState::Error, 1500);
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first().at(0).toString().contains("assembly watchdog"));
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void incomingInflightFloodFailsWithoutGrowth() {
        PeripheralTransport transport; PeripheralTransportLimits limit; limit.inFlight = 1;
        QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.virtualTimeNs = 1000; request.data = requestData();
        request.requestId = "1"; peer.send(request);
        request.requestId = "2"; peer.send(request);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
        QCOMPARE(transport.inFlightCount(), 0);
    }
    void truncatedTcpFrameIsAProtocolFailure() {
        PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = fixture(); request.sessionId = peer.context.sessionId; request.connectionId = peer.context.connectionId; request.sequence = 2;
        QString error;
        const auto frame = PeripheralFrameCodec::encode(request, error);
        peer.socket.write(frame.left(frame.size() - 1)); peer.socket.disconnectFromHost();
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Error);
    }
    void hostWatchdogNeverCreatesGuestTimeout() {
        PeripheralTransport transport; PeripheralTransportLimits limit; limit.hostWatchdogMs = 250;
        QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy stalled(&transport, &PeripheralTransport::hostServiceStalled);
        QSignalSpy responses(&transport, &PeripheralTransport::responseReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto data = requestData(); data["virtual_deadline_ns"] = "18446744073709551615";
        transport.sendRequest(data, 1000);
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        QTRY_COMPARE_WITH_TIMEOUT(transport.state(), PeripheralTransportState::Stalled, 1500);
        QCOMPARE(stalled.size(), 1);
        const auto pending = stalled.first().at(0).value<BusEnvelope>();
        QCOMPARE(pending.virtualTimeNs, quint64(1000));
        QVERIFY(pending.sequence > 0);
        QCOMPARE(responses.size(), 0);
        QCOMPARE(transport.inFlightCount(), 0);
        QCOMPARE(peer.count(BusMessageKind::Response), 0);
    }
    void slowHostReplyStillUsesModeledVirtualCompletion() {
        PeripheralTransport transport; PeripheralTransportLimits limit; limit.hostWatchdogMs = 1000;
        QVERIFY(transport.setLimits(limit)); QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy stalled(&transport, &PeripheralTransport::hostServiceStalled);
        QSignalSpy responses(&transport, &PeripheralTransport::responseReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        transport.sendRequest(requestData(), 1000);
        QTRY_COMPARE(peer.count(BusMessageKind::Request), 1);
        QTest::qWait(100);
        peer.respond(peer.last(BusMessageKind::Request));
        QTRY_COMPARE(responses.size(), 1);
        QCOMPARE(responses.first().at(0).value<BusEnvelope>().virtualTimeNs, quint64(1100));
        QCOMPARE(stalled.size(), 0);
    }
    void responseTopologyAndVirtualDeadlineAreChecked() {
        PeripheralTransport transport; QVERIFY(transport.listen()); DevicePeer peer(transport.port());
        QSignalSpy requests(&transport, &PeripheralTransport::requestReceived);
        QTRY_COMPARE(transport.state(), PeripheralTransportState::Ready);
        auto request = peer.context; request.kind = BusMessageKind::Request; request.status = BusStatus::Pending;
        request.sequence = 0; request.requestId = "1"; request.virtualTimeNs = 1000; request.data = requestData();
        peer.send(request); QTRY_COMPARE(requests.size(), 1);
        const auto original = requests.first().at(0).value<BusEnvelope>();
        auto response = responseData(original, QByteArray::fromHex("112233"));
        response["net_ids"] = QJsonArray{"wrong-net"};
        QVERIFY(!transport.sendResponse(original, response, BusStatus::Ok, 1100));
        response = responseData(original, QByteArray::fromHex("112233"), 5000);
        QVERIFY(!transport.sendResponse(original, response, BusStatus::Ok, 6000));
        response = responseData(original, QByteArray::fromHex("112233"), 3000);
        QVERIFY(transport.sendResponse(original, response, BusStatus::Timeout, 4000));
    }
};

QTEST_GUILESS_MAIN(PeripheralTransportTest)
#include "PeripheralTransportTest.moc"
