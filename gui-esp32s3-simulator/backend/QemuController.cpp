#include "QemuController.h"
#include "QemuLaunchOptions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <cmath>

#ifdef Q_OS_LINUX
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#endif

static QStringList splitLines(QString &buffer)
{
    QStringList lines;
    int idx = buffer.indexOf('\n');
    while (idx >= 0) {
        QString line = buffer.left(idx);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        lines.append(line);
        buffer.remove(0, idx + 1);
        idx = buffer.indexOf('\n');
    }
    return lines;
}

static QByteArray filterPrintableSerialLog(const QByteArray &bytes)
{
    QByteArray out;
    out.reserve(bytes.size());
    for (char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
            out.append(ch);
        }
    }
    return out;
}

static constexpr char kEspSyncPreamble[] = {0x07, 0x07, 0x12, 0x20};

static const QString kElectricalPath = QStringLiteral("/machine/soc/electrical");
static const QString kElectricalProfile = QStringLiteral("s3-explicit-finite-v1");
static constexpr qsizetype kMaxQmpMessage = 4 * 1024 * 1024;
static constexpr qsizetype kMaxNativeProject = 1024 * 1024;

static bool decimalCounter(const QJsonValue &value)
{
    if (!value.isString() || value.toString().isEmpty()) return false;
    for (const auto ch : value.toString())
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    bool ok = false;
    value.toString().toULongLong(&ok);
    return ok;
}

static bool parseElectricalSnapshot(const QJsonValue &value, QJsonObject &snapshot)
{
    if (!value.isString() || value.toString().size() > kMaxQmpMessage) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (object.value("abi") != 1 || !decimalCounter(object.value("generation"))
        || !decimalCounter(object.value("timestamp_ns")) || !object.value("status").isString()
        || !object.value("diagnostic").isString() || !object.value("nets").isArray()) return false;
    QSet<QString> ids;
    for (const auto &entry : object.value("nets").toArray()) {
        if (!entry.isObject()) return false;
        const auto net = entry.toObject();
        const auto id = net.value("id").toString();
        const auto voltage = net.value("voltage_v");
        if (id.isEmpty() || ids.contains(id) || !net.value("valid").isBool()
            || !net.value("floating").isBool() || (!voltage.isNull() && !voltage.isDouble())
            || (voltage.isDouble() && !std::isfinite(voltage.toDouble()))
            || (net.value("valid").toBool() && !voltage.isDouble())) return false;
        ids.insert(id);
    }
    snapshot = object;
    return true;
}

#ifdef Q_OS_LINUX
static speed_t baudToSpeed(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default:
        return B115200;
    }
}
#endif

QemuController::QemuController(QObject *parent)
    : QObject(parent),
      qemuProcess(new QProcess(this)),
            qmpTcpSocket(new QTcpSocket(this)),
            liveTimer(new QTimer(this)),
            qmpPort(0),
            bootMode(0),
            memoryInspectBase("0x3FC80000"),
            qmpReady(false),
            qmpSeq(1),
            pendingSnapshotCb(-1),
            pendingRegsCb(-1),
            pendingMemCb(-1),
            gdbEnabled(false),
            gdbPort(1234),
            gdbWaitForAttach(false),
            spiFlashEnabled(true),
            spiFlashSizeMB(16),
            psramEnabled(false),
            psramSizeMB(8),
            psramMode("qspi"),
            serialCommunicationType("UART TTL"),
            serialBaudRate(115200),
            serialDataBits(8),
            serialParity("None"),
            serialStopBits(1),
            serialFlowControl("None"),
            serialLineEnding("LF"),
            customBaseMac(""),
            chipRevisionEnabled(false),
            chipRevision(0),
            uartMasterFd(-1),
            uartKeepAliveSlaveFd(-1),
            uartAliasPath("/tmp/esp32s3-uart"),
            uartReadNotifier(nullptr),
            autoDownloadByUartSync(true),
            autoDownloadSwitchPending(false)
{
    qRegisterMetaType<RuntimeStatus>();
    qRegisterMetaType<QList<RuntimeCapability>>();
    qmpDeadlineTimer = new QTimer(this);
    qmpDeadlineTimer->setSingleShot(true);
    qmpDeadlineTimer->setInterval(10000);
    connect(qmpDeadlineTimer, &QTimer::timeout, this, [this]() {
        if (qmpTransportExpected() && (!initializationComplete || !executionStatusObserved))
            failRuntime(QStringLiteral("QMP initialization deadline exceeded. Restart the simulation to retry."), true);
    });
    qmpCommandClock.start();
    qmpCommandTimer = new QTimer(this);
    qmpCommandTimer->setInterval(250);
    connect(qmpCommandTimer, &QTimer::timeout, this, [this]() {
        if (!qmpReady || stoppingProcess) return;
        const qint64 now = qmpCommandClock.elapsed();
        for (const auto &command : pendingQmpCommands) {
            if (now - command.sentAt >= 10000) {
                failRuntime(QString("QMP %1 acknowledgement deadline exceeded; the simulation was stopped.")
                            .arg(command.execute), true);
                return;
            }
        }
    });
    qmpCommandTimer->start();
    nativeCircuitTimer = new QTimer(this);
    nativeCircuitTimer->setInterval(500);
    connect(nativeCircuitTimer, &QTimer::timeout, this, &QemuController::requestNativeCircuitSnapshot);
    updateRuntimeCapabilities();
    qemuProcess->setProcessChannelMode(QProcess::SeparateChannels);
        liveTimer->setInterval(500);

    connect(qemuProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const QByteArray bytes = qemuProcess->readAllStandardOutput();
        if (bytes.isEmpty()) {
            return;
        }

#ifdef Q_OS_LINUX
        if (uartMasterFd >= 0) {
            const qint64 written = ::write(uartMasterFd, bytes.constData(), static_cast<size_t>(bytes.size()));
            if (written < 0 && errno != EIO) {
                emit debugMessageReceived(QString("[Serial] virtual UART write error: errno=%1").arg(errno));
            }
        }
#endif

        if (bootMode == 1) {
            const QByteArray filtered = filterPrintableSerialLog(bytes);
            if (!filtered.isEmpty()) {
                handleQemuOutputChunk(QString::fromLatin1(filtered));
            }
        } else {
            handleQemuOutputChunk(QString::fromUtf8(bytes));
        }
    });

    connect(qemuProcess, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray bytes = qemuProcess->readAllStandardError();
        if (bytes.isEmpty()) {
            return;
        }
        handleQemuOutputChunk(QString::fromUtf8(bytes));
    });

    connect(qemuProcess, &QProcess::started, this, [this]() {
        emit debugMessageReceived("[QEMU] process started");
        emit qemuStarted();
        if (bootMode == 1) {
            qmpReady = false;
            emit debugMessageReceived("[QMP] disabled in Download Boot mode to keep ROM serial downloader path exclusive");
            setRuntimePhase(RuntimePhase::WaitingForDevice,
                            QStringLiteral("ROM download process started; waiting for firmware upload. CPU execution is not observed because QMP is disabled."));
            updateRuntimeCapabilities();
        } else {
            setRuntimePhase(RuntimePhase::Connecting, QStringLiteral("QEMU process started; connecting to QMP before releasing firmware."));
            qmpDeadlineTimer->start();
            QTimer::singleShot(250, this, &QemuController::connectQmp);
        }
    });

    connect(qemuProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit debugMessageReceived(QString("[QEMU] process error: %1").arg(static_cast<int>(error)));
        if (!stoppingProcess)
            failRuntime(QString("QEMU process error: %1").arg(qemuProcess->errorString()));
    });

    connect(qemuProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                emit debugMessageReceived(QString("[QEMU] exited code=%1 status=%2")
                                        .arg(code)
                                        .arg(status == QProcess::NormalExit ? "normal" : "crash"));
                if (status == QProcess::CrashExit || code == 139 || code == 11) {
                    emit debugMessageReceived("[QEMU] guest/emulator crash detected; UART/QMP transport may disconnect as a consequence");
                }
                qmpReady = false;
                nativePeripheralBridge = false;
                nativeI2cBuses.clear();
                nativeSpiControllers.clear();
                qmpDeadlineTimer->stop();
                disconnectQmp();
                emit peripheralBridgeAvailable(false);
                liveTimer->stop();
                autoDownloadSwitchPending = false;
                if (this->status.phase != RuntimePhase::Error) {
                    if (!stoppingProcess && (status == QProcess::CrashExit || code != 0))
                        setRuntimePhase(RuntimePhase::Error, QString("QEMU exited with code %1 (%2).")
                                        .arg(code).arg(status == QProcess::CrashExit ? "crashed" : "failed"));
                    else
                        setRuntimePhase(RuntimePhase::Stopped, QStringLiteral("QEMU process stopped."));
                }
                updateRuntimeCapabilities();
                emit qemuStopped();
            });

    connect(qmpTcpSocket, &QTcpSocket::connected, this, [this]() {
        emit debugMessageReceived("[QMP] connected");
        qmpDeadlineTimer->start();
    });

    connect(qmpTcpSocket, &QTcpSocket::readyRead, this, [this]() {
        const QByteArray bytes = qmpTcpSocket->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        qmpBuffer += QString::fromUtf8(bytes);
        if (qmpBuffer.size() > kMaxQmpMessage) {
            failRuntime(QStringLiteral("QMP reply exceeded the bounded message size."), true);
            return;
        }
        processQmpBuffer();
    });

    connect(qmpTcpSocket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
        emit debugMessageReceived(QString("[QMP] socket error: %1").arg(static_cast<int>(e)));
        if (stoppingProcess || status.phase == RuntimePhase::Stopping || status.phase == RuntimePhase::Stopped)
            return;
        if (qmpReady) {
            failRuntime(QStringLiteral("QMP connection was lost; execution state is no longer observable."), true);
        } else if (qmpTransportExpected() && qmpConnectionAttempts < 40) {
            QTimer::singleShot(250, this, &QemuController::connectQmp);
        } else if (qmpTransportExpected() && qmpConnectionAttempts >= 40) {
            emit debugMessageReceived("[QMP] connection deadline exceeded; restart the target to retry.");
            failRuntime(QStringLiteral("QMP connection deadline exceeded. Restart the simulation to retry."), true);
        }
    });
    connect(qmpTcpSocket, &QTcpSocket::disconnected, this, [this]() {
        if (!stoppingProcess && status.phase != RuntimePhase::Stopping && qmpReady)
            failRuntime(QStringLiteral("QMP disconnected; execution state is no longer observable."), true);
    });

    connect(liveTimer, &QTimer::timeout, this, &QemuController::pollLiveState);

    if (!setupUartPty()) {
        emit debugMessageReceived("[Serial] startup PTY initialization failed; external adapter disabled");
    }
}

bool QemuController::containsDownloadSyncPreamble(const QByteArray &bytes)
{
    uartIngressHistory += bytes;
    if (uartIngressHistory.size() > 512) {
        uartIngressHistory.remove(0, uartIngressHistory.size() - 512);
    }

    return uartIngressHistory.contains(QByteArray(kEspSyncPreamble, sizeof(kEspSyncPreamble)));
}

