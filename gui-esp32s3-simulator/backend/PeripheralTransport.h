#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <functional>

class QTcpServer;
class QTcpSocket;
class QTimer;

enum class BusMessageKind { Hello, HelloAck, Request, Response, Event, Reset };
enum class BusStatus { Ok, Pending, Nack, Error, Timeout, Unavailable };
enum class PeripheralTransportState { Disconnected, Handshaking, Ready, Stalled, Error };

struct BusEnvelope {
    quint16 majorVersion = 1;
    quint16 minorVersion = 0;
    BusMessageKind kind = BusMessageKind::Event;
    QString sessionId;
    QString connectionId;
    quint64 resetEpoch = 0;
    quint64 topologyGeneration = 0;
    quint64 sequence = 0;
    QString requestId;
    quint64 virtualTimeNs = 0;
    BusStatus status = BusStatus::Ok;
    QJsonObject data;
};

struct PeripheralTransportLimits {
    static constexpr quint32 MaximumFrameBytes = 1024 * 1024;
    static constexpr quint32 MaximumBusPayloadBytes = 64 * 1024;
    static constexpr quint32 MaximumInFlight = 64;
    quint32 frameBytes = MaximumFrameBytes;
    quint32 busPayloadBytes = MaximumBusPayloadBytes;
    quint32 inFlight = MaximumInFlight;
    quint32 queuedBytes = 4 * 1024 * 1024;
    quint32 discardedMessageLimit = 256;
    int hostWatchdogMs = 5000;
};

QString busMessageKindKey(BusMessageKind kind);
QString busStatusKey(BusStatus status);

// Incremental network-order uint32 length + UTF-8 JSON codec. Its internal
// storage never exceeds one bounded frame plus four prefix bytes.
class PeripheralFrameCodec {
public:
    explicit PeripheralFrameCodec(quint32 frameBytes = PeripheralTransportLimits::MaximumFrameBytes,
                                  quint32 payloadBytes = PeripheralTransportLimits::MaximumBusPayloadBytes);
    static QByteArray encode(const BusEnvelope &envelope, QString &error,
                             quint32 frameBytes = PeripheralTransportLimits::MaximumFrameBytes,
                             quint32 payloadBytes = PeripheralTransportLimits::MaximumBusPayloadBytes);
    static bool decode(const QByteArray &json, BusEnvelope &envelope, QString &error,
                       quint32 frameBytes = PeripheralTransportLimits::MaximumFrameBytes,
                       quint32 payloadBytes = PeripheralTransportLimits::MaximumBusPayloadBytes);
    bool feed(const QByteArray &bytes, const std::function<bool(const BusEnvelope &)> &receiver,
              QString &error);
    bool finish(QString &error) const;
    bool framePending() const;
    void reset();
    void setLimits(quint32 frameBytes, quint32 payloadBytes);
    qsizetype bufferedBytes() const;
private:
    quint32 frameLimit;
    quint32 payloadLimit;
    quint32 expectedBytes = 0;
    QByteArray prefix;
    QByteArray body;
};

// Host-side, loopback-only endpoint. The host sends hello, a single peer sends
// hello_ack. This has no QEMU/UART integration and does not advance guest time.
class PeripheralTransport : public QObject {
    Q_OBJECT
public:
    explicit PeripheralTransport(QObject *parent = nullptr);
    ~PeripheralTransport() override;
    bool setLimits(const PeripheralTransportLimits &limits, QString *error = nullptr);
    bool setCapabilities(const QStringList &supported, const QStringList &required,
                         QString *error = nullptr);
    bool beginSession(const QString &sessionId, quint64 resetEpoch = 0,
                      quint64 topologyGeneration = 0, QString *error = nullptr);
    bool listen(quint16 port = 0, QString *error = nullptr);
    void close();
    quint16 port() const;
    PeripheralTransportState state() const;
    QString connectionId() const;
    QStringList negotiatedCapabilities() const;
    PeripheralTransportLimits negotiatedLimits() const;
    int inFlightCount() const;
    qint64 queuedByteCount() const;

