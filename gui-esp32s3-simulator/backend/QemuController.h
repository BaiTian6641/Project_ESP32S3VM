#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QString>
#include <QSet>

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
    bool containsDownloadSyncPreamble(const QByteArray &bytes);
    void startQemuWithFirmware(const QString &firmwarePath);
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
};
