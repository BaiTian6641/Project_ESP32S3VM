#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QString>
#include <QSet>
#include <QHash>
#include <QElapsedTimer>
#include "RuntimeContract.h"

class QProcess;
class QTcpSocket;
class QTimer;
class QSocketNotifier;

class QemuController : public QObject
{
    Q_OBJECT

public:
    explicit QemuController(QObject *parent = nullptr);
    ~QemuController() override;

    void sendUart0(const QString &text);
    void setSerialConfig(const QString &communicationType,
                         int baudRate,
                         int dataBits,
                         const QString &parity,
                         int stopBits,
                         const QString &flowControl,
                         const QString &lineEnding);
    void requestCpuSnapshot();
    void startLiveUpdates(bool enabled);
    void setMemoryInspectBase(const QString &addressText);
    void handleBridgeResponse(const QString &busKind, const QJsonObject &payload);
    bool ingestBridgeEventLine(const QString &line);

    /* I2C bridge address management (dynamic device registration) */
    void registerI2cBridgeAddress(int busIndex, const QString &hexAddr);
    void unregisterI2cBridgeAddress(int busIndex, const QString &hexAddr);
    void clearAllI2cBridgeAddresses();
    void setI2cBridgeResponseMap(int busIndex, const QString &mapStr);

    /* SPI bridge configuration */
    void setSpiDcGpio(const QString &controller, int gpioNum);

    void pauseExecution();
    void continueExecution();
    void stepInstruction();
    void addBreakpoint(const QString &addressText);
    void clearBreakpoints();
    void stopSimulation();
    RuntimeStatus runtimeStatus() const;
    QList<RuntimeCapability> runtimeCapabilities() const;

    bool nativeCircuitAvailable() const;
    QString nativeCircuitUnavailableReason() const;
    QString nativeCircuitApplyUnavailableReason() const;
    bool applyNativeCircuit(const QJsonObject &document);
    void requestNativeCircuitSnapshot();

    // Test seam: exercise the real QMP parser/correlation against a local server.
    // It does not launch firmware or make peripheral support claims.
    void attachQmpEndpointForTesting(quint16 port);

    void setGdbServerConfig(bool enabled, int port, bool waitForAttach);
    void startWithGdb(const QString &firmwarePath, int port, bool waitForAttach);
    void setSpiFlashConfig(bool enabled, int sizeMB);
    void setPsramConfig(bool enabled, int sizeMB, const QString &mode);
    void setChipIdentityConfig(const QString &baseMac, bool chipRevisionEnabled, int chipRevision);
    QString currentUartPort() const;
    QString recommendedEsptoolCommand(const QString &firmwarePath) const;

    void resetTarget();
    void setBootMode(int modeIndex);
    void loadFirmware(const QString &path);

signals:
    void runtimeStatusChanged(const RuntimeStatus &status);
    void runtimeCapabilitiesChanged(const QList<RuntimeCapability> &capabilities);
    void peripheralBridgeAvailable(bool available);
    void nativeCircuitAvailableChanged(bool available, const QString &reason);
    void nativeCircuitApplyInFlightChanged(bool inFlight, const QString &result);
    void nativeCircuitSnapshotUpdated(const QJsonObject &snapshot, const QJsonObject &acceptedDocument);
    void nativeCircuitSnapshotUnavailable(const QString &reason);
    void qemuStarted();
    void qemuStopped();
    void i2cTransferRequested(const QJsonObject &request);
    void spiTransferRequested(const QJsonObject &request);
    void uartTxRequested(const QJsonObject &request);
    void serialLineReceived(const QString &line);
    void debugMessageReceived(const QString &line);
    void cpuSnapshotUpdated(const QString &pc,
                            const QStringList &scalarRegs,
                            const QStringList &vectorRegs,
                            const QStringList &memoryWords);
    void debugStatusUpdated(const QString &status);
    void gdbAttachCommandUpdated(const QString &command);

private:
    struct PendingQmpCommand;
    bool containsDownloadSyncPreamble(const QByteArray &bytes);
    void startQemuWithFirmware(const QString &firmwarePath, bool preserveSession = false);
    void stopQemu();
    QString resolveQemuBinary() const;
    void handleQemuOutputChunk(const QString &chunk);
    bool prepareSpiFlashImage(const QString &firmwarePath, QString &flashPathOut);
    bool setupUartPty();
    void teardownUartPty();
    void flushUartBridgeBuffers();