QemuController::~QemuController()
{
    stopQemu();
    teardownUartPty();
}

RuntimeStatus QemuController::runtimeStatus() const { return status; }
QList<RuntimeCapability> QemuController::runtimeCapabilities() const { return capabilities; }

bool QemuController::nativeCircuitAvailable() const { return nativeCircuitSupported; }
QString QemuController::nativeCircuitUnavailableReason() const { return nativeCircuitReason; }

QString QemuController::nativeCircuitApplyUnavailableReason() const
{
    if (!nativeCircuitSupported) return nativeCircuitReason;
    if (!qmpReady || !qmpTransportExpected() || !executionStatusObserved || !executionStoppedObserved)
        return QStringLiteral("An acknowledged stopped execution state is required before native Apply.");
    if (status.phase != RuntimePhase::Paused && status.phase != RuntimePhase::Initializing)
        return QStringLiteral("Pause the live simulator and await acknowledgement before native Apply.");
    if (pendingNativeCircuitApply >= 0)
        return QStringLiteral("Native Apply is in flight; awaiting QMP acknowledgement.");
    for (const auto &command : pendingQmpCommands)
        if (command.execute == "cont" || command.execute == "stop")
            return QStringLiteral("Await the pending execution-control acknowledgement before native Apply.");
    return {};
}

bool QemuController::applyNativeCircuit(const QJsonObject &document)
{
    QString reason = nativeCircuitApplyUnavailableReason();
    const auto electrical = document.value("runtime").toObject().value("electrical").toObject();
    const auto mode = electrical.value("mode").toString("dc");
    if (reason.isEmpty() && (document.value("version") != 3
        || electrical.value("driver_profile").toString() != kElectricalProfile))
        reason = QStringLiteral("Native Apply requires a pure v3 circuit and the explicit S3 finite driver profile.");
    if (reason.isEmpty() && mode != "dc" && mode != "rc")
        reason = QStringLiteral("Native Apply requires an explicit DC or RC solver mode.");
    if (reason.isEmpty() && mode == "rc" &&
        !nativeCircuitDiscoverySnapshot.value("support").toObject().value("rc").toBool())
        reason = QStringLiteral("This native electrical runtime did not advertise RC support.");
    if (reason.isEmpty() && mode == "rc" && electrical.value("edit_charge") != "keep"
        && electrical.value("edit_charge") != "reset")
        reason = QStringLiteral("Native RC Apply requires an explicit keep/reset edit charge policy.");
    const auto json = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (reason.isEmpty() && json.size() > kMaxNativeProject)
        reason = QStringLiteral("The native circuit exceeds the bounded project JSON size.");
    if (!reason.isEmpty()) {
        // A duplicate caller must not clear the first request's in-flight state.
        if (pendingNativeCircuitApply < 0) emit nativeCircuitApplyInFlightChanged(false, reason);
        emit debugMessageReceived("[Electrical] " + reason);
        return false;
    }
    clearNativeCircuit(false, false, QStringLiteral("Awaiting native project-json acknowledgement."));
    pendingNativeCircuit = document; // Immutable submitted graph, never the editor's later draft.
    pendingNativeCircuitApply = qmpSeq++;
    sendQmpCommand("qom-set", {{"path", kElectricalPath}, {"property", "project-json"},
                              {"value", QString::fromUtf8(json)}}, pendingNativeCircuitApply);
    emit nativeCircuitApplyInFlightChanged(true, QStringLiteral("Native Apply is awaiting QMP acknowledgement."));
    return true;
}

void QemuController::requestNativeCircuitSnapshot()
{
    if (!nativeCircuitSupported || !qmpReady || !qmpTransportExpected() || !executionStatusObserved
        || acceptedNativeCircuit.isEmpty() || pendingNativeCircuitApply >= 0
        || pendingNativeCircuitSnapshot >= 0
        || (status.phase != RuntimePhase::Running && status.phase != RuntimePhase::Paused
            && status.phase != RuntimePhase::Initializing && status.phase != RuntimePhase::WaitingForDebugger))
        return;
    pendingNativeCircuitSnapshot = qmpSeq++;
    sendQmpCommand("qom-get", {{"path", kElectricalPath}, {"property", "snapshot-json"}},
                   pendingNativeCircuitSnapshot);
}

void QemuController::clearNativeCircuit(bool clearSupport, bool clearAcceptedDocument, const QString &reason)
{
    ++nativeCircuitContext;
    const bool wasApplying = pendingNativeCircuitApply >= 0;
    pendingNativeCircuitApply = pendingNativeCircuitSnapshot = -1;
    pendingNativeCircuitDiscovery = 0;
    pendingNativeCircuit = {};
    nativeCircuitGeneration.clear();
    for (auto it = pendingQmpCommands.begin(); it != pendingQmpCommands.end();) {
        if (it->arguments.value("path").toString() == kElectricalPath) it = pendingQmpCommands.erase(it);
        else ++it;
    }
    if (clearAcceptedDocument) acceptedNativeCircuit = {};
    emit nativeCircuitSnapshotUpdated({}, acceptedNativeCircuit);
    if (wasApplying) emit nativeCircuitApplyInFlightChanged(false, reason);
    if (clearSupport) {
        nativeCircuitSupported = false;
        nativeCircuitProperties.clear();
        nativeCircuitDiscoverySnapshot = {};
        nativeCircuitReason = reason;
        nativeCircuitTimer->stop();
        emit nativeCircuitAvailableChanged(false, nativeCircuitReason);
    }
}

void QemuController::finishNativeCircuitDiscovery()
{
    const auto support = nativeCircuitDiscoverySnapshot.value("support").toObject();
    nativeCircuitSupported = nativeCircuitProperties.contains("project-json")
        && nativeCircuitProperties.contains("snapshot-json")
        && nativeCircuitDiscoverySnapshot.value("abi") == 1
        && support.value("dc").toBool() && support.value("profile").toString() == kElectricalProfile;
    nativeCircuitReason = nativeCircuitSupported
        ? QStringLiteral("QOM project-json and snapshot-json ABI 1 acknowledged; explicit finite DC support is implemented, not hardware-qualified.")
        : QStringLiteral("This runtime did not acknowledge project-json, snapshot-json ABI 1 and the explicit finite DC profile.");
    emit nativeCircuitAvailableChanged(nativeCircuitSupported, nativeCircuitReason);
    if (nativeCircuitSupported) nativeCircuitTimer->start();
    updateRuntimeCapabilities();
}

bool QemuController::handleNativeCircuitReply(int id, const PendingQmpCommand &command, const QJsonObject &reply)
{
    if (command.circuitContext != nativeCircuitContext) return true;
    const bool error = reply.contains("error");
    const auto errorText = reply.value("error").toObject().value("desc").toString("Unknown QMP error");
    if (pendingNativeCircuitDiscovery > 0) {
        if (!error && command.execute == "qom-list") {
            for (const auto &entry : reply.value("return").toArray()) {
                const auto property = entry.toObject();
                if (property.value("type") == "string")
                    nativeCircuitProperties.insert(property.value("name").toString());
            }
        } else if (!error && command.execute == "qom-get") {
            parseElectricalSnapshot(reply.value("return"), nativeCircuitDiscoverySnapshot);
        }
        --pendingNativeCircuitDiscovery;
        --pendingInitializationProbes;
        if (pendingNativeCircuitDiscovery == 0) finishNativeCircuitDiscovery();
        finishRuntimeInitialization();
        return true;
    }
    if (id == pendingNativeCircuitApply) {
        pendingNativeCircuitApply = -1;
        if (error) {
            pendingNativeCircuit = {};
            emit nativeCircuitApplyInFlightChanged(false, "Native Apply failed: " + errorText
                + QStringLiteral(" The previous acknowledged graph is unchanged."));
        } else {
            acceptedNativeCircuit = pendingNativeCircuit;
            pendingNativeCircuit = {};
            emit nativeCircuitApplyInFlightChanged(false, QStringLiteral("Native circuit accepted by QMP. Readings await a backend snapshot."));
        }
        emit nativeCircuitSnapshotUpdated({}, acceptedNativeCircuit);
        requestNativeCircuitSnapshot();
        return true;
    }
    if (id == pendingNativeCircuitSnapshot) {
        pendingNativeCircuitSnapshot = -1;
        QJsonObject snapshot;
        QString reason;
        if (error) reason = "Native snapshot query failed: " + errorText;
        else if (!parseElectricalSnapshot(reply.value("return"), snapshot))
            reason = QStringLiteral("Native snapshot has an invalid ABI 1 response; no readings were displayed.");
        else if (!nativeCircuitGeneration.isEmpty()
                 && nativeCircuitGeneration != snapshot.value("generation").toString())
            reason = QStringLiteral("The backend topology generation changed outside this accepted Apply context; no readings were displayed.");
        if (!reason.isEmpty()) {
            emit nativeCircuitSnapshotUpdated({}, acceptedNativeCircuit);
            emit nativeCircuitSnapshotUnavailable(reason);
            return true;
        }
        nativeCircuitGeneration = snapshot.value("generation").toString();
        emit nativeCircuitSnapshotUpdated(snapshot, acceptedNativeCircuit);
        return true;
    }
    return true; // Unmatched native replies cannot establish graph or support state.
}

void QemuController::setRuntimePhase(RuntimePhase phase, const QString &message)
{
    status.phase = phase;
    status.message = message;
    emit runtimeStatusChanged(status);
    if (phase == RuntimePhase::Error || phase == RuntimePhase::Stopping || phase == RuntimePhase::Stopped)
        clearNativeCircuit(true, true, QStringLiteral("No observable live native electrical session is available."));
}

