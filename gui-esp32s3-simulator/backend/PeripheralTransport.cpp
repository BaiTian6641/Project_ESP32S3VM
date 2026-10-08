#include "PeripheralTransport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <limits>

namespace {
void whitespace(const QByteArray &bytes, qsizetype &position)
{
    while (position < bytes.size()) {
        const char c = bytes.at(position);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return;
        ++position;
    }
}
bool stringEnd(const QByteArray &bytes, qsizetype &position)
{
    if (position >= bytes.size() || bytes.at(position++) != '"') return false;
    bool escaped = false;
    while (position < bytes.size()) {
        const char character = bytes.at(position++);
        if (escaped) escaped = false;
        else if (character == '\\') escaped = true;
        else if (character == '"') return true;
    }
    return false;
}
// QJsonDocument collapses duplicate members. Inspect the original valid JSON so
// aliases such as "sequence" and "\u0073equence" cannot disagree across peers.
bool uniqueMembers(const QByteArray &bytes, qsizetype &position, int depth, QString &error)
{
    whitespace(bytes, position);
    if (depth > 64 || position >= bytes.size()) { error = "JSON nesting exceeds 64 levels"; return false; }
    const char opening = bytes.at(position);
    if (opening == '"') return stringEnd(bytes, position);
    if (opening == '{' || opening == '[') {
        ++position;
        QSet<QString> members;
        const char closing = opening == '{' ? '}' : ']';
        whitespace(bytes, position);
        if (bytes.at(position) == closing) { ++position; return true; }
        while (position < bytes.size()) {
            if (opening == '{') {
                const qsizetype beginning = position;
                if (!stringEnd(bytes, position)) return false;
                const auto wrapped = QJsonDocument::fromJson(QByteArray("[") + bytes.mid(beginning, position - beginning) + ']');
                const QString name = wrapped.array().at(0).toString();
                if (members.contains(name)) { error = "Duplicate JSON member: " + name; return false; }
                members.insert(name);
                whitespace(bytes, position);
                ++position; // Colon: syntax was already validated by QJsonDocument.
            }
            if (!uniqueMembers(bytes, position, depth + 1, error)) return false;
            whitespace(bytes, position);
            if (bytes.at(position++) == closing) return true;
            whitespace(bytes, position); // Otherwise the validated delimiter is a comma.
        }
        return false;
    }
    while (position < bytes.size()) {
        const char c = bytes.at(position);
        if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;
        ++position;
    }
    return true;
}
bool uuid(const QString &value)
{
    static const QRegularExpression pattern("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
    return pattern.match(value).hasMatch() && !QUuid(value).isNull();
}
bool token(const QString &value)
{
    static const QRegularExpression pattern("\\A[A-Za-z0-9][A-Za-z0-9_.:/-]{0,63}\\z");
    return pattern.match(value).hasMatch();
}
bool unsignedText(const QJsonValue &value, quint64 &number)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern("\\A(0|[1-9][0-9]*)\\z");
    const QString text = value.toString();
    if (text.size() > 20 || !pattern.match(text).hasMatch()) return false;
    bool ok = false;
    number = text.toULongLong(&ok);
    return ok;
}
bool integer(const QJsonValue &value, double minimum, double maximum)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum;
}
bool keys(const QJsonObject &object, const QStringList &required,
          const QStringList &optional, QString &error)
{
    for (const QString &name : required) {
        if (!object.contains(name)) { error = "Missing field: " + name; return false; }
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key())) {
            error = "Unknown field: " + it.key(); return false;
        }
    }
    return true;
}
bool tokens(const QJsonValue &value, int minimum, int maximum, bool capabilities, QString &error)
{
    if (!value.isArray() || value.toArray().size() < minimum || value.toArray().size() > maximum) {
        error = "Invalid identifier/capability list length"; return false;
    }
    QSet<QString> seen;
    static const QRegularExpression feature("\\A[a-z][a-z0-9.-]{0,63}\\z");
    for (const auto &entry : value.toArray()) {
        const QString text = entry.toString();
        if (!entry.isString() || !(capabilities ? feature.match(text).hasMatch() : token(text)) || seen.contains(text)) {
            error = "Invalid or duplicate identifier/capability"; return false;
        }
        seen.insert(text);
    }
    return true;
}
QStringList strings(const QJsonArray &array)
{
    QStringList result;
    for (const auto &value : array) result.append(value.toString());
    return result;
}
bool payload(const QJsonObject &data, quint32 limit, QByteArray &decoded, QString &error)
{
    if (data.value("payload_encoding") != "base64" || !data.value("payload").isString()) {
        error = "Payload must explicitly use base64 encoding"; return false;
    }
    const QByteArray encoded = data.value("payload").toString().toUtf8();
    if (encoded.size() > ((limit + 2) / 3) * 4) { error = "Bus payload limit exceeded"; return false; }
    decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() > limit || decoded.toBase64() != encoded) {
        error = "Invalid/noncanonical base64 or bus payload limit exceeded"; return false;
    }
    return true;
}
bool metadata(const QJsonValue &value)
{
    return value.isObject() && QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).size() <= 16384;
}
bool busContext(const QJsonObject &data, QString &error)
{
    static const QStringList buses{"i2c", "spi", "gpio", "uart", "i2s", "rmt", "lcd", "cam"};
    if (!data.value("bus").isString() || !buses.contains(data.value("bus").toString())
        || !data.value("controller_id").isString() || !token(data.value("controller_id").toString())) {
        error = "Invalid bus/controller identifier"; return false;
    }
    return tokens(data.value("endpoint_ids"), 0, 64, false, error)
        && tokens(data.value("net_ids"), 1, 64, false, error);
}
bool validateData(const BusEnvelope &message, quint32 payloadLimit, QString &error)
{
    const auto &data = message.data;
    quint64 number = 0;
    if (message.kind == BusMessageKind::Hello || message.kind == BusMessageKind::HelloAck) {
        if (message.status != BusStatus::Ok || !keys(data, {"capabilities", "required_capabilities", "limits"}, {}, error)
            || !tokens(data.value("capabilities"), 0, 32, true, error)
            || !tokens(data.value("required_capabilities"), 0, 32, true, error)
            || !data.value("limits").isObject()) { if (error.isEmpty()) error = "Invalid handshake"; return false; }
        const auto limits = data.value("limits").toObject();
        if (!keys(limits, {"frame_bytes", "bus_payload_bytes", "in_flight"}, {}, error)
            || !integer(limits.value("frame_bytes"), 512, PeripheralTransportLimits::MaximumFrameBytes)
            || !integer(limits.value("bus_payload_bytes"), 1, PeripheralTransportLimits::MaximumBusPayloadBytes)
            || !integer(limits.value("in_flight"), 1, PeripheralTransportLimits::MaximumInFlight)) {
            if (error.isEmpty()) error = "Invalid handshake limits"; return false;
        }
        const auto supported = strings(data.value("capabilities").toArray());
        for (const QString &required : strings(data.value("required_capabilities").toArray())) {
            if (!supported.contains(required)) { error = "Required capability was not offered"; return false; }
        }
        return true;
    }
    if (message.kind == BusMessageKind::Reset) {
        if (message.status != BusStatus::Ok || !keys(data, {"reason"}, {}, error)
            || !data.value("reason").isString() || data.value("reason").toString().size() > 512) {
            if (error.isEmpty()) error = "Invalid reset record"; return false;
        }
        return true;
    }
    QByteArray bytes;
    if (!payload(data, payloadLimit, bytes, error)) return false;
    if (message.kind == BusMessageKind::Event) {
        if ((message.status != BusStatus::Ok && message.status != BusStatus::Error)
            || !keys(data, {"event", "payload_encoding", "payload", "details"}, {}, error)
            || !data.value("event").isString() || !token(data.value("event").toString())
            || !metadata(data.value("details"))) { if (error.isEmpty()) error = "Invalid event record/metadata limit"; return false; }
        return true;
    }
    if (!busContext(data, error)) return false;
    if (message.kind == BusMessageKind::Request) {
        if (message.status != BusStatus::Pending
            || !keys(data, {"bus", "controller_id", "endpoint_ids", "net_ids", "phases", "bit_length",
                "bit_order", "mode", "chip_select", "read_length", "virtual_deadline_ns", "payload_encoding", "payload"},
                {"parameters"}, error)
            || !unsignedText(data.value("bit_length"), number) || number > quint64(payloadLimit) * 8 + 4096
            || !QStringList{"msb-first", "lsb-first"}.contains(data.value("bit_order").toString())
            || !(data.value("mode").isNull() || integer(data.value("mode"), 0, 3))
            || !(data.value("chip_select").isNull() || integer(data.value("chip_select"), 0, 5))
            || !integer(data.value("read_length"), 0, payloadLimit)
            || !unsignedText(data.value("virtual_deadline_ns"), number) || number < message.virtualTimeNs
            || !data.value("phases").isArray() || data.value("phases").toArray().isEmpty()
            || data.value("phases").toArray().size() > 64
            || (data.contains("parameters") && !metadata(data.value("parameters")))) {
            if (error.isEmpty()) error = "Invalid bus request fields/deadline"; return false;
        }
        static const QStringList kinds{"start", "address", "write", "read", "restart", "stop", "command", "dummy", "edge", "sample", "frame"};
        if (data.value("bus") == "spi" ? !integer(data.value("mode"), 0, 3)
            : (!data.value("mode").isNull() || !data.value("chip_select").isNull())) {
            error = "SPI mode is required; non-SPI mode/CS must be null"; return false;
        }
        for (const auto &entry : data.value("phases").toArray()) {
            const auto phase = entry.toObject();
            if (!entry.isObject() || !keys(phase, {"kind"}, {"length", "address", "direction", "bit_length", "level", "duration_ns"}, error)
                || !kinds.contains(phase.value("kind").toString())
                || (phase.contains("length") && !integer(phase.value("length"), 0, payloadLimit))
                || (phase.contains("address") && !integer(phase.value("address"), 0, 1023))
                || (phase.contains("direction") && !QStringList{"read", "write"}.contains(phase.value("direction").toString()))
                || (phase.contains("bit_length") && (!unsignedText(phase.value("bit_length"), number) || number > quint64(payloadLimit) * 8 + 4096))
                || (phase.contains("level") && !phase.value("level").isBool())
                || (phase.contains("duration_ns") && !unsignedText(phase.value("duration_ns"), number))) {
                if (error.isEmpty()) error = "Invalid bus phase"; return false;
            }
        }
        return true;
    }
    if (message.kind == BusMessageKind::Response) {
        if (message.status == BusStatus::Pending
            || !keys(data, {"bus", "controller_id", "endpoint_ids", "net_ids", "phase_statuses", "modeled_latency_ns",
                           "accepted_length", "payload_encoding", "payload", "error_message"}, {}, error)
            || !unsignedText(data.value("modeled_latency_ns"), number)
            || !integer(data.value("accepted_length"), 0, payloadLimit)
            || !data.value("phase_statuses").isArray() || data.value("phase_statuses").toArray().size() > 64
            || !data.value("error_message").isString() || data.value("error_message").toString().size() > 1024) {
            if (error.isEmpty()) error = "Invalid bus response"; return false;
        }
        QSet<int> seen;
        for (const auto &entry : data.value("phase_statuses").toArray()) {
            const auto phase = entry.toObject();
            if (!entry.isObject() || !keys(phase, {"index", "status"}, {}, error)
                || !integer(phase.value("index"), 0, 63)
                || !QStringList{"ack", "nack", "error", "timeout"}.contains(phase.value("status").toString())
                || seen.contains(phase.value("index").toInt())) {
                if (error.isEmpty()) error = "Invalid/duplicate phase result"; return false;
            }
            seen.insert(phase.value("index").toInt());
        }
        return true;
    }
    error = "Unknown message kind";
    return false;
}
QJsonObject json(const BusEnvelope &message)
{
    return {{"version", QJsonObject{{"major", message.majorVersion}, {"minor", message.minorVersion}}},
        {"kind", busMessageKindKey(message.kind)}, {"status", busStatusKey(message.status)},
        {"session_id", message.sessionId}, {"connection_id", message.connectionId},
        {"reset_epoch", QString::number(message.resetEpoch)}, {"topology_generation", QString::number(message.topologyGeneration)},
        {"sequence", QString::number(message.sequence)}, {"request_id", message.requestId},
        {"virtual_time_ns", QString::number(message.virtualTimeNs)}, {"data", message.data}};
}
QJsonObject limitJson(const PeripheralTransportLimits &limits)
{
    return {{"frame_bytes", int(limits.frameBytes)}, {"bus_payload_bytes", int(limits.busPayloadBytes)}, {"in_flight", int(limits.inFlight)}};
}
}

