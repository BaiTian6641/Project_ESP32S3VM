#pragma once

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

class QProcess;

class PeripheralManager : public QObject
{
    Q_OBJECT

public:
    explicit PeripheralManager(QObject *parent = nullptr);
    ~PeripheralManager() override;

    void setWorkspaceRoot(const QString &path);
    bool loadConfig(const QString &path);
    QString findDefaultConfigPath() const;
    bool loadDefaultConfig();

    void startAll();
    void ensureAllRunning();
    void stopAll();
    void refreshStates();
    void dispatchI2cTransfer(const QJsonObject &request);
    void dispatchSpiTransfer(const QJsonObject &request);
    void dispatchUartTx(const QJsonObject &request);

    void sendDeviceRpc(const QString &deviceId,
                       const QString &method,
                       const QJsonObject &params = QJsonObject());
    void setDeviceParameter(const QString &deviceId,
                            const QString &paramName,
                            const QVariant &value);

    QJsonArray devicesSnapshot() const;
    QString configPath() const;

    /** Return I2C addresses per bus index (0‑based).
     *  Map key = bus index (e.g. 0 for "i2c0"), value = set of hex addresses
     *  without "0x" prefix (e.g. "3c", "40"). */
    QMap<int, QSet<QString>> getI2cBusAddresses() const;

signals:
    void devicesChanged();
    void deviceSetChanged();  // fired only when config loads/starts/stops (not on state polls)
    void deviceLogLine(const QString &deviceId, const QString &line);
    void deviceTraceLine(const QString &deviceId, const QString &line);
    void bridgeResponseReady(const QString &busKind, const QJsonObject &payload);
    void i2cResponseMapReady(int busIndex, const QString &mapStr);
    void managerMessage(const QString &line);

private:
    struct DeviceRuntime {
        QString id;
        QString type;
        QString status;
        QString lastError;

        QString busKind;
        QString busController;
        QString busAddress;

        QJsonObject rawConfig;
        QJsonObject capabilities;
        QJsonObject state;

        QString simExec;
        QStringList simArgs;

        QProcess *process = nullptr;
        QByteArray stdoutBuffer;
        int rpcSeq = 1;
        QHash<int, QJsonObject> pendingMethods;
        bool stateRequestInFlight = false;
        qint64 lastStateRequestMs = 0;
    };

    QString resolvePathForConfig(const QString &raw) const;
    QString resolvePythonExecutable(QStringList *bootstrapArgs = nullptr) const;
    bool isPythonScriptPath(const QString &path) const;
    bool validateConfigDocument(const QJsonObject &root, QStringList &errors, QStringList &warnings) const;
    bool busKindMatchesProtocol(const QString &busKind, const QString &protocol) const;
    QString normalizeAddressText(const QString &value) const;
    bool shouldEmitBridgeLog(const QString &key, qint64 minIntervalMs);
    void emitTrace(DeviceRuntime *device, const QString &dir, const QJsonObject &obj);
    void startDevice(DeviceRuntime *device);
    void stopDevice(DeviceRuntime *device);

    void sendRpc(DeviceRuntime *device,
                 const QString &method,
                 const QJsonObject &params = QJsonObject(),
                 bool withId = true,
                 const QJsonObject &meta = QJsonObject());

    void parseStdout(DeviceRuntime *device, const QByteArray &bytes);
    void parseStderr(DeviceRuntime *device, const QByteArray &bytes);
    void handleJsonMessage(DeviceRuntime *device, const QJsonObject &obj);
    void emitI2cResponseMap(DeviceRuntime *device, const QJsonObject &responseMap);

    QList<DeviceRuntime *> devices;
    QTimer *autoRefreshTimer = nullptr;
    QString loadedConfigPath;
    QString configDir;
    QString workspaceRoot;
    QHash<QString, qint64> bridgeLogLastMs;
};