void QemuController::updateRuntimeCapabilities()
{
    const bool active = qemuProcess->state() == QProcess::Running || qmpTestEndpoint;
    const bool controllablePhase = status.phase == RuntimePhase::Running || status.phase == RuntimePhase::Paused
        || status.phase == RuntimePhase::WaitingForDebugger;
    const bool controlled = qmpReady && initializationComplete && executionStatusObserved && !stoppingProcess
        && controllablePhase;
    const QString noControl = bootMode == 1
        ? QStringLiteral("QMP is disabled in ROM download mode; execution state is not observed.")
        : QStringLiteral("QMP initialization and an acknowledged execution status are required.");
    capabilities.clear();
    auto add = [this](const QString &id, const QString &title, CapabilityMaturity maturity,
                      bool available, const QString &reason, const QStringList &evidence = {}) {
        capabilities.append({id, title, maturity, available, reason, evidence});
    };
    add("qemu.identity", "QEMU runtime identity", CapabilityMaturity::Implemented,
        active && !runtimeIdentity.isEmpty(), runtimeIdentity.isEmpty()
            ? QStringLiteral("No runtime version has been observed.") : runtimeIdentity,
        runtimeIdentity.isEmpty() ? QStringList{} : QStringList{"QMP greeting version: " + runtimeIdentity});
    add("qmp.control", "Pause, resume and execution status", CapabilityMaturity::Implemented,
        controlled, controlled ? QStringLiteral("QMP negotiated with this runtime; execution states require acknowledgements/events.") : noControl);
    add("uart.console", "UART0 console", CapabilityMaturity::Implemented,
        qemuProcess->state() == QProcess::Running && bootMode == 0,
        bootMode == 1 ? QStringLiteral("ROM download mode uses the external PTY upload path.")
                      : QStringLiteral("Firmware UART byte transport; framing/electrical fidelity is not qualified."));
    add("debug.inspect", "Scalar registers and memory inspection", CapabilityMaturity::Implemented,
        controlled, controlled ? QStringLiteral("QMP monitor inspection; unavailable values remain explicitly unavailable.") : noControl);
    for (int bus = 0; bus < I2C_BUS_COUNT; ++bus) {
        const QString path = QString("/machine/soc/i2c%1/i2c/child[0]").arg(bus);
        const auto probe = bridgeProbes.value(path);
        const bool detected = nativeI2cBuses.contains(bus);
        const QString reason = detected
            ? QStringLiteral("Cached address/command response bridge; generic transactions, timing and electrical routing are not qualified.")
            : (probe.type.isEmpty() ? QStringLiteral("Cached I2C bridge was not detected in the selected runtime.")
                                   : QString("QOM type %1 does not expose the required cached-read interface.").arg(probe.type));
        add(QString("i2c%1.cached-read").arg(bus), QString("I2C%1 cached sensor bridge").arg(bus),
            detected ? CapabilityMaturity::Implemented : CapabilityMaturity::Catalogued,
            controlled && detected, reason,
            detected ? QStringList{path + ": esp32s3.i2c-bridge; registered-addrs; read-response-map"} : QStringList{});
    }
    for (const QString &controller : {QStringLiteral("spi2"), QStringLiteral("spi3")}) {
        const bool detected = nativeSpiControllers.contains(controller);
        add(controller + ".tx", controller.toUpper() + " TX display bridge",
            detected ? CapabilityMaturity::Implemented : CapabilityMaturity::Catalogued,
            controlled && detected, detected
                ? QStringLiteral("TX event bridge only; MISO, full duplex, timing and electrical routing are not qualified.")
                : QStringLiteral("An enabled GP-SPI TX bridge of the expected QOM type was not detected."),
            detected ? QStringList{"QOM esp32s3.gpspi; bridge-enabled=true; bridge-dc-gpio"} : QStringList{});
    }
    add("electrical.native", "Native electrical circuit Apply and snapshots",
        nativeCircuitSupported ? CapabilityMaturity::Implemented : CapabilityMaturity::Catalogued,
        nativeCircuitSupported, nativeCircuitReason,
        nativeCircuitSupported ? QStringList{kElectricalPath + ": project-json; snapshot-json ABI 1"} : QStringList{});
    add("debug.step", "Single instruction step", CapabilityMaturity::Catalogued, false,
        "A verified GDB control path has not been implemented; use an external GDB client.");
    add("debug.breakpoints", "Breakpoint control", CapabilityMaturity::Catalogued, false,
        "A verified GDB control path has not been implemented; monitor command requests are not proof of acceptance.");
    for (const auto &item : QList<QPair<QString, QString>>{{"gpio.nets", "Electrical pin routing"},
            {"analog.adc", "Analog voltages and ADC"}, {"radio.wifi", "Native Wi-Fi"},
            {"radio.ble", "Native Bluetooth LE"}, {"cpu.simd", "Qualified SIMD instruction behavior"}})
        add(item.first, item.second, CapabilityMaturity::Catalogued, false,
            "This runtime has no loaded qualification evidence for this capability.");
    emit runtimeCapabilitiesChanged(capabilities);
}

void QemuController::failRuntime(const QString &message, bool stopProcess)
{
    setRuntimePhase(RuntimePhase::Error, message);
    emit debugMessageReceived("[Runtime] " + message);
    if (stopProcess) {
        qmpTestEndpoint = false;
        stopQemu();
    }
    updateRuntimeCapabilities();
}

void QemuController::stopSimulation()
{
    qmpTestEndpoint = false;
    ++observationRevision;
    stopQemu();
    nativePeripheralBridge = false;
    nativeI2cBuses.clear();
    nativeSpiControllers.clear();
    updateRuntimeCapabilities();
    emit peripheralBridgeAvailable(false);
    if (qemuProcess->state() == QProcess::NotRunning)
        setRuntimePhase(RuntimePhase::Stopped, QStringLiteral("Simulation stopped."));
}

void QemuController::attachQmpEndpointForTesting(quint16 port)
{
    stopSimulation();
    status.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    status.resetEpoch = 0;
    qmpTestEndpoint = true;
    runtimeIdentity.clear();
    bridgeProbes.clear();
    qmpPort = port;
    qmpConnectionAttempts = 0;
    qmpBootReleasePending = false;
    initializationComplete = false;
    executionStatusObserved = false;
    bridgeSettingsReplayed = false;
    collectingBridgeSettings = false;
    pendingBridgeSettings = 0;
    debuggerWaitPending = gdbEnabled && gdbWaitForAttach;
    setRuntimePhase(RuntimePhase::Connecting, QStringLiteral("Connecting to the local QMP test endpoint."));
    qmpDeadlineTimer->start();
    connectQmp();
}

void QemuController::sendUart0(const QString &text)
{
    if (qemuProcess->state() != QProcess::Running) {
        emit debugMessageReceived("[UART0] QEMU is not running");
        return;
    }

    if (bootMode == 1) {
        emit debugMessageReceived("[UART0] input blocked in Download Boot mode; use esptool on the recommended /dev/pts/<N> port");
        return;
    }

    qemuProcess->write(text.toUtf8());
}

void QemuController::setSerialConfig(const QString &communicationType,
                                     int baudRate,
                                     int dataBits,
                                     const QString &parity,
                                     int stopBits,
                                     const QString &flowControl,
                                     const QString &lineEnding)
{
    serialCommunicationType = communicationType;
    serialBaudRate = baudRate;
    serialDataBits = dataBits;
    serialParity = parity;
    serialStopBits = stopBits;
    serialFlowControl = flowControl;
    serialLineEnding = lineEnding;

    emit debugMessageReceived(QString("[Serial] mode=%1 baud=%2 %3%4 %5 flow=%6 line=%7")
                            .arg(serialCommunicationType)
                            .arg(serialBaudRate)
                            .arg(serialDataBits)
                            .arg(serialParity.left(1).toUpper())
                            .arg(serialStopBits)
                            .arg(serialFlowControl)
                            .arg(serialLineEnding));

    if (qemuProcess->state() == QProcess::Running) {
        emit debugMessageReceived("[Serial] settings applied to GUI/backend; stdio transport remains host-side byte stream");
    }

#ifdef Q_OS_LINUX
    if (uartKeepAliveSlaveFd >= 0) {
        struct termios ttyCfg;
        if (::tcgetattr(uartKeepAliveSlaveFd, &ttyCfg) == 0) {
            const speed_t speed = baudToSpeed(serialBaudRate);
            ::cfsetispeed(&ttyCfg, speed);
            ::cfsetospeed(&ttyCfg, speed);

            if (serialFlowControl == "RTS/CTS") {
                ttyCfg.c_cflag |= CRTSCTS;
            } else {
                ttyCfg.c_cflag &= ~CRTSCTS;
            }

            if (serialFlowControl == "XON/XOFF") {
                ttyCfg.c_iflag |= (IXON | IXOFF);
            } else {
                ttyCfg.c_iflag &= ~(IXON | IXOFF);
            }

            ::tcsetattr(uartKeepAliveSlaveFd, TCSANOW, &ttyCfg);
        }
    }
#endif
}

void QemuController::requestCpuSnapshot()
{
    if (!qmpReady) {
        emit debugMessageReceived("[QMP] not ready for snapshot");
        return;
    }

    if (pendingRegsCb >= 0 || pendingMemCb >= 0) {
        return;
    }

    pendingPcText = "unavailable";
    pendingScalars.fill("unavailable", 16);
    pendingVectors.fill("unavailable", 8);

    pendingRegsCb = qmpSeq++;
    QJsonObject args;
    args["command-line"] = "info registers";
    sendQmpCommand("human-monitor-command", args, pendingRegsCb);
}

void QemuController::startLiveUpdates(bool enabled)
{
    if (enabled) {
        liveTimer->start();
        emit debugMessageReceived("[Debug] live mode enabled");
        pollLiveState();
    } else {
        liveTimer->stop();
        emit debugMessageReceived("[Debug] live mode disabled");
    }
}

void QemuController::setMemoryInspectBase(const QString &addressText)
{
    memoryInspectBase = addressText.trimmed();
    if (memoryInspectBase.isEmpty()) {
        memoryInspectBase = "0x3FC80000";
    }
    emit debugMessageReceived(QString("[Debug] memory inspect base set: %1").arg(memoryInspectBase));
}

void QemuController::handleBridgeResponse(const QString &busKind, const QJsonObject &payload)
{
    const QString upper = busKind.trimmed().toUpper();
    const QString jsonText = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString line = QString("[PERIPH][%1][RSP] %2").arg(upper, jsonText);

    emit debugMessageReceived(line);

    // Legacy tester sketches explicitly parse these UART lines. Ordinary
    // firmware must never receive host peripheral JSON as console input.
    if (qemuProcess->state() == QProcess::Running && qEnvironmentVariableIntValue("ESP32S3_SERIAL_BRIDGE") == 1) {
        qemuProcess->write((line + "\n").toUtf8());
    }
}

void QemuController::pauseExecution()
{
    if (!qmpReady || !initializationComplete || !executionStatusObserved || stoppingProcess
        || pendingNativeCircuitApply >= 0
        || (status.phase != RuntimePhase::Running && status.phase != RuntimePhase::Paused
            && status.phase != RuntimePhase::WaitingForDebugger)) {
        return;
    }
    ++observationRevision;
    sendQmpCommand("stop");
    emit debugMessageReceived("[Debug] pause requested");
}

void QemuController::continueExecution()
{
    if (!qmpReady || !initializationComplete || !executionStatusObserved || stoppingProcess
        || pendingNativeCircuitApply >= 0
        || (status.phase != RuntimePhase::Running && status.phase != RuntimePhase::Paused
            && status.phase != RuntimePhase::WaitingForDebugger)) {
        return;
    }
    ++observationRevision;
    sendQmpCommand("cont");
    emit debugMessageReceived("[Debug] continue requested");
}

void QemuController::stepInstruction()
{
    emit debugMessageReceived("[Debug] Single-step is unavailable: a verified GDB control path has not been implemented. Use an external GDB client.");
}

void QemuController::addBreakpoint(const QString &addressText)
{
    Q_UNUSED(addressText)
    emit debugMessageReceived("[Debug] Breakpoints are unavailable: a verified GDB control path has not been implemented. Use an external GDB client.");
}

void QemuController::clearBreakpoints()
{
    emit debugMessageReceived("[Debug] Breakpoint removal is unavailable: a verified GDB control path has not been implemented.");
}

/* ================================================================== */
/*  I2C bridge address management                                      */
/* ================================================================== */