QString busMessageKindKey(BusMessageKind kind)
{
    switch (kind) {
    case BusMessageKind::Hello: return "hello";
    case BusMessageKind::HelloAck: return "hello_ack";
    case BusMessageKind::Request: return "request";
    case BusMessageKind::Response: return "response";
    case BusMessageKind::Event: return "event";
    case BusMessageKind::Reset: return "reset";
    }
    return "invalid";
}
QString busStatusKey(BusStatus status)
{
    switch (status) {
    case BusStatus::Ok: return "ok";
    case BusStatus::Pending: return "pending";
    case BusStatus::Nack: return "nack";
    case BusStatus::Error: return "error";
    case BusStatus::Timeout: return "timeout";
    case BusStatus::Unavailable: return "unavailable";
    }
    return "invalid";
}

PeripheralFrameCodec::PeripheralFrameCodec(quint32 frameBytes, quint32 payloadBytes)
    : frameLimit(frameBytes), payloadLimit(payloadBytes) {}

QByteArray PeripheralFrameCodec::encode(const BusEnvelope &message, QString &error, quint32 frameBytes, quint32 payloadBytes)
{
    const QByteArray body = QJsonDocument(json(message)).toJson(QJsonDocument::Compact);
    BusEnvelope validated;
    if (!decode(body, validated, error, frameBytes, payloadBytes)) return {};
    const quint32 length = quint32(body.size());
    QByteArray framed;
    for (int shift : {24, 16, 8, 0}) framed.append(char((length >> shift) & 0xff));
    framed += body;
    return framed;
}