    // Data shapes are strictly validated by the codec and documented schema.
    QString sendRequest(const QJsonObject &data, quint64 virtualTimeNs, QString *error = nullptr);
    bool sendResponse(const BusEnvelope &originalRequest, const QJsonObject &data, BusStatus status,
                      quint64 virtualCompletionNs, QString *error = nullptr);
    bool sendEvent(const QJsonObject &data, quint64 virtualTimeNs, QString *error = nullptr);
    bool resetGeneration(quint64 resetEpoch, quint64 topologyGeneration,
                         quint64 virtualTimeNs, QString *error = nullptr);

signals:
    void stateChanged(PeripheralTransportState state, const QString &reason);
    void ready(const QStringList &capabilities);
    void requestReceived(const BusEnvelope &request);
    void responseReceived(const BusEnvelope &response);
    void eventReceived(const BusEnvelope &event);
    void generationChanged(quint64 resetEpoch, quint64 topologyGeneration);
    void messageRejected(const QString &reason);
    void messageDiscarded(const QString &requestId, const QString &reason);
    void protocolError(const QString &reason);
    void requestCancelled(const QString &requestId, bool outgoing, const QString &reason);
    // Host wall-clock failure only. Never turned into a guest Timeout response.
    void hostServiceStalled(const BusEnvelope &request, bool outgoing, const QString &reason);

private:
    struct PendingRequest { BusEnvelope envelope; qint64 sinceMs = 0; };
    void adoptPeer(QTcpSocket *socket);
    void consumeInput();
    bool dispatch(const BusEnvelope &envelope);
    bool negotiate(const BusEnvelope &ack);
    BusEnvelope envelope(BusMessageKind kind, BusStatus status, quint64 virtualTimeNs) const;
    bool enqueue(BusEnvelope envelope, QString *error = nullptr);
    void pumpWrites();
    bool reject(const QString &reason, QString *error);
    void discard(const BusEnvelope &envelope, const QString &reason);
    void fail(const QString &reason);
    void detachPeer(const QString &reason);
    void cancelRequests(const QString &reason);
    void setState(PeripheralTransportState state, const QString &reason);
    void checkWatchdog();
    bool responseMatches(const BusEnvelope &response, const BusEnvelope &request, QString &error) const;
    bool configurationFits(const PeripheralTransportLimits &limits, const QStringList &supported,
                           const QStringList &required, QString &error) const;

    QTcpServer *server;
    QTcpSocket *peer = nullptr;
    QTimer *watchdog;
    QElapsedTimer monotonicClock;
    PeripheralFrameCodec codec;
    PeripheralTransportLimits localLimits;
    PeripheralTransportLimits agreedLimits;
    PeripheralTransportState currentState = PeripheralTransportState::Disconnected;
    QString currentSession;
    QString currentConnection;
    quint64 currentEpoch = 0;
    quint64 currentTopology = 0;
    quint64 transmitSequence = 0;
    quint64 receiveSequence = 0;
    quint64 nextRequestOrdinal = 0;
    quint64 lastPeerRequestOrdinal = 0;
    QStringList supportedCapabilities;
    QStringList requiredCapabilities;
    QStringList agreedCapabilities;
    QHash<QString, PendingRequest> outgoingRequests;
    QHash<QString, PendingRequest> incomingRequests;
    QQueue<QByteArray> writeQueue;
    qint64 pendingWriteBytes = 0;
    qint64 handshakeSinceMs = 0;
    qint64 assemblySinceMs = -1;
    BusEnvelope handshakeEnvelope;
    quint32 consecutiveDiscarded = 0;
    bool resetTransaction = false;
};

Q_DECLARE_METATYPE(BusEnvelope)
Q_DECLARE_METATYPE(PeripheralTransportState)