void QemuController::registerI2cBridgeAddress(int busIndex, const QString &hexAddr)
{
    if (busIndex < 0 || busIndex >= I2C_BUS_COUNT) {
        return;
    }
    const QString norm = hexAddr.trimmed().toLower();
    if (norm.isEmpty()) {
        return;
    }
    i2cBridgeAddrs[busIndex].insert(norm);
    pushI2cBridgeAddresses(busIndex);
}

void QemuController::unregisterI2cBridgeAddress(int busIndex, const QString &hexAddr)
{
    if (busIndex < 0 || busIndex >= I2C_BUS_COUNT) {
        return;
    }
    const QString norm = hexAddr.trimmed().toLower();
    i2cBridgeAddrs[busIndex].remove(norm);
    pushI2cBridgeAddresses(busIndex);
}

void QemuController::clearAllI2cBridgeAddresses()
{
    for (int b = 0; b < I2C_BUS_COUNT; b++) {
        i2cBridgeAddrs[b].clear();
        pushI2cBridgeAddresses(b);
    }
}

void QemuController::pushI2cBridgeAddresses(int busIndex)
{
    if (!qmpReady || !nativeI2cBuses.contains(busIndex) || busIndex < 0 || busIndex >= I2C_BUS_COUNT) {
        return;
    }

    /* Build comma-separated hex string (no 0x prefix, e.g. "3c,40") */
    QStringList sorted(i2cBridgeAddrs[busIndex].begin(),
                       i2cBridgeAddrs[busIndex].end());
    sorted.sort();
    const QString addrsValue = sorted.join(',');

    /*
     * QOM path for the bridge on bus N:
     *   /machine/soc/i2cN/i2c/child[0]
     * (the bridge is the first — and only — slave created on each bus)
     */
    const QString path = QString("/machine/soc/i2c%1/i2c/child[0]").arg(busIndex);

    QJsonObject args;
    args["path"]     = path;
    args["property"] = QStringLiteral("registered-addrs");
    args["value"]    = addrsValue;
    sendQmpCommand("qom-set", args);
}

void QemuController::pushAllI2cBridgeAddresses()
{
    for (int b = 0; b < I2C_BUS_COUNT; b++) {
        if (!i2cBridgeAddrs[b].isEmpty()) {
            pushI2cBridgeAddresses(b);
        }
    }
}

void QemuController::setI2cBridgeResponseMap(int busIndex, const QString &mapStr)
{
    if (busIndex >= 0 && busIndex < I2C_BUS_COUNT) i2cResponseMaps[busIndex] = mapStr;
    if (!qmpReady || !nativeI2cBuses.contains(busIndex) || busIndex < 0 || busIndex >= I2C_BUS_COUNT) {
        return;
    }

    const QString path = QString("/machine/soc/i2c%1/i2c/child[0]").arg(busIndex);

    QJsonObject args;
    args["path"]     = path;
    args["property"] = QStringLiteral("read-response-map");
    args["value"]    = mapStr;
    sendQmpCommand("qom-set", args);
}

void QemuController::setSpiDcGpio(const QString &controller, int gpioNum)
{
    spiDcGpios[controller] = gpioNum;
    if (!qmpReady || !nativeSpiControllers.contains(controller.trimmed().toLower())) {
        return;
    }

    /* Map controller name to GP-SPI index inside the SoC:
     *   spi2 → gpspi2   (index 0 → child "gpspi2")
     *   spi3 → gpspi3   (index 1 → child "gpspi3")
     */
    int idx = -1;
    const QString ctrl = controller.trimmed().toLower();
    if (ctrl == "spi2") {
        idx = 2;
    } else if (ctrl == "spi3") {
        idx = 3;
    }
    if (idx < 0) {
        return;
    }

    const QString path = QString("/machine/soc/gpspi%1").arg(idx);

    QJsonObject args;
    args["path"]     = path;
    args["property"] = QStringLiteral("bridge-dc-gpio");
    args["value"]    = QString::number(gpioNum);
    sendQmpCommand("qom-set", args);
}

void QemuController::setGdbServerConfig(bool enabled, int port, bool waitForAttach)
{
    gdbEnabled = enabled;
    gdbPort = (port > 0) ? port : 1234;
    gdbWaitForAttach = waitForAttach;

    const QString status = gdbEnabled
        ? QString("[GDB] enabled on tcp::%1 (%2)")
              .arg(gdbPort)
              .arg(gdbWaitForAttach ? "wait" : "no-wait")
        : QString("[GDB] disabled");

    emit debugMessageReceived(status);
    emit debugStatusUpdated(status);

    const QString attachCommand = pendingFirmware.endsWith(".elf", Qt::CaseInsensitive)
        ? QString("xtensa-esp32s3-elf-gdb %1 -ex \"target remote 127.0.0.1:%2\"")
              .arg(pendingFirmware.isEmpty() ? "<firmware.elf>" : pendingFirmware)
              .arg(gdbPort)
        : QString("gdb -ex \"target remote 127.0.0.1:%1\"").arg(gdbPort);
    emit gdbAttachCommandUpdated(attachCommand);
}

void QemuController::startWithGdb(const QString &firmwarePath, int port, bool waitForAttach)
{
    if (firmwarePath.trimmed().isEmpty()) {
        emit debugMessageReceived("[GDB] firmware path is empty");
        return;
    }

    setGdbServerConfig(true, port, waitForAttach);
    loadFirmware(firmwarePath.trimmed());
}

void QemuController::setSpiFlashConfig(bool enabled, int sizeMB)
{
    spiFlashEnabled = enabled;
    if (sizeMB == 2 || sizeMB == 4 || sizeMB == 8 || sizeMB == 16) {
        spiFlashSizeMB = sizeMB;
    }

    emit debugMessageReceived(QString("[SPI Flash] %1, size=%2MB")
                            .arg(spiFlashEnabled ? "enabled" : "disabled")
                            .arg(spiFlashSizeMB));
}

void QemuController::setPsramConfig(bool enabled, int sizeMB, const QString &mode)
{
    psramEnabled = enabled;

    if (sizeMB == 2 || sizeMB == 4 || sizeMB == 8 || sizeMB == 16 || sizeMB == 32) {
        psramSizeMB = sizeMB;
    }

    const QString normalizedMode = mode.trimmed().toLower();
    if (normalizedMode == "opi" || normalizedMode == "ospi" || normalizedMode == "oct" || normalizedMode == "octal") {
        psramMode = "opi";
    } else {
        psramMode = "qspi";
    }

    emit debugMessageReceived(QString("[PSRAM] %1, size=%2MB, mode=%3")
                            .arg(psramEnabled ? "enabled" : "disabled")
                            .arg(psramSizeMB)
                            .arg(psramMode.toUpper()));
}

void QemuController::setChipIdentityConfig(const QString &baseMac,
                                           bool revisionEnabled,
                                           int revision)
{
    customBaseMac = baseMac.trimmed();
    chipRevisionEnabled = revisionEnabled;
    chipRevision = revision;

    if (!customBaseMac.isEmpty()) {
        emit debugMessageReceived(QString("[Chip] custom base MAC: %1").arg(customBaseMac));
    } else {
        emit debugMessageReceived("[Chip] base MAC: default");
    }

    if (chipRevisionEnabled) {
        emit debugMessageReceived(QString("[Chip] chip revision override: %1").arg(chipRevision));
    } else {
        emit debugMessageReceived("[Chip] chip revision: default");
    }
}

QString QemuController::currentUartPort() const
{
    if (!uartSlavePath.isEmpty()) {
        return uartSlavePath;
    }
    return QString();
}

QString QemuController::recommendedEsptoolCommand(const QString &firmwarePath) const
{
    const QString port = currentUartPort();
    if (port.isEmpty()) {
        return QString("# External esptool flashing port is unavailable on this platform/runtime");
    }

    const QString fw = firmwarePath.trimmed().isEmpty() ? QString("<firmware.bin>")
                                                        : firmwarePath.trimmed();
    return QString("esptool --chip esp32s3 --port %1 --before no-reset --after no-reset --no-stub write-flash 0x0 %2")
        .arg(port, fw);
}

void QemuController::resetTarget()
{
    if (pendingFirmware.isEmpty()) {
        emit debugMessageReceived("[Control] No firmware selected for reset/restart");
        return;
    }

    emit debugMessageReceived("[Control] Reset requested: restarting QEMU process");
    startQemuWithFirmware(pendingFirmware, true);
}

void QemuController::setBootMode(int modeIndex)
{
    bootMode = modeIndex;
    const QString modeName = (bootMode == 0) ? "Normal Boot" : "Download Boot";
    emit debugMessageReceived(QString("[Control] Boot mode set: %1").arg(modeName));
}

void QemuController::loadFirmware(const QString &path)
{
    QString resolvedPath = QFileInfo(path).absoluteFilePath();

    if (resolvedPath.endsWith(".ino.bin", Qt::CaseInsensitive)) {
        const QString mergedPath = resolvedPath.left(resolvedPath.size() - 4) + ".merged.bin";
        if (QFileInfo::exists(mergedPath)) {
            emit debugMessageReceived(QString("[Control] auto-selected merged firmware image: %1").arg(mergedPath));
            resolvedPath = mergedPath;
        }
    }

    pendingFirmware = resolvedPath;
    emit debugMessageReceived(QString("[Control] Firmware selected: %1").arg(pendingFirmware));
    startQemuWithFirmware(pendingFirmware);
}