bool PeripheralFrameCodec::decode(const QByteArray &bytes, BusEnvelope &message, QString &error,
                                  quint32 frameBytes, quint32 payloadBytes)
{
    error.clear();
    if (bytes.isEmpty() || bytes.size() > frameBytes || frameBytes > PeripheralTransportLimits::MaximumFrameBytes
        || payloadBytes > PeripheralTransportLimits::MaximumBusPayloadBytes) {
        error = "Empty frame or frame/payload limit exceeded"; return false;
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decodedText = utf8(bytes);
    Q_UNUSED(decodedText)
    if (utf8.hasError()) { error = "Frame is not valid UTF-8"; return false; }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = "Frame is not a JSON object: " + parseError.errorString(); return false;
    }
    qsizetype scanned = 0;
    if (!uniqueMembers(bytes, scanned, 0, error)) {
        if (error.isEmpty()) error = "Invalid JSON member structure";
        return false;
    }
    const auto object = document.object();
    if (!keys(object, {"version", "kind", "status", "session_id", "connection_id", "reset_epoch",
            "topology_generation", "sequence", "request_id", "virtual_time_ns", "data"}, {}, error)) return false;
    const auto version = object.value("version").toObject();
    if (!object.value("version").isObject() || !keys(version, {"major", "minor"}, {}, error)
        || !integer(version.value("major"), 1, 1) || !integer(version.value("minor"), 0, 65535)) {
        if (error.isEmpty()) error = "Unsupported major or invalid protocol version"; return false;
    }
    const QStringList kinds{"hello", "hello_ack", "request", "response", "event", "reset"};
    const QStringList statuses{"ok", "pending", "nack", "error", "timeout", "unavailable"};
    const int kind = kinds.indexOf(object.value("kind").toString());
    const int status = statuses.indexOf(object.value("status").toString());
    if (kind < 0 || status < 0 || !object.value("session_id").isString() || !uuid(object.value("session_id").toString())
        || !object.value("connection_id").isString() || !uuid(object.value("connection_id").toString())
        || !unsignedText(object.value("reset_epoch"), message.resetEpoch)
        || !unsignedText(object.value("topology_generation"), message.topologyGeneration)
        || !unsignedText(object.value("sequence"), message.sequence) || message.sequence == 0
        || !unsignedText(object.value("virtual_time_ns"), message.virtualTimeNs)
        || !object.value("request_id").isString() || !object.value("data").isObject()) {
        error = "Invalid envelope fields/uint64/identifier"; return false;
    }
    message.majorVersion = quint16(version.value("major").toInt());
    message.minorVersion = quint16(version.value("minor").toInt());
    message.kind = static_cast<BusMessageKind>(kind);
    message.status = static_cast<BusStatus>(status);
    message.sessionId = object.value("session_id").toString();
    message.connectionId = object.value("connection_id").toString();
    message.requestId = object.value("request_id").toString();
    message.data = object.value("data").toObject();
    quint64 request = 0;
    if (message.kind == BusMessageKind::Request || message.kind == BusMessageKind::Response) {
        if (!unsignedText(object.value("request_id"), request) || request == 0) { error = "Invalid request ordinal"; return false; }
    } else if (!message.requestId.isEmpty()) { error = "Control/events cannot carry request ordinals"; return false; }
    return validateData(message, payloadBytes, error);
}