    void connectQmp();
    void disconnectQmp();
    void processQmpBuffer();
    void handleQmpMessage(const QJsonObject &obj);
    void sendQmpCommand(const QString &execute,
                        const QJsonObject &arguments = QJsonObject(),
                        int callbackId = -1);
    void pushI2cBridgeAddresses(int busIndex);
    void pushAllI2cBridgeAddresses();
    void setRuntimePhase(RuntimePhase phase, const QString &message);
    void updateRuntimeCapabilities();
    void failRuntime(const QString &message, bool stopProcess = false);
    void requestRuntimeStatus();
    void finishRuntimeInitialization();
    void startBridgeProbes();
    bool qmpTransportExpected() const;
    void clearNativeCircuit(bool clearSupport, bool clearAcceptedDocument, const QString &reason);
    void finishNativeCircuitDiscovery();
    bool handleNativeCircuitReply(int id, const PendingQmpCommand &command, const QJsonObject &reply);
    void pollLiveState();
    QString resolveQemuDataDir() const;
    void parseRegisterDump(const QString &dump,
                           QString &pcText,
                           QStringList &scalars,
                           QStringList &vectors) const;
    QStringList parseMemoryDump(const QString &dump) const;

    QProcess *qemuProcess;
    QTcpSocket *qmpTcpSocket;
    QTimer *liveTimer;
    quint16 qmpPort;
    int bootMode;
    QString pendingFirmware;
    QString serialBuffer;
    QString qmpBuffer;
    QString qemuBinaryPath;
    QString memoryInspectBase;
    bool qmpReady;
    bool nativePeripheralBridge = false;
    int pendingBridgeProbe = -1;
    int qmpConnectionAttempts = 0;
    bool qmpBootReleasePending = false;
    int qmpSeq;
    int pendingSnapshotCb;
    int pendingRegsCb;
    int pendingMemCb;
    QString pendingPcText;
    QStringList pendingScalars;
    QStringList pendingVectors;

    bool gdbEnabled;
    int gdbPort;
    bool gdbWaitForAttach;

    bool spiFlashEnabled;
    int spiFlashSizeMB;
    QString spiFlashImagePath;
    bool psramEnabled;
    int psramSizeMB;
    QString psramMode;

    QString serialCommunicationType;
    int serialBaudRate;
    int serialDataBits;
    QString serialParity;
    int serialStopBits;
    QString serialFlowControl;
    QString serialLineEnding;

    QString customBaseMac;
    bool chipRevisionEnabled;
    int chipRevision;

    int uartMasterFd;
    int uartKeepAliveSlaveFd;
    QString uartSlavePath;
    QString uartAliasPath;
    QSocketNotifier *uartReadNotifier;
    QByteArray uartIngressHistory;
    bool autoDownloadByUartSync;
    bool autoDownloadSwitchPending;

    /* I2C bridge address sets (one per bus, indexed 0/1) */
    static constexpr int I2C_BUS_COUNT = 2;
    QSet<QString> i2cBridgeAddrs[I2C_BUS_COUNT];
    QString i2cResponseMaps[I2C_BUS_COUNT];
    QHash<QString, int> spiDcGpios;

    struct PendingQmpCommand {
        QString execute;
        QJsonObject arguments;
        quint64 revision = 0;
        quint64 epoch = 0;
        bool initializationSetting = false;
        qint64 sentAt = 0;
        quint64 circuitContext = 0;
    };
    struct BridgeCapabilityProbe {
        QString type;
        QSet<QString> properties;
        bool enabled = false;
        int remaining = 0;
    };
    RuntimeStatus status;
    QList<RuntimeCapability> capabilities;
    QString runtimeIdentity;
    QHash<int, PendingQmpCommand> pendingQmpCommands;
    QHash<QString, BridgeCapabilityProbe> bridgeProbes;
    QSet<int> nativeI2cBuses;
    QSet<QString> nativeSpiControllers;
    int pendingCapabilitiesCb = -1;
    int pendingInitializationProbes = 0;
    bool initializationComplete = false;
    bool executionStatusObserved = false;
    bool bridgeSettingsReplayed = false;
    bool collectingBridgeSettings = false;
    int pendingBridgeSettings = 0;
    bool debuggerWaitPending = false;
    bool stoppingProcess = false;
    bool qmpTestEndpoint = false;
    quint64 observationRevision = 0;
    QTimer *qmpDeadlineTimer = nullptr;
    QTimer *qmpCommandTimer = nullptr;
    QTimer *nativeCircuitTimer = nullptr;
    bool nativeCircuitSupported = false;
    bool executionStoppedObserved = false;
    QString nativeCircuitReason = QStringLiteral("Native electrical QOM support has not been confirmed.");
    QSet<QString> nativeCircuitProperties;
    QJsonObject nativeCircuitDiscoverySnapshot;
    QJsonObject acceptedNativeCircuit;
    QJsonObject pendingNativeCircuit;
    QString nativeCircuitGeneration;
    int pendingNativeCircuitDiscovery = 0;
    int pendingNativeCircuitApply = -1;
    int pendingNativeCircuitSnapshot = -1;
    quint64 nativeCircuitContext = 0;
    QElapsedTimer qmpCommandClock;
};