void QemuController::startQemuWithFirmware(const QString &firmwarePath, bool preserveSession)
{
    stopQemu();
    if (qemuProcess->state() != QProcess::NotRunning) {
        failRuntime(QStringLiteral("The previous QEMU process did not stop; a new session was not launched."));
        return;
    }
    qmpTestEndpoint = false;
    stoppingProcess = false;
    if (preserveSession && !status.sessionId.isEmpty()) ++status.resetEpoch;
    else { status.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces); status.resetEpoch = 0; }
    runtimeIdentity.clear();
    nativeI2cBuses.clear();
    nativeSpiControllers.clear();
    initializationComplete = false;
    executionStatusObserved = false;
    bridgeSettingsReplayed = false;
    collectingBridgeSettings = false;
    pendingBridgeSettings = 0;
    debuggerWaitPending = bootMode == 0 && gdbEnabled && gdbWaitForAttach;
    ++observationRevision;
    setRuntimePhase(RuntimePhase::Validating, QStringLiteral("Checking firmware and QEMU machine compatibility."));
    updateRuntimeCapabilities();
    uartIngressHistory.clear();
    autoDownloadSwitchPending = false;

    QFileInfo fwInfo(firmwarePath);
    if (!fwInfo.exists()) {
        emit debugMessageReceived(QString("[QEMU] Firmware file not found: %1").arg(firmwarePath));
        failRuntime(QString("Firmware file not found: %1").arg(firmwarePath));
        return;
    }

    qemuBinaryPath = resolveQemuBinary();
    if (qemuBinaryPath.isEmpty()) {
        emit debugMessageReceived("[QEMU] qemu-system-xtensa not found. Set ESP32S3_QEMU_BIN or build qemu/build/qemu-system-xtensa");
        failRuntime(QStringLiteral("QEMU was not found. Configure ESP32S3_QEMU_BIN or build the runtime."));
        return;
    }
    {
        const QFileInfo qemuInfo(qemuBinaryPath);
        const QString qemuInfoLine = QString("[QEMU] binary: %1 (size=%2, mtime=%3)")
                .arg(qemuInfo.absoluteFilePath())
                .arg(qemuInfo.size())
                .arg(qemuInfo.lastModified().toString(Qt::ISODate));
        emit debugMessageReceived(qemuInfoLine);
        emit serialLineReceived(QString("[SIMDBG] %1").arg(qemuInfoLine));
    }

    if (uartMasterFd < 0 && !setupUartPty()) {
        emit debugMessageReceived("[Serial] PTY bridge unavailable, continuing with integrated GUI serial only");
    }

    flushUartBridgeBuffers();

    QProcess probe;
    probe.start(qemuBinaryPath, {"-machine", "esp32s3,help"});
    if (!probe.waitForFinished(3000) || probe.exitCode() != 0) {
        probe.kill();
        probe.waitForFinished();
        emit debugMessageReceived("[QEMU] This binary does not provide the esp32s3 machine.");
        failRuntime(QStringLiteral("The selected QEMU binary does not provide the ESP32-S3 machine."));
        return;
    }
    const auto properties = parseMachineProperties(QString::fromUtf8(probe.readAllStandardOutput()));
    QemuLaunchOptions options;
    options.downloadBoot = bootMode == 1;
    options.psramEnabled = psramEnabled;
    options.psramMode = psramMode;
    options.baseMac = customBaseMac;
    options.revisionEnabled = chipRevisionEnabled;
    options.revision = chipRevision;
    QStringList args;
    QString error;
    if (!buildMachineArguments(properties, options, args, error)) {
        emit debugMessageReceived("[QEMU] " + error);
        failRuntime(error);
        return;
    }
    nativePeripheralBridge = false;
    pendingBridgeProbe = -1;
    qmpConnectionAttempts = 0;
    qmpBootReleasePending = bootMode == 0 && !(gdbEnabled && gdbWaitForAttach);
    args << "-accel" << "tcg"
         << "-display" << "none"
         << "-monitor" << "none"
            << "-serial" << "stdio"
         ;

    /* QMP over TCP — works on all platforms (replaces Unix-only local socket). */
    qmpPort = static_cast<quint16>(45454 + (QCoreApplication::applicationPid() % 10000));
    args << "-qmp" << QString("tcp:127.0.0.1:%1,server=on,wait=off").arg(qmpPort);

    /* Resolve QEMU data directory so the machine model can find esp32s3_rev0_rom.bin. */
    const QString dataDir = resolveQemuDataDir();
    if (!dataDir.isEmpty()) {
        args << "-L" << dataDir;
        emit debugMessageReceived(QString("[QEMU] data dir: %1").arg(dataDir));
    } else {
        emit debugMessageReceived("[QEMU] warning: QEMU data directory not found; .bin firmware may fail with 'missing -bios'");
    }

    if (psramEnabled) {
        args << "-m" << QString("%1M").arg(psramSizeMB);
    }

    const bool isElf = firmwarePath.endsWith(".elf", Qt::CaseInsensitive);
    const bool isBin = firmwarePath.endsWith(".bin", Qt::CaseInsensitive);

    if (isElf) {
        args << "-kernel" << firmwarePath;
    } else if (isBin) {
        if (!spiFlashEnabled) {
            emit debugMessageReceived("[QEMU] .bin firmware requires SPI flash in this launcher; enable SPI flash in Control tab");
            failRuntime(QStringLiteral("BIN firmware requires an enabled SPI flash profile."));
            return;
        }

        QString flashPath;
        if (!prepareSpiFlashImage(firmwarePath, flashPath)) {
            failRuntime(QStringLiteral("Firmware flash preparation failed. Select a merged image with bootloader and partition table; see the log for details."));
            return;
        }

        QString escapedFlashPath = flashPath;
        escapedFlashPath.replace(",", ",,"); // QEMU key/value option escaping
        args << "-drive" << QString("file=%1,if=mtd,format=raw").arg(escapedFlashPath);
        emit debugMessageReceived(QString("[QEMU] SPI flash image prepared: %1 (%2MB)")
                                .arg(flashPath)
                                .arg(spiFlashSizeMB));
    } else {
        emit debugMessageReceived("[QEMU] Unsupported firmware type; use .elf or .bin");
        failRuntime(QStringLiteral("Unsupported firmware type. Select an ELF or merged BIN image."));
        return;
    }

    if (gdbEnabled) {
        args << "-gdb" << QString("tcp:127.0.0.1:%1").arg(gdbPort);
        if (gdbWaitForAttach) {
            args << "-S";
        }
    }

    // Freeze normal boot until QMP has installed the available bridge state.
    // UART download mode runs immediately and does not depend on QMP.
    if (bootMode == 0 && !(gdbEnabled && gdbWaitForAttach)) args << "-S";

    if (!isElf) {
        emit debugMessageReceived("[QEMU] BIN launch uses SPI flash boot path (no -kernel)");
    }

    if (psramEnabled) {
        emit debugMessageReceived(QString("[PSRAM] enabled: %1MB (%2)")
                                .arg(psramSizeMB)
                                .arg(psramMode.toUpper()));
    } else {
        emit debugMessageReceived("[PSRAM] disabled");
    }

    if (bootMode == 1) {
        const QString flashPort = currentUartPort();
        emit debugMessageReceived("[QEMU] Download Boot mode selected (BootROM UART/USB downloader)");
        if (!flashPort.isEmpty()) {
            emit debugMessageReceived(QString("[Flash] recommended: esptool --chip esp32s3 --port %1 --before no-reset --after no-reset --no-stub write-flash 0x0 <firmware.bin>\n"
                                            "[Flash] baud negotiation: add --baud <rate> (esptool syncs at ROM speed first, then switches)")
                                    .arg(flashPort));
        } else {
            emit debugMessageReceived("[Flash] no host PTY bridge available for external esptool in this runtime");
        }
    } else {
        emit debugMessageReceived("[QEMU] Normal Boot mode selected (SPI flash boot)");
        emit debugMessageReceived("[Flash] UART sync auto-entry is enabled: esptool sync packet can auto-switch simulator to Download Boot mode");
    }

    {
        const QString launchLine = QString("[QEMU] Launch: %1 %2").arg(qemuBinaryPath, args.join(' '));
        emit debugMessageReceived(launchLine);
        emit serialLineReceived(QString("[SIMDBG] %1").arg(launchLine));
    }
    if (gdbEnabled) {
        const QString endpoint = QString("127.0.0.1:%1").arg(gdbPort);
        const QString mode = gdbWaitForAttach ? "waiting for debugger" : "execution status pending QMP";
        emit debugStatusUpdated(QString("[GDB] endpoint %1 (%2)").arg(endpoint, mode));
    } else {
        emit debugStatusUpdated("[GDB] disabled");
    }
    setRuntimePhase(RuntimePhase::Launching, QStringLiteral("Starting the selected QEMU executable."));
    qemuProcess->start(qemuBinaryPath, args);
}

void QemuController::flushUartBridgeBuffers()
{
#ifdef Q_OS_LINUX
    if (uartMasterFd < 0) {
        return;
    }

    if (uartReadNotifier) {
        uartReadNotifier->setEnabled(false);
    }

    int flushed = 0;
    QByteArray tmp;
    tmp.resize(1024);
    while (true) {
        const ssize_t readLen = ::read(uartMasterFd, tmp.data(), static_cast<size_t>(tmp.size()));
        if (readLen > 0) {
            flushed += static_cast<int>(readLen);
            continue;
        }
        if (readLen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) {
            break;
        }
        if (readLen <= 0) {
            break;
        }
    }

    if (uartKeepAliveSlaveFd >= 0) {
        ::tcflush(uartKeepAliveSlaveFd, TCIOFLUSH);
    }

    if (uartReadNotifier) {
        uartReadNotifier->setEnabled(true);
    }

    if (flushed > 0) {
        emit debugMessageReceived(QString("[Serial] flushed %1 stale bytes from virtual adapter").arg(flushed));
    }
#endif
}