bool PeripheralFrameCodec::feed(const QByteArray &bytes, const std::function<bool(const BusEnvelope &)> &receiver, QString &error)
{
    error.clear();
    qsizetype position = 0;
    while (position < bytes.size()) {
        if (!expectedBytes) {
            const qsizetype take = qMin(qsizetype(4 - prefix.size()), bytes.size() - position);
            prefix.append(bytes.constData() + position, take);
            position += take;
            if (prefix.size() != 4) continue;
            quint32 length = 0;
            for (char byte : prefix) length = (length << 8) | quint8(byte);
            prefix.clear();
            if (!length || length > frameLimit || frameLimit > PeripheralTransportLimits::MaximumFrameBytes) {
                reset(); error = "Invalid/oversized length prefix"; return false;
            }
            expectedBytes = length;
        }
        const qsizetype take = qMin(qsizetype(expectedBytes) - body.size(), bytes.size() - position);
        body.append(bytes.constData() + position, take);
        position += take;
        if (body.size() == expectedBytes) {
            BusEnvelope message;
            const bool valid = decode(body, message, error, frameLimit, payloadLimit);
            reset();
            if (!valid || !receiver(message)) return false;
        }
    }
    return true;
}
bool PeripheralFrameCodec::finish(QString &error) const
{
    error.clear();
    if (!prefix.isEmpty() || expectedBytes || !body.isEmpty()) { error = "Truncated frame at stream end"; return false; }
    return true;
}
bool PeripheralFrameCodec::framePending() const { return !prefix.isEmpty() || expectedBytes || !body.isEmpty(); }
void PeripheralFrameCodec::reset() { prefix.clear(); body.clear(); expectedBytes = 0; }
void PeripheralFrameCodec::setLimits(quint32 frameBytes, quint32 payloadBytes) { frameLimit = frameBytes; payloadLimit = payloadBytes; }
qsizetype PeripheralFrameCodec::bufferedBytes() const { return prefix.size() + body.size(); }