bool QemuController::setupUartPty()
{
    if (uartMasterFd >= 0 && !uartSlavePath.isEmpty()) {
        return true;
    }

#ifdef Q_OS_LINUX
    int masterFd = -1;
    int slaveFd = -1;
    char slaveName[256] = {0};
    if (::openpty(&masterFd, &slaveFd, slaveName, nullptr, nullptr) != 0) {
        emit debugMessageReceived(QString("[Serial] failed to create virtual UART PTY: errno=%1").arg(errno));
        return false;
    }

    struct termios ttyCfg;
    if (::tcgetattr(slaveFd, &ttyCfg) == 0) {
        ::cfmakeraw(&ttyCfg);
        ttyCfg.c_cflag |= (CLOCAL | CREAD);
        ttyCfg.c_cflag &= ~CRTSCTS;
        ::cfsetispeed(&ttyCfg, B115200);
        ::cfsetospeed(&ttyCfg, B115200);
        ::tcsetattr(slaveFd, TCSANOW, &ttyCfg);
    }

    const int flags = ::fcntl(masterFd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
    }

    uartMasterFd = masterFd;
    uartKeepAliveSlaveFd = slaveFd;
    uartSlavePath = QString::fromLocal8Bit(slaveName);

    uartReadNotifier = new QSocketNotifier(uartMasterFd, QSocketNotifier::Read, this);
    connect(uartReadNotifier, &QSocketNotifier::activated, this, [this](int) {
        if (uartMasterFd < 0) {
            return;
        }

        QByteArray bytes;
        bytes.resize(4096);
        const ssize_t readLen = ::read(uartMasterFd, bytes.data(), static_cast<size_t>(bytes.size()));
        if (readLen > 0) {
            bytes.resize(static_cast<int>(readLen));

            if (autoDownloadByUartSync && bootMode == 0 && qemuProcess->state() == QProcess::Running
                && !autoDownloadSwitchPending && !pendingFirmware.isEmpty()
                && containsDownloadSyncPreamble(bytes)) {
                autoDownloadSwitchPending = true;
                emit debugMessageReceived("[Flash] esptool UART sync detected (0x07 0x07 0x12 0x20)");
                emit debugMessageReceived("[Flash] auto-switching to Download Boot mode for firmware transfer");

                QTimer::singleShot(0, this, [this]() {
                    if (!autoDownloadSwitchPending || pendingFirmware.isEmpty()) {
                        return;
                    }
                    autoDownloadSwitchPending = false;
                    bootMode = 1;
                    emit debugMessageReceived("[Control] Boot mode auto-switched: Download Boot (UART sync)");
                    startQemuWithFirmware(pendingFirmware);
                });
                return;
            }

            if (qemuProcess->state() == QProcess::Running) {
                qemuProcess->write(bytes);
            }
            return;
        }

        if (readLen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        if (readLen < 0 && errno == EIO) {
            return;
        }

        if (readLen < 0) {
            emit debugMessageReceived(QString("[Serial] virtual UART read error: errno=%1").arg(errno));
        }
    });

    emit debugMessageReceived(QString("[Serial] virtual UART device ready at startup: %1").arg(uartSlavePath));

    QFile::remove(uartAliasPath);
    if (::symlink(uartSlavePath.toUtf8().constData(), uartAliasPath.toUtf8().constData()) == 0) {
        emit debugMessageReceived(QString("[Serial] stable UART alias: %1 -> %2")
                                .arg(uartAliasPath)
                                .arg(uartSlavePath));
        emit debugMessageReceived(QString("[Serial] monitor command: python3 -m serial.tools.miniterm %1 115200 --raw")
                                .arg(uartAliasPath));
        emit debugMessageReceived(QString("[Serial] esptool port: %1 (use PTY path directly for best compatibility)")
                                .arg(uartSlavePath));
    } else {
        emit debugMessageReceived(QString("[Serial] failed to create stable UART alias %1 (errno=%2)")
                                .arg(uartAliasPath)
                                .arg(errno));
    }

    emit debugMessageReceived("[Serial] GUI UART output is always active; external tools can also open this virtual adapter");
    return true;
#else
    emit debugMessageReceived("[Serial] virtual UART PTY mode is only supported on Linux");
    return false;
#endif
}

void QemuController::teardownUartPty()
{
    if (uartReadNotifier) {
        delete uartReadNotifier;
        uartReadNotifier = nullptr;
    }

#ifdef Q_OS_LINUX
    QFileInfo aliasInfo(uartAliasPath);
    if (aliasInfo.isSymLink() && aliasInfo.symLinkTarget() == uartSlavePath) {
        QFile::remove(uartAliasPath);
    }

    if (uartMasterFd >= 0) {
        ::close(uartMasterFd);
        uartMasterFd = -1;
    }

    if (uartKeepAliveSlaveFd >= 0) {
        ::close(uartKeepAliveSlaveFd);
        uartKeepAliveSlaveFd = -1;
    }
#else
    uartMasterFd = -1;
    uartKeepAliveSlaveFd = -1;
#endif

    uartSlavePath.clear();
}

bool QemuController::prepareSpiFlashImage(const QString &firmwarePath, QString &flashPathOut)
{
    QFile fwFile(firmwarePath);
    if (!fwFile.open(QIODevice::ReadOnly)) {
        emit debugMessageReceived(QString("[SPI Flash] failed to open firmware: %1").arg(firmwarePath));
        return false;
    }

    const QByteArray fwData = fwFile.readAll();
    fwFile.close();

    if (bootMode == 0 && (fwData.size() < 0x8020
        || static_cast<quint8>(fwData.at(0)) != 0xE9
        || static_cast<quint8>(fwData.at(0x8000)) != 0xAA
        || static_cast<quint8>(fwData.at(0x8001)) != 0x50)) {
        emit debugMessageReceived("[SPI Flash] Select a merged ESP32-S3 flash image containing the bootloader at 0x0 and partition table at 0x8000. An application .bin alone cannot boot through ROM.");
        return false;
    }

    auto decodeFlashSizeFromHeader = [](const QByteArray &image) -> int {
        if (image.size() < 4) {
            return 0;
        }

        const quint8 magic = static_cast<quint8>(image.at(0));
        const quint8 flashCfg = static_cast<quint8>(image.at(3));
        if (magic != 0xE9) {
            return 0;
        }

        const quint8 sizeNibble = (flashCfg >> 4) & 0x0F;
        switch (sizeNibble) {
        case 0x0: return 1;
        case 0x1: return 2;
        case 0x2: return 4;
        case 0x3: return 8;
        case 0x4: return 16;
        case 0x5: return 32;
        case 0x6: return 64;
        case 0x7: return 128;
        default: return 0;
        }
    };

    int effectiveFlashMB = spiFlashSizeMB;
    const int firmwareFlashMB = decodeFlashSizeFromHeader(fwData);
    if (firmwareFlashMB > 0 && firmwareFlashMB != effectiveFlashMB) {
        emit debugMessageReceived(QString("[SPI Flash] firmware header declares %1MB, simulator hardware is %2MB; using simulator hardware size")
                                .arg(firmwareFlashMB)
                                .arg(effectiveFlashMB));
    }

    const qint64 flashSizeBytes = static_cast<qint64>(effectiveFlashMB) * 1024 * 1024;
    if (fwData.size() > flashSizeBytes) {
        emit debugMessageReceived(QString("[SPI Flash] firmware too large (%1 bytes) for %2MB flash")
                                .arg(fwData.size())
                                .arg(effectiveFlashMB));
        return false;
    }

    const QFileInfo fwInfo(firmwarePath);
    QString imageBase = fwInfo.completeBaseName();
    if (imageBase.endsWith(".merged", Qt::CaseInsensitive)) {
        imageBase.chop(QStringLiteral(".merged").size());
    }
    spiFlashImagePath = QDir(fwInfo.absolutePath()).filePath(
        QString("%1.qemu_flash_%2MB.bin").arg(imageBase).arg(effectiveFlashMB));

    const QFileInfo flashInfo(spiFlashImagePath);
    const bool flashExists = flashInfo.exists();
    const bool flashSizeOk = flashExists && flashInfo.size() == flashSizeBytes;
    const bool sourceNewerThanFlash = flashExists && fwInfo.lastModified() > flashInfo.lastModified();
    const bool shouldInitialize = !flashExists || !flashSizeOk || sourceNewerThanFlash;

    if (shouldInitialize) {
        QFile flashFile(spiFlashImagePath);
        if (!flashFile.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
            emit debugMessageReceived(QString("[SPI Flash] failed to create flash image: %1").arg(spiFlashImagePath));
            return false;
        }

        QByteArray fillChunk(1024 * 1024, static_cast<char>(0xFF));
        int remainingMB = effectiveFlashMB;
        while (remainingMB-- > 0) {
            if (flashFile.write(fillChunk) != fillChunk.size()) {
                emit debugMessageReceived("[SPI Flash] failed while initializing flash image");
                flashFile.close();
                return false;
            }
        }

        if (!flashFile.seek(0)) {
            emit debugMessageReceived("[SPI Flash] failed to seek flash image");
            flashFile.close();
            return false;
        }

        if (flashFile.write(fwData) != fwData.size()) {
            emit debugMessageReceived("[SPI Flash] failed writing merged firmware into flash image");
            flashFile.close();
            return false;
        }

        flashFile.flush();
        flashFile.close();

        const QString initReason = !flashExists
            ? QStringLiteral("new image")
            : (!flashSizeOk ? QStringLiteral("size changed") : QStringLiteral("firmware updated"));
        emit debugMessageReceived(QString("[SPI Flash] initialized virtual flash (%1): %2")
                                .arg(initReason, spiFlashImagePath));
    } else {
        emit debugMessageReceived(QString("[SPI Flash] reusing persistent virtual flash: %1")
                                .arg(spiFlashImagePath));
    }

    auto readLe32 = [](const QByteArray &data, int off) -> quint32 {
        if (off < 0 || off + 4 > data.size()) {
            return 0;
        }
        return static_cast<quint32>(static_cast<quint8>(data.at(off)))
            | (static_cast<quint32>(static_cast<quint8>(data.at(off + 1))) << 8)
            | (static_cast<quint32>(static_cast<quint8>(data.at(off + 2))) << 16)
            | (static_cast<quint32>(static_cast<quint8>(data.at(off + 3))) << 24);
    };

    auto normalizeCoredumpHeader = [this, &readLe32, flashSizeBytes](const QString &flashPath) -> bool {
        static constexpr int kPartitionTableOffset = 0x8000;
        static constexpr int kPartitionEntrySize = 32;
        static constexpr int kPartitionTableSpan = 0xC00;

        QFile flashFile(flashPath);
        if (!flashFile.open(QIODevice::ReadWrite)) {
            emit debugMessageReceived(QString("[SPI Flash] failed to open virtual flash for coredump normalization: %1")
                                    .arg(flashPath));
            return false;
        }

        if (!flashFile.seek(kPartitionTableOffset)) {
            flashFile.close();
            return false;
        }

        const QByteArray part = flashFile.read(kPartitionTableSpan);
        if (part.size() < kPartitionEntrySize) {
            flashFile.close();
            return false;
        }

        qint64 coredumpOffset = -1;
        for (int entryOff = 0; entryOff + kPartitionEntrySize <= part.size(); entryOff += kPartitionEntrySize) {
            const quint16 magic = static_cast<quint16>(static_cast<quint8>(part.at(entryOff)))
                | (static_cast<quint16>(static_cast<quint8>(part.at(entryOff + 1))) << 8);
            if (magic != 0x50AA) {
                continue;
            }

            const quint8 type = static_cast<quint8>(part.at(entryOff + 2));
            const quint8 subtype = static_cast<quint8>(part.at(entryOff + 3));
            if (type == 0x01 && subtype == 0x03) {
                const qint64 off = static_cast<qint64>(readLe32(part, entryOff + 4));
                const qint64 size = static_cast<qint64>(readLe32(part, entryOff + 8));
                if (off >= 0 && size >= 4 && (off + size) <= flashSizeBytes) {
                    coredumpOffset = off;
                }
                break;
            }
        }

        if (coredumpOffset < 0) {
            flashFile.close();
            return true;
        }

        if (!flashFile.seek(coredumpOffset)) {
            flashFile.close();
            return false;
        }

        const QByteArray before = flashFile.read(4);
        if (!flashFile.seek(coredumpOffset)) {
            flashFile.close();
            return false;
        }

        const QByteArray blankHeader(4, static_cast<char>(0xFF));
        if (flashFile.write(blankHeader) != blankHeader.size()) {
            flashFile.close();
            return false;
        }

        flashFile.flush();
        flashFile.close();

        if (before.size() == 4) {
            const quint32 prev = static_cast<quint32>(static_cast<quint8>(before.at(0)))
                               | (static_cast<quint32>(static_cast<quint8>(before.at(1))) << 8)
                               | (static_cast<quint32>(static_cast<quint8>(before.at(2))) << 16)
                               | (static_cast<quint32>(static_cast<quint8>(before.at(3))) << 24);
            if (prev != 0xFFFFFFFFu) {
                emit debugMessageReceived(QString("[SPI Flash] normalized coredump header at 0x%1 (prev=0x%2)")
                                        .arg(QString::number(coredumpOffset, 16).toUpper())
                                        .arg(QString::number(prev, 16).toUpper()));
            }
        }

        return true;
    };

    if (!normalizeCoredumpHeader(spiFlashImagePath)) {
        emit debugMessageReceived("[SPI Flash] warning: coredump header normalization failed");
    }

    flashPathOut = spiFlashImagePath;
    return true;
}

void QemuController::stopQemu()
{
    stoppingProcess = true;
    qmpDeadlineTimer->stop();
    if (qemuProcess->state() != QProcess::NotRunning && status.phase != RuntimePhase::Error)
        setRuntimePhase(RuntimePhase::Stopping, QStringLiteral("Stopping QEMU and closing its transports."));
    disconnectQmp();

    if (qemuProcess->state() == QProcess::NotRunning) {
        stoppingProcess = false;
        return;
    }

    qemuProcess->terminate();
    if (!qemuProcess->waitForFinished(2000)) {
        qemuProcess->kill();
        qemuProcess->waitForFinished(1000);
    }
    stoppingProcess = false;
}

QString QemuController::resolveQemuBinary() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName =
#if defined(Q_OS_WIN)
        QStringLiteral("qemu-system-xtensa.exe");
#else
        QStringLiteral("qemu-system-xtensa");
#endif

    const QString envBin = qEnvironmentVariable("ESP32S3_QEMU_BIN");
    if (!envBin.isEmpty() && QFileInfo::exists(envBin)) {
        return QFileInfo(envBin).absoluteFilePath();
    }

    const QStringList candidates = {
        QDir::cleanPath(appDir + "/qemu/" + exeName),
        QDir::cleanPath(appDir + "/../qemu/" + exeName),
        QDir::cleanPath(appDir + "/../../qemu/" + exeName),
        QDir::cleanPath(appDir + "/" + exeName),
        QDir::cleanPath(appDir + "/../" + exeName),
        QDir::cleanPath(appDir + "/../../" + exeName),
        QDir::cleanPath(appDir + "/../../qemu/build/" + exeName),
        QDir::cleanPath(appDir + "/../../../qemu/build/" + exeName),
        QDir::cleanPath(QDir::currentPath() + "/../qemu/build/" + exeName),
        QDir::cleanPath(QDir::currentPath() + "/qemu/build/" + exeName),
        QDir::cleanPath(QDir::currentPath() + "/../../qemu/build/" + exeName)
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    const QString fromPath = QStandardPaths::findExecutable(exeName);
    if (!fromPath.isEmpty()) {
        return fromPath;
    }

    return QString();
}

void QemuController::handleQemuOutputChunk(const QString &chunk)
{
    serialBuffer += chunk;
    const QStringList lines = splitLines(serialBuffer);
    for (const QString &line : lines) {
        /* Bridge events are consumed silently — never shown in the serial console */
        if (ingestBridgeEventLine(line)) {
            continue;
        }
        emit serialLineReceived(line);
    }
}

bool QemuController::ingestBridgeEventLine(const QString &line)
{
    const QString trimmed = line.trimmed();

    auto parseAndEmit = [&](const QString &prefix, auto emitter) -> bool {
        if (!trimmed.startsWith(prefix)) {
            return false;
        }
        const QString jsonText = trimmed.mid(prefix.size()).trimmed();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }
        emitter(doc.object());
        return true;
    };

    if (parseAndEmit("[PERIPH][I2C]", [this](const QJsonObject &obj) { emit i2cTransferRequested(obj); })) {
        return true;
    }
    if (parseAndEmit("[PERIPH][SPI]", [this](const QJsonObject &obj) { emit spiTransferRequested(obj); })) {
        return true;
    }
    if (parseAndEmit("[PERIPH][UART]", [this](const QJsonObject &obj) { emit uartTxRequested(obj); })) {
        return true;
    }

    return false;
}

bool QemuController::qmpTransportExpected() const
{
    return bootMode == 0 && !stoppingProcess && status.phase != RuntimePhase::Error
        && status.phase != RuntimePhase::Stopping && status.phase != RuntimePhase::Stopped
        && (qmpTestEndpoint || qemuProcess->state() == QProcess::Running);
}

void QemuController::connectQmp()
{
    if (!qmpTransportExpected() || qmpTcpSocket->state() == QAbstractSocket::ConnectedState
        || qmpTcpSocket->state() == QAbstractSocket::ConnectingState) return;
    if (qmpConnectionAttempts >= 40) {
        failRuntime(QStringLiteral("QMP connection deadline exceeded. Restart the simulation to retry."), true);
        return;
    }
    ++qmpConnectionAttempts;
    qmpBuffer.clear();
    qmpReady = false;
    pendingCapabilitiesCb = -1;
    pendingQmpCommands.clear();
    clearNativeCircuit(true, true, QStringLiteral("Awaiting this QMP connection's native electrical handshake."));
    qmpTcpSocket->abort();
    qmpTcpSocket->connectToHost(QStringLiteral("127.0.0.1"), qmpPort);
}

void QemuController::disconnectQmp()
{
    qmpReady = false;
    liveTimer->stop();
    qmpTcpSocket->abort();
    qmpBuffer.clear();
    pendingSnapshotCb = -1;
    pendingRegsCb = -1;
    pendingMemCb = -1;
    pendingCapabilitiesCb = -1;
    pendingInitializationProbes = 0;
    pendingBridgeSettings = 0;
    collectingBridgeSettings = false;
    pendingQmpCommands.clear();
    initializationComplete = false;
    executionStatusObserved = false;
    executionStoppedObserved = false;
    clearNativeCircuit(true, true, QStringLiteral("Native electrical transport is disconnected."));
}

void QemuController::processQmpBuffer()
{
    int idx = qmpBuffer.indexOf('\n');
    while (idx >= 0) {
        const QString line = qmpBuffer.left(idx).trimmed();
        qmpBuffer.remove(0, idx + 1);

        if (!line.isEmpty()) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                handleQmpMessage(doc.object());
            }
        }

        idx = qmpBuffer.indexOf('\n');
    }
}

void QemuController::handleQmpMessage(const QJsonObject &obj)
{
    if (obj.contains("QMP")) {
        if (pendingCapabilitiesCb >= 0 || qmpReady) return;
        const auto version = obj.value("QMP").toObject().value("version").toObject();
        const auto number = version.value("qemu").toObject();
        if (number.contains("major") && number.contains("minor") && number.contains("micro"))
            runtimeIdentity = QString("QEMU %1.%2.%3 %4")
                .arg(number.value("major").toInt()).arg(number.value("minor").toInt())
                .arg(number.value("micro").toInt()).arg(version.value("package").toString()).trimmed();
        setRuntimePhase(RuntimePhase::Initializing, QStringLiteral("Negotiating QMP and discovering this runtime's interfaces."));
        updateRuntimeCapabilities();
        pendingCapabilitiesCb = qmpSeq++;
        sendQmpCommand("qmp_capabilities", {}, pendingCapabilitiesCb);
        return;
    }

    if (obj.contains("event")) {
        const QString event = obj.value("event").toString();
        if (!qmpReady || stoppingProcess || status.phase == RuntimePhase::Stopped
            || status.phase == RuntimePhase::Stopping) return;
        if (event == "STOP" || event == "RESUME") {
            ++observationRevision;
            executionStatusObserved = true;
            executionStoppedObserved = event == "STOP";
            if (event == "RESUME") {
                debuggerWaitPending = false;
                setRuntimePhase(RuntimePhase::Running, QStringLiteral("QMP reports firmware execution resumed."));
            } else {
                setRuntimePhase(debuggerWaitPending ? RuntimePhase::WaitingForDebugger : RuntimePhase::Paused,
                                QStringLiteral("QMP reports CPU execution stopped."));
            }
            if (initializationComplete) qmpDeadlineTimer->stop();
            updateRuntimeCapabilities();
        } else if (event == "RESET") {
            ++status.resetEpoch;
            ++observationRevision;
            executionStatusObserved = false;
            executionStoppedObserved = false;
            clearNativeCircuit(false, false, QStringLiteral("Reset invalidated pending native electrical replies."));
            pendingQmpCommands.clear();
            pendingRegsCb = pendingMemCb = pendingSnapshotCb = -1;
            setRuntimePhase(RuntimePhase::Initializing, QStringLiteral("QMP reported a target reset; refreshing execution state."));
            if (!initializationComplete) startBridgeProbes();
            else requestRuntimeStatus();
            qmpDeadlineTimer->start();
            updateRuntimeCapabilities();
        } else if (event == "SHUTDOWN") {
            ++observationRevision;
            setRuntimePhase(RuntimePhase::Stopping, QStringLiteral("QMP reported guest shutdown; waiting for process exit."));
            liveTimer->stop();
            updateRuntimeCapabilities();
        } else if (event == "GUEST_PANICKED") {
            failRuntime(QStringLiteral("QMP reported a guest panic; inspect the serial log and reset the target."));
        }
        return;
    }
    if (!obj.contains("id") || (!obj.contains("return") && !obj.contains("error"))) return;
    const int id = obj.value("id").toInt(-1);
    if (!pendingQmpCommands.contains(id)) return; // Late/unknown acknowledgements cannot establish state.
    const auto command = pendingQmpCommands.take(id);
    if (command.epoch != status.resetEpoch) return;
    const bool error = obj.contains("error");
    const QString errorText = obj.value("error").toObject().value("desc").toString("Unknown QMP error");

    if (id == pendingCapabilitiesCb) {
        pendingCapabilitiesCb = -1;
        if (error) { failRuntime("QMP negotiation failed: " + errorText, true); return; }
        qmpReady = true;
        emit debugMessageReceived("[QMP] capabilities enabled");
        startBridgeProbes();
        return;
    }
    const QString path = command.arguments.value("path").toString();
    if (path == kElectricalPath && handleNativeCircuitReply(id, command, obj)) return;
    if (bridgeProbes.contains(path) && bridgeProbes[path].remaining > 0
        && (command.execute == "qom-list" || command.execute == "qom-get")) {
        auto &probe = bridgeProbes[path];
        if (!error) {
            const QString property = command.arguments.value("property").toString();
            if (command.execute == "qom-list") {
                for (const auto &value : obj.value("return").toArray())
                    probe.properties.insert(value.toObject().value("name").toString());
            } else if (property == "type") probe.type = obj.value("return").toString();
            else if (property == "bridge-enabled") probe.enabled = obj.value("return").toBool();
        }
        --probe.remaining;
        --pendingInitializationProbes;
        finishRuntimeInitialization();
        return;
    }
    if (command.initializationSetting) {
        --pendingBridgeSettings;
        if (error) { failRuntime("Peripheral initialization failed: " + errorText, true); return; }
        finishRuntimeInitialization();
        return;
    }
    if (command.execute == "query-status") {
        if (command.revision != observationRevision) return;
        if (error) { failRuntime("QMP execution-status query failed: " + errorText, true); return; }
        const auto observed = obj.value("return").toObject();
        if (!observed.value("running").isBool() || !observed.value("status").isString()) {
            failRuntime(QStringLiteral("QMP returned an invalid execution-status response."), true);
            return;
        }
        const QString runState = observed.value("status").toString();
        executionStatusObserved = true;
        executionStoppedObserved = !observed.value("running").toBool()
            && (runState == "paused" || runState == "prelaunch" || runState == "debug");
        if (observed.value("running").toBool() && runState == "running") {
            debuggerWaitPending = false;
            setRuntimePhase(RuntimePhase::Running, QStringLiteral("QMP confirms firmware is running."));
        } else if (runState == "shutdown") {
            setRuntimePhase(RuntimePhase::Stopping, QStringLiteral("QMP confirms guest shutdown."));
        } else if (runState == "internal-error" || runState == "io-error" || runState == "guest-panicked") {
            failRuntime(QString("QEMU reports %1; inspect the log before resuming.").arg(runState));
        } else {
            setRuntimePhase(debuggerWaitPending ? RuntimePhase::WaitingForDebugger : RuntimePhase::Paused,
                            QString("QMP confirms execution is stopped (%1).").arg(runState));
        }
        if (initializationComplete) qmpDeadlineTimer->stop();
        updateRuntimeCapabilities();
        return;
    }
    if (command.execute == "stop" || command.execute == "cont") {
        if (command.revision != observationRevision) return;
        if (error) failRuntime("Execution control failed: " + errorText);
        else requestRuntimeStatus();
        return;
    }
    if (obj.contains("error")) {
        if (id == pendingSnapshotCb) {
            pendingSnapshotCb = -1;
        }
        if (id == pendingRegsCb) {
            pendingRegsCb = -1;
        }
        if (id == pendingMemCb) {
            pendingMemCb = -1;
        }
        emit debugMessageReceived(QString("[QMP] command error for id=%1: %2").arg(id).arg(errorText));
        return;
    }

    if (id == pendingRegsCb && obj.contains("return")) {
        pendingRegsCb = -1;
        parseRegisterDump(obj.value("return").toString(), pendingPcText, pendingScalars, pendingVectors);

        pendingMemCb = qmpSeq++;
        QJsonObject args;
        args["command-line"] = QString("xp /8wx %1").arg(memoryInspectBase);
        sendQmpCommand("human-monitor-command", args, pendingMemCb);
        return;
    }

    if (id == pendingMemCb && obj.contains("return")) {
        pendingMemCb = -1;
        const QStringList mem = parseMemoryDump(obj.value("return").toString());
        emit cpuSnapshotUpdated(pendingPcText, pendingScalars, pendingVectors, mem);
        return;
    }
}