PeripheralTransport::PeripheralTransport(QObject *parent)
    : QObject(parent), server(new QTcpServer(this)), watchdog(new QTimer(this)),
      currentSession(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      supportedCapabilities{"bus.gpio.v1", "bus.i2c.v1", "bus.spi.v1", "control.reset-generation", "payload.base64"},
      requiredCapabilities{"control.reset-generation", "payload.base64"}
{
    qRegisterMetaType<BusEnvelope>();
    qRegisterMetaType<PeripheralTransportState>();
    monotonicClock.start();
    server->setMaxPendingConnections(1);
    connect(server, &QTcpServer::newConnection, this, [this]() {
        while (server->hasPendingConnections()) adoptPeer(server->nextPendingConnection());
    });
    watchdog->setInterval(20);
    connect(watchdog, &QTimer::timeout, this, &PeripheralTransport::checkWatchdog);
    watchdog->start();
}
PeripheralTransport::~PeripheralTransport()
{
    // Parent widgets may already be in base-class teardown; explicit close()
    // remains observable, destruction must not call their derived slots.
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    close();
}
bool PeripheralTransport::reject(const QString &reason, QString *error)
{
    if (error) *error = reason;
    emit messageRejected(reason);
    return false;
}
void PeripheralTransport::setState(PeripheralTransportState state, const QString &reason)
{
    currentState = state;
    emit stateChanged(state, reason);
}
bool PeripheralTransport::setLimits(const PeripheralTransportLimits &limits, QString *error)
{
    if (peer) return reject("Limits cannot change during an active connection", error);
    if (limits.frameBytes < 512 || limits.frameBytes > PeripheralTransportLimits::MaximumFrameBytes
        || limits.busPayloadBytes == 0 || limits.busPayloadBytes > PeripheralTransportLimits::MaximumBusPayloadBytes
        || !limits.inFlight || limits.inFlight > PeripheralTransportLimits::MaximumInFlight
        || limits.queuedBytes < 512 || limits.queuedBytes > 4 * 1024 * 1024
        || !limits.discardedMessageLimit || limits.discardedMessageLimit > 256
        || limits.hostWatchdogMs < 1 || limits.hostWatchdogMs > 600000) return reject("Invalid transport limits", error);
    QString why;
    if (!configurationFits(limits, supportedCapabilities, requiredCapabilities, why)) return reject(why, error);
    localLimits = agreedLimits = limits;
    codec.setLimits(limits.frameBytes, limits.busPayloadBytes);
    return true;
}
bool PeripheralTransport::setCapabilities(const QStringList &supported, const QStringList &required, QString *error)
{
    if (peer) return reject("Capabilities cannot change during an active connection", error);
    QString why;
    if (!tokens(QJsonArray::fromStringList(supported), 0, 32, true, why)
        || !tokens(QJsonArray::fromStringList(required), 0, 32, true, why)) return reject(why, error);
    for (const QString &item : required) if (!supported.contains(item)) return reject("Required capability is not supported locally", error);
    for (const QString &core : {QStringLiteral("control.reset-generation"), QStringLiteral("payload.base64")})
        if (!supported.contains(core) || !required.contains(core)) return reject("Core reset/base64 capabilities must remain required", error);
    if (!configurationFits(localLimits, supported, required, why)) return reject(why, error);
    supportedCapabilities = supported; requiredCapabilities = required;
    return true;
}
bool PeripheralTransport::beginSession(const QString &sessionId, quint64 epoch, quint64 topology, QString *error)
{
    if (resetTransaction) return reject("Reset transaction is active; retry after generationChanged", error);
    if (!uuid(sessionId)) return reject("Session must be a canonical nonzero UUID", error);
    QString why;
    if (!configurationFits(localLimits, supportedCapabilities, requiredCapabilities, why)) return reject(why, error);
    detachPeer("Session replaced");
    currentSession = sessionId; currentEpoch = epoch; currentTopology = topology;
    setState(PeripheralTransportState::Disconnected, "Session configured; no device peer connected");
    return true;
}
bool PeripheralTransport::listen(quint16 requestedPort, QString *error)
{
    if (server->isListening()) return reject("Endpoint is already listening", error);
    QString why;
    if (!configurationFits(localLimits, supportedCapabilities, requiredCapabilities, why)) return reject(why, error);
    if (!server->listen(QHostAddress::LocalHost, requestedPort)) return reject(server->errorString(), error);
    return true;
}
bool PeripheralTransport::configurationFits(const PeripheralTransportLimits &limits, const QStringList &supported,
                                           const QStringList &required, QString &error) const
{
    BusEnvelope hello;
    hello.kind = BusMessageKind::Hello;
    hello.sessionId = "11111111-2222-3333-4444-555555555555";
    hello.connectionId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    hello.sequence = 1;
    hello.resetEpoch = hello.topologyGeneration = std::numeric_limits<quint64>::max();
    hello.data = {{"capabilities", QJsonArray::fromStringList(supported)},
                  {"required_capabilities", QJsonArray::fromStringList(required)}, {"limits", limitJson(limits)}};
    BusEnvelope reset = hello;
    reset.kind = BusMessageKind::Reset;
    reset.sequence = reset.virtualTimeNs = std::numeric_limits<quint64>::max();
    reset.data = {{"reason", "Reset/topology generation changed"}};
    const qsizetype bytes = qMax(QJsonDocument(json(hello)).toJson(QJsonDocument::Compact).size(),
                                QJsonDocument(json(reset)).toJson(QJsonDocument::Compact).size());
    if (bytes > limits.frameBytes || bytes + 4 > limits.queuedBytes) {
        error = QString("Handshake/reset requires at least %1 body bytes and %2 queued bytes for these capabilities and uint64 counters")
            .arg(bytes).arg(bytes + 4);
        return false;
    }
    return true;
}
void PeripheralTransport::close()
{
    server->close();
    detachPeer("Transport closed");
    setState(PeripheralTransportState::Disconnected, "Transport closed");
}
quint16 PeripheralTransport::port() const { return server->serverPort(); }
PeripheralTransportState PeripheralTransport::state() const { return currentState; }
QString PeripheralTransport::connectionId() const { return currentConnection; }
QStringList PeripheralTransport::negotiatedCapabilities() const { return agreedCapabilities; }
PeripheralTransportLimits PeripheralTransport::negotiatedLimits() const { return agreedLimits; }
int PeripheralTransport::inFlightCount() const { return outgoingRequests.size() + incomingRequests.size(); }
qint64 PeripheralTransport::queuedByteCount() const { return pendingWriteBytes + (peer ? peer->bytesToWrite() : 0); }

BusEnvelope PeripheralTransport::envelope(BusMessageKind kind, BusStatus status, quint64 virtualTimeNs) const
{
    BusEnvelope record;
    record.kind = kind; record.status = status; record.sessionId = currentSession; record.connectionId = currentConnection;
    record.resetEpoch = currentEpoch; record.topologyGeneration = currentTopology; record.virtualTimeNs = virtualTimeNs;
    return record;
}
void PeripheralTransport::adoptPeer(QTcpSocket *socket)
{
    if (peer) { socket->abort(); socket->deleteLater(); emit messageRejected("Only one device peer is allowed"); return; }
    peer = socket;
    peer->setReadBufferSize(localLimits.frameBytes + 4);
    currentConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transmitSequence = receiveSequence = nextRequestOrdinal = lastPeerRequestOrdinal = 0;
    consecutiveDiscarded = 0;
    agreedCapabilities.clear(); agreedLimits = localLimits;
    codec.reset(); codec.setLimits(localLimits.frameBytes, localLimits.busPayloadBytes);
    assemblySinceMs = -1;
    handshakeSinceMs = monotonicClock.elapsed();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { if (peer == socket) consumeInput(); });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64) { if (peer == socket) pumpWrites(); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (peer != socket) return;
        QString error;
        if (!codec.finish(error)) { fail(error); return; }
        detachPeer("Device peer disconnected");
        setState(PeripheralTransportState::Disconnected, "Device peer disconnected");
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        if (peer == socket && error != QAbstractSocket::RemoteHostClosedError) fail("Device socket error: " + socket->errorString());
    });
    setState(PeripheralTransportState::Handshaking, "Negotiating a dedicated device channel");
    auto hello = envelope(BusMessageKind::Hello, BusStatus::Ok, 0);
    hello.data = {{"capabilities", QJsonArray::fromStringList(supportedCapabilities)},
                  {"required_capabilities", QJsonArray::fromStringList(requiredCapabilities)}, {"limits", limitJson(localLimits)}};
    QString error;
    if (!enqueue(hello, &error)) fail("Unable to queue handshake: " + error);
    else { hello.sequence = transmitSequence; handshakeEnvelope = hello; }
}
bool PeripheralTransport::enqueue(BusEnvelope record, QString *error)
{
    if (!peer || peer->state() != QAbstractSocket::ConnectedState) return reject("No connected device peer", error);
    if (transmitSequence == std::numeric_limits<quint64>::max()) return reject("Sequence exhausted; start a new connection", error);
    record.sequence = transmitSequence + 1;
    QString why;
    const QByteArray frame = PeripheralFrameCodec::encode(record, why, agreedLimits.frameBytes, agreedLimits.busPayloadBytes);
    if (frame.isEmpty()) return reject(why, error);
    if (queuedByteCount() + frame.size() > localLimits.queuedBytes) return reject("Outbound byte queue limit exceeded", error);
    ++transmitSequence;
    writeQueue.enqueue(frame); pendingWriteBytes += frame.size();
    QTimer::singleShot(0, this, &PeripheralTransport::pumpWrites);
    return true;
}
void PeripheralTransport::pumpWrites()
{
    while (peer && peer->state() == QAbstractSocket::ConnectedState && !writeQueue.isEmpty()) {
        const QByteArray frame = writeQueue.head();
        if (peer->bytesToWrite() + frame.size() > localLimits.queuedBytes) return;
        if (peer->write(frame) != frame.size()) { fail("Device socket could not accept a complete frame"); return; }
        writeQueue.dequeue(); pendingWriteBytes -= frame.size();
    }
}
void PeripheralTransport::consumeInput()
{
    for (int batch = 0; peer && peer->bytesAvailable() && batch < 4; ++batch) {
        const QByteArray bytes = peer->read(64 * 1024);
        QString error;
        if (!codec.framePending()) assemblySinceMs = monotonicClock.elapsed();
        if (!codec.feed(bytes, [this](const BusEnvelope &record) {
            assemblySinceMs = -1;
            return dispatch(record);
        }, error)) {
            if (!error.isEmpty() && peer) fail(error);
            return;
        }
        if (codec.framePending()) {
            if (assemblySinceMs < 0) assemblySinceMs = monotonicClock.elapsed();
        } else assemblySinceMs = -1;
    }
    if (peer && peer->bytesAvailable()) QTimer::singleShot(0, this, &PeripheralTransport::consumeInput);
}
bool PeripheralTransport::negotiate(const BusEnvelope &ack)
{
    if (ack.kind != BusMessageKind::HelloAck || ack.minorVersion != 0 || ack.sequence != 1) {
        fail("Expected hello_ack with supported minor version 0 and sequence 1"); return false;
    }
    const auto capabilities = strings(ack.data.value("capabilities").toArray());
    for (const QString &item : capabilities) if (!supportedCapabilities.contains(item)) {
        fail("Peer selected an unoffered capability"); return false;
    }
    for (const QString &required : requiredCapabilities) if (!capabilities.contains(required)) {
        fail("Peer omitted required capability: " + required); return false;
    }
    const auto limits = ack.data.value("limits").toObject();
    if (limits.value("frame_bytes").toInt() > int(localLimits.frameBytes)
        || limits.value("bus_payload_bytes").toInt() > int(localLimits.busPayloadBytes)
        || limits.value("in_flight").toInt() > int(localLimits.inFlight)) {
        fail("Peer increased an offered limit"); return false;
    }
    auto proposedLimits = agreedLimits;
    proposedLimits.frameBytes = quint32(limits.value("frame_bytes").toInt());
    proposedLimits.busPayloadBytes = quint32(limits.value("bus_payload_bytes").toInt());
    proposedLimits.inFlight = quint32(limits.value("in_flight").toInt());
    QString configurationError;
    if (!configurationFits(proposedLimits, capabilities, requiredCapabilities, configurationError)) {
        fail(configurationError); return false;
    }
    agreedLimits = proposedLimits;
    agreedCapabilities = capabilities;
    codec.setLimits(agreedLimits.frameBytes, agreedLimits.busPayloadBytes);
    setState(PeripheralTransportState::Ready, "Dedicated device channel negotiated; guest barrier integration is not implemented");
    emit ready(agreedCapabilities);
    return peer != nullptr;
}
void PeripheralTransport::discard(const BusEnvelope &record, const QString &reason)
{
    emit messageDiscarded(record.requestId, reason);
    if (++consecutiveDiscarded >= localLimits.discardedMessageLimit) fail("Discarded-message limit exceeded: " + reason);
}
bool PeripheralTransport::dispatch(const BusEnvelope &record)
{
    if (record.sessionId != currentSession || record.connectionId != currentConnection) {
        discard(record, "Old/unknown session or connection"); return peer != nullptr;
    }
    if (record.sequence <= receiveSequence) { discard(record, "Duplicate/out-of-order sequence"); return peer != nullptr; }
    receiveSequence = record.sequence;
    if (record.resetEpoch < currentEpoch || record.topologyGeneration < currentTopology) {
        discard(record, "Old reset epoch or topology generation"); return peer != nullptr;
    }
    if (record.resetEpoch != currentEpoch || record.topologyGeneration != currentTopology) {
        fail("Unexpected future generation; only the host issues reset records"); return false;
    }
    if (currentState == PeripheralTransportState::Handshaking) return negotiate(record);
    if (currentState != PeripheralTransportState::Ready) { fail("Bus traffic before negotiation"); return false; }
    if (record.minorVersion != 0) { fail("Message minor version differs from negotiation"); return false; }
    if (record.kind == BusMessageKind::Response) {
        if (!outgoingRequests.contains(record.requestId)) { discard(record, "Unknown/completed request ordinal"); return peer != nullptr; }
        QString error;
        if (!responseMatches(record, outgoingRequests.value(record.requestId).envelope, error)) { fail(error); return false; }
        outgoingRequests.remove(record.requestId); consecutiveDiscarded = 0;
        emit responseReceived(record);
    } else if (record.kind == BusMessageKind::Request) {
        const quint64 ordinal = record.requestId.toULongLong();
        if (ordinal <= lastPeerRequestOrdinal) { discard(record, "Duplicate/out-of-order request ordinal"); return peer != nullptr; }
        if (!agreedCapabilities.contains("bus." + record.data.value("bus").toString() + ".v1")) {
            fail("Peer used an unnegotiated bus capability"); return false;
        }
        if (inFlightCount() >= int(agreedLimits.inFlight)) { fail("Peer exceeded in-flight request limit"); return false; }
        lastPeerRequestOrdinal = ordinal;
        incomingRequests.insert(record.requestId, {record, monotonicClock.elapsed()});
        consecutiveDiscarded = 0;
        emit requestReceived(record);
    } else if (record.kind == BusMessageKind::Event) {
        consecutiveDiscarded = 0;
        emit eventReceived(record);
    } else { fail("Unexpected control record after negotiation"); return false; }
    return peer != nullptr;
}
bool PeripheralTransport::responseMatches(const BusEnvelope &response, const BusEnvelope &request, QString &error) const
{
    for (const QString &name : {QStringLiteral("bus"), QStringLiteral("controller_id"), QStringLiteral("endpoint_ids"), QStringLiteral("net_ids")}) {
        if (response.data.value(name) != request.data.value(name)) { error = "Response changed request topology/context"; return false; }
    }
    const QByteArray transmit = QByteArray::fromBase64(request.data.value("payload").toString().toUtf8());
    const QByteArray receive = QByteArray::fromBase64(response.data.value("payload").toString().toUtf8());
    const quint64 deadline = request.data.value("virtual_deadline_ns").toString().toULongLong();
    const quint64 latency = response.data.value("modeled_latency_ns").toString().toULongLong();
    if (response.virtualTimeNs < request.virtualTimeNs || latency != response.virtualTimeNs - request.virtualTimeNs
        || (response.status == BusStatus::Timeout ? response.virtualTimeNs < deadline : response.virtualTimeNs > deadline)
        || response.data.value("accepted_length").toInt() > transmit.size()
        || receive.size() > request.data.value("read_length").toInt()) {
        error = "Response violates virtual completion/deadline or accepted/RX lengths"; return false;
    }
    for (const auto &phase : response.data.value("phase_statuses").toArray()) {
        if (phase.toObject().value("index").toInt() >= request.data.value("phases").toArray().size()) {
            error = "Response references a nonexistent phase"; return false;
        }
    }
    return true;
}
QString PeripheralTransport::sendRequest(const QJsonObject &data, quint64 virtualTimeNs, QString *error)
{
    if (resetTransaction) { reject("Reset transaction is active; retry after generationChanged", error); return {}; }
    if (currentState != PeripheralTransportState::Ready) { reject("Device channel is not negotiated", error); return {}; }
    if (inFlightCount() >= int(agreedLimits.inFlight)) { reject("In-flight request limit exceeded", error); return {}; }
    if (!agreedCapabilities.contains("bus." + data.value("bus").toString() + ".v1")) { reject("Bus capability was not negotiated", error); return {}; }
    if (nextRequestOrdinal == std::numeric_limits<quint64>::max()) { reject("Request ordinals exhausted", error); return {}; }
    auto record = envelope(BusMessageKind::Request, BusStatus::Pending, virtualTimeNs);
    record.requestId = QString::number(nextRequestOrdinal + 1); record.data = data;
    if (!enqueue(record, error)) return {};
    record.sequence = transmitSequence;
    ++nextRequestOrdinal;
    outgoingRequests.insert(record.requestId, {record, monotonicClock.elapsed()});
    return record.requestId;
}
bool PeripheralTransport::sendResponse(const BusEnvelope &originalRequest, const QJsonObject &data, BusStatus responseStatus,
                                       quint64 virtualCompletionNs, QString *error)
{
    if (resetTransaction) return reject("Reset transaction is active; retry after generationChanged", error);
    const QString requestId = originalRequest.requestId;
    if (currentState != PeripheralTransportState::Ready || !incomingRequests.contains(requestId)) return reject("Unknown active incoming request", error);
    const auto &active = incomingRequests.value(requestId).envelope;
    if (originalRequest.kind != BusMessageKind::Request || originalRequest.status != BusStatus::Pending
        || originalRequest.majorVersion != active.majorVersion || originalRequest.minorVersion != active.minorVersion
        || originalRequest.sessionId != active.sessionId || originalRequest.connectionId != active.connectionId
        || originalRequest.resetEpoch != active.resetEpoch || originalRequest.topologyGeneration != active.topologyGeneration
        || originalRequest.sequence != active.sequence || originalRequest.requestId != active.requestId
        || originalRequest.virtualTimeNs != active.virtualTimeNs || originalRequest.data != active.data)
        return reject("Original request context is no longer active or was modified", error);
    auto record = envelope(BusMessageKind::Response, responseStatus, virtualCompletionNs);
    record.requestId = requestId; record.data = data;
    QString why;
    if (!responseMatches(record, incomingRequests.value(requestId).envelope, why)) return reject(why, error);
    if (!enqueue(record, error)) return false;
    incomingRequests.remove(requestId);
    return true;
}
bool PeripheralTransport::sendEvent(const QJsonObject &data, quint64 virtualTimeNs, QString *error)
{
    if (resetTransaction) return reject("Reset transaction is active; retry after generationChanged", error);
    if (currentState != PeripheralTransportState::Ready) return reject("Device channel is not negotiated", error);
    auto record = envelope(BusMessageKind::Event, BusStatus::Ok, virtualTimeNs); record.data = data;
    return enqueue(record, error);
}
bool PeripheralTransport::resetGeneration(quint64 epoch, quint64 topology, quint64 virtualTimeNs, QString *error)
{
    if (resetTransaction) return reject("A reset transaction is already active", error);
    if (epoch < currentEpoch || topology < currentTopology || (epoch == currentEpoch && topology == currentTopology))
        return reject("Reset/topology generations must advance monotonically", error);
    QString configurationError;
    if (!configurationFits(localLimits, supportedCapabilities, requiredCapabilities, configurationError))
        return reject(configurationError, error);
    resetTransaction = true;
    const auto outgoing = outgoingRequests;
    const auto incoming = incomingRequests;
    outgoingRequests.clear(); incomingRequests.clear();
    currentEpoch = epoch; currentTopology = topology;
    writeQueue.clear(); pendingWriteBytes = 0;
    consecutiveDiscarded = 0;
    bool delivered = true;
    if (currentState == PeripheralTransportState::Handshaking) {
        detachPeer("Generation changed during handshake; reconnect required");
        setState(PeripheralTransportState::Disconnected, "Generation changed during handshake; reconnect required");
    } else if (currentState == PeripheralTransportState::Ready) {
        auto record = envelope(BusMessageKind::Reset, BusStatus::Ok, virtualTimeNs);
        record.data = {{"reason", "Reset/topology generation changed"}};
        QString why;
        if (!enqueue(record, &why)) {
            if (error) *error = why;
            delivered = false;
            fail("Reset record could not be delivered: " + why);
        }
    }
    // Cancellation slots cannot create work while old-generation queues/maps
    // are being retired. The Reset is already queued before any retry is exposed.
    auto ids = outgoing.keys(); ids.sort();
    for (const auto &id : ids) emit requestCancelled(id, true, "Reset/topology generation changed");
    ids = incoming.keys(); ids.sort();
    for (const auto &id : ids) emit requestCancelled(id, false, "Reset/topology generation changed");
    resetTransaction = false;
    emit generationChanged(epoch, topology);
    return delivered;
}
void PeripheralTransport::cancelRequests(const QString &reason)
{
    const auto outgoing = outgoingRequests;
    const auto incoming = incomingRequests;
    outgoingRequests.clear(); incomingRequests.clear();
    auto ids = outgoing.keys(); ids.sort();
    for (const auto &id : ids) emit requestCancelled(id, true, reason);
    ids = incoming.keys(); ids.sort();
    for (const auto &id : ids) emit requestCancelled(id, false, reason);
}
void PeripheralTransport::detachPeer(const QString &reason)
{
    QTcpSocket *old = peer;
    peer = nullptr;
    cancelRequests(reason);
    writeQueue.clear(); pendingWriteBytes = 0;
    codec.reset(); agreedCapabilities.clear();
    assemblySinceMs = -1;
    if (old) { old->abort(); old->deleteLater(); }
}
void PeripheralTransport::fail(const QString &reason)
{
    setState(PeripheralTransportState::Error, reason);
    emit protocolError(reason);
    detachPeer(reason);
}
void PeripheralTransport::checkWatchdog()
{
    if (!peer) return;
    const qint64 now = monotonicClock.elapsed();
    if (codec.framePending() && assemblySinceMs >= 0 && now - assemblySinceMs >= localLimits.hostWatchdogMs) {
        fail("Incomplete device frame assembly watchdog expired; no guest timeout was generated");
        return;
    }
    if (currentState == PeripheralTransportState::Handshaking && now - handshakeSinceMs >= localLimits.hostWatchdogMs) {
        setState(PeripheralTransportState::Stalled, "Host device handshake watchdog expired");
        emit hostServiceStalled(handshakeEnvelope, true, "Host device handshake watchdog expired; no guest timeout was generated");
        detachPeer("Host handshake stalled");
        return;
    }
    if (currentState != PeripheralTransportState::Ready) return;
    bool found = false, outgoing = false;
    PendingRequest oldest;
    for (const auto &request : outgoingRequests)
        if (!found || request.sinceMs < oldest.sinceMs || (request.sinceMs == oldest.sinceMs
            && request.envelope.requestId.toULongLong() < oldest.envelope.requestId.toULongLong())) {
            oldest = request; found = outgoing = true;
        }
    for (const auto &request : incomingRequests)
        if (!found || request.sinceMs < oldest.sinceMs || (request.sinceMs == oldest.sinceMs
            && request.envelope.requestId.toULongLong() < oldest.envelope.requestId.toULongLong())) {
            oldest = request; found = true; outgoing = false;
        }
    if (found && now - oldest.sinceMs >= localLimits.hostWatchdogMs) {
        const QString reason = "Host device response watchdog expired; simulation barrier handling is pending and no guest timeout was generated";
        setState(PeripheralTransportState::Stalled, reason);
        emit hostServiceStalled(oldest.envelope, outgoing, reason);
        detachPeer(reason);
    }
}