void QemuController::sendQmpCommand(const QString &execute,
                                    const QJsonObject &arguments,
                                    int callbackId)
{
    if (qmpTcpSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    if (execute == "query-status") {
        for (const auto &pending : pendingQmpCommands)
            if (pending.execute == execute && pending.epoch == status.resetEpoch
                && pending.revision == observationRevision) return;
    }

    QJsonObject obj;
    obj["execute"] = execute;
    if (!arguments.isEmpty()) {
        obj["arguments"] = arguments;
    }
    const int id = callbackId >= 0 ? callbackId : qmpSeq++;
    obj["id"] = id;
    const bool initializationSetting = collectingBridgeSettings && execute == "qom-set";
    pendingQmpCommands.insert(id, {execute, arguments, observationRevision, status.resetEpoch,
                                   initializationSetting, qmpCommandClock.elapsed(), nativeCircuitContext});
    if (initializationSetting) ++pendingBridgeSettings;

    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    qmpTcpSocket->write(data);
    qmpTcpSocket->flush();
}

void QemuController::requestRuntimeStatus()
{
    if (qmpReady) sendQmpCommand("query-status");
}

void QemuController::startBridgeProbes()
{
    bridgeProbes.clear();
    nativeI2cBuses.clear();
    nativeSpiControllers.clear();
    nativePeripheralBridge = false;
    pendingInitializationProbes = 0;
    pendingBridgeSettings = 0;
    bridgeSettingsReplayed = false;
    initializationComplete = false;
    for (int bus = 0; bus < I2C_BUS_COUNT; ++bus) {
        const QString path = QString("/machine/soc/i2c%1/i2c/child[0]").arg(bus);
        bridgeProbes[path].remaining = 2;
        pendingInitializationProbes += 2;
        sendQmpCommand("qom-get", {{"path", path}, {"property", "type"}});
        sendQmpCommand("qom-list", {{"path", path}});
    }
    for (int spi = 2; spi <= 3; ++spi) {
        const QString path = QString("/machine/soc/gpspi%1").arg(spi);
        bridgeProbes[path].remaining = 3;
        pendingInitializationProbes += 3;
        sendQmpCommand("qom-get", {{"path", path}, {"property", "type"}});
        sendQmpCommand("qom-list", {{"path", path}});
        sendQmpCommand("qom-get", {{"path", path}, {"property", "bridge-enabled"}});
    }
    clearNativeCircuit(true, true, QStringLiteral("Discovering native electrical QOM properties and snapshot ABI."));
    pendingNativeCircuitDiscovery = 2;
    pendingInitializationProbes += pendingNativeCircuitDiscovery;
    sendQmpCommand("qom-list", {{"path", kElectricalPath}});
    sendQmpCommand("qom-get", {{"path", kElectricalPath}, {"property", "snapshot-json"}});
}

void QemuController::finishRuntimeInitialization()
{
    if (pendingInitializationProbes > 0 || initializationComplete) return;
    if (!bridgeSettingsReplayed) {
        bridgeSettingsReplayed = true;
        for (int bus = 0; bus < I2C_BUS_COUNT; ++bus) {
            const auto probe = bridgeProbes.value(QString("/machine/soc/i2c%1/i2c/child[0]").arg(bus));
            if (probe.type == "esp32s3.i2c-bridge" && probe.properties.contains("registered-addrs")
                && probe.properties.contains("read-response-map")) nativeI2cBuses.insert(bus);
        }
        for (int spi = 2; spi <= 3; ++spi) {
            const auto probe = bridgeProbes.value(QString("/machine/soc/gpspi%1").arg(spi));
            if (probe.type == "esp32s3.gpspi" && probe.enabled && probe.properties.contains("bridge-dc-gpio"))
                nativeSpiControllers.insert(QString("spi%1").arg(spi));
        }
        nativePeripheralBridge = !nativeI2cBuses.isEmpty() || !nativeSpiControllers.isEmpty();
        collectingBridgeSettings = true;
        pushAllI2cBridgeAddresses();
        for (int bus = 0; bus < I2C_BUS_COUNT; ++bus)
            if (!i2cResponseMaps[bus].isEmpty()) setI2cBridgeResponseMap(bus, i2cResponseMaps[bus]);
        const auto dcGpios = spiDcGpios;
        for (auto it = dcGpios.cbegin(); it != dcGpios.cend(); ++it) setSpiDcGpio(it.key(), it.value());
        collectingBridgeSettings = false;
        emit debugMessageReceived(nativePeripheralBridge
            ? "[Peripherals] Runtime bridge subtypes detected; cached I2C / SPI TX paths have explicit limits."
            : "[Peripherals] Native bus bridge unavailable in this QEMU. Device panels run independently; firmware bus connections require the custom QEMU extension.");
        emit peripheralBridgeAvailable(nativePeripheralBridge);
        updateRuntimeCapabilities();
    }
    if (pendingBridgeSettings > 0) return;
    initializationComplete = true;
    if (qmpBootReleasePending) {
        qmpBootReleasePending = false;
        ++observationRevision;
        sendQmpCommand("cont");
    }
    requestRuntimeStatus();
}

void QemuController::pollLiveState()
{
    requestRuntimeStatus();
    requestCpuSnapshot();
}

QString QemuController::resolveQemuDataDir() const
{
    if (qemuBinaryPath.isEmpty()) {
        return QString();
    }

    const QString binDir = QFileInfo(qemuBinaryPath).absolutePath();
    const QString romName = QStringLiteral("esp32s3_rev0_rom.bin");

    /* Search candidate directories where the ROM binary might live.
       Covers: packaged layout, standard QEMU install, and source build tree. */
    const QStringList candidates = {
        QDir::cleanPath(binDir + "/share/qemu"),
        QDir::cleanPath(binDir + "/../share/qemu"),
        QDir::cleanPath(binDir + "/../pc-bios"),
        QDir::cleanPath(binDir + "/../../pc-bios"),
        binDir,
    };

    for (const QString &dir : candidates) {
        if (QFileInfo::exists(QDir::cleanPath(dir + "/" + romName))) {
            return QFileInfo(dir).absoluteFilePath();
        }
    }

    /* Fall back to app directory based search (similar to resolveQemuBinary). */
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList appCandidates = {
        QDir::cleanPath(appDir + "/qemu/share/qemu"),
        QDir::cleanPath(appDir + "/../qemu/share/qemu"),
        QDir::cleanPath(appDir + "/../../qemu/pc-bios"),
        QDir::cleanPath(appDir + "/../../../qemu/pc-bios"),
        QDir::cleanPath(QDir::currentPath() + "/../qemu/pc-bios"),
        QDir::cleanPath(QDir::currentPath() + "/../../qemu/pc-bios"),
    };

    for (const QString &dir : appCandidates) {
        if (QFileInfo::exists(QDir::cleanPath(dir + "/" + romName))) {
            return QFileInfo(dir).absoluteFilePath();
        }
    }

    return QString();
}

void QemuController::parseRegisterDump(const QString &dump,
                                       QString &pcText,
                                       QStringList &scalars,
                                       QStringList &vectors) const
{
    QRegularExpression pcRe("\\bPC\\s*[:=]\\s*([0-9A-Fa-fx]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression scalarRe("\\bA0?([0-9]|1[0-5])\\s*[:=]\\s*([0-9A-Fa-fx]+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression floatRe("\\bF0?([0-7])\\s*[:=]\\s*([0-9A-Fa-fx]+)", QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch pcMatch = pcRe.match(dump);
    if (pcMatch.hasMatch()) {
        QString val = pcMatch.captured(1).toUpper();
        if (!val.startsWith("0X")) {
            val.prepend("0x");
        }
        pcText = val;
    }

    QRegularExpressionMatchIterator it = scalarRe.globalMatch(dump);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const int index = m.captured(1).toInt();
        if (index >= 0 && index < scalars.size()) {
            QString val = m.captured(2).toUpper();
            if (!val.startsWith("0X")) {
                val.prepend("0x");
            }
            scalars[index] = val;
        }
    }

    it = floatRe.globalMatch(dump);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        const int index = m.captured(1).toInt();
        if (index >= 0 && index < vectors.size()) {
            QString val = m.captured(2).toUpper();
            if (!val.startsWith("0X")) {
                val.prepend("0x");
            }
            vectors[index] = val;
        }
    }
}

QStringList QemuController::parseMemoryDump(const QString &dump) const
{
    QStringList out;
    out.fill("unavailable", 8);

    QRegularExpression wordRe("0x[0-9A-Fa-f]+|[0-9A-Fa-f]{8}");
    const QStringList lines = dump.split('\n', Qt::SkipEmptyParts);
    int pos = 0;
    for (const QString &line : lines) {
        const int colonPos = line.indexOf(':');
        if (colonPos < 0) {
            continue;
        }

        QRegularExpressionMatchIterator it = wordRe.globalMatch(line.mid(colonPos + 1));
        while (it.hasNext() && pos < out.size()) {
            const QRegularExpressionMatch m = it.next();
            QString val = m.captured(0).toUpper();
            if (!val.startsWith("0X")) {
                val.prepend("0x");
            }
            out[pos++] = val;
        }

        if (pos >= out.size()) {
            break;
        }
    }

    return out;
}
