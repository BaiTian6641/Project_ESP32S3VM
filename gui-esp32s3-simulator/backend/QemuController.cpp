#include "QemuController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

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
            qmpSocket(new QLocalSocket(this)),
            liveTimer(new QTimer(this)),
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
        } else {
            QTimer::singleShot(250, this, &QemuController::connectQmp);
        }
    });

    connect(qemuProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit debugMessageReceived(QString("[QEMU] process error: %1").arg(static_cast<int>(error)));
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
                liveTimer->stop();
                autoDownloadSwitchPending = false;
                emit qemuStopped();
            });

    connect(qmpSocket, &QLocalSocket::connected, this, [this]() {
        emit debugMessageReceived("[QMP] connected");
    });

    connect(qmpSocket, &QLocalSocket::readyRead, this, [this]() {
        const QByteArray bytes = qmpSocket->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        qmpBuffer += QString::fromUtf8(bytes);
        processQmpBuffer();
    });

    connect(qmpSocket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError e) {
        emit debugMessageReceived(QString("[QMP] socket error: %1").arg(static_cast<int>(e)));
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

    pendingPcText = "0x00000000";
    pendingScalars.fill("0x00000000", 16);
    pendingVectors.fill("0x00000000", 8);

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

    if (qemuProcess->state() == QProcess::Running) {
        qemuProcess->write((line + "\n").toUtf8());
    }
}

void QemuController::pauseExecution()
{
    if (!qmpReady) {
        return;
    }
    sendQmpCommand("stop");
    emit debugMessageReceived("[Debug] pause requested");
}

void QemuController::continueExecution()
{
    if (!qmpReady) {
        return;
    }
    sendQmpCommand("cont");
    emit debugMessageReceived("[Debug] continue requested");
}

void QemuController::stepInstruction()
{
    if (!qmpReady) {
        return;
    }
    QJsonObject args;
    args["command-line"] = "si";
    sendQmpCommand("human-monitor-command", args);
    emit debugMessageReceived("[Debug] single-step requested");
    requestCpuSnapshot();
}

void QemuController::addBreakpoint(const QString &addressText)
{
    if (!qmpReady) {
        return;
    }
    QJsonObject args;
    args["command-line"] = QString("break %1").arg(addressText.trimmed());
    sendQmpCommand("human-monitor-command", args);
    emit debugMessageReceived(QString("[Debug] breakpoint set at %1").arg(addressText.trimmed()));
}

void QemuController::clearBreakpoints()
{
    if (!qmpReady) {
        return;
    }
    QJsonObject args;
    args["command-line"] = "delete";
    sendQmpCommand("human-monitor-command", args);
    emit debugMessageReceived("[Debug] all breakpoints clear requested");
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
    if (!qmpReady || busIndex < 0 || busIndex >= I2C_BUS_COUNT) {
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
    startQemuWithFirmware(pendingFirmware);
}

void QemuController::setBootMode(int modeIndex)
{
    bootMode = modeIndex;
    const QString modeName = (bootMode == 0) ? "Normal Boot" : "Download Boot";
    emit debugMessageReceived(QString("[Control] Boot mode set: %1").arg(modeName));
}

void QemuController::loadFirmware(const QString &path)
{
    pendingFirmware = path;
    emit debugMessageReceived(QString("[Control] Firmware selected: %1").arg(pendingFirmware));
    startQemuWithFirmware(pendingFirmware);
}

void QemuController::startQemuWithFirmware(const QString &firmwarePath)
{
    stopQemu();
    uartIngressHistory.clear();
    autoDownloadSwitchPending = false;

    QFileInfo fwInfo(firmwarePath);
    if (!fwInfo.exists()) {
        emit debugMessageReceived(QString("[QEMU] Firmware file not found: %1").arg(firmwarePath));
        return;
    }

    qemuBinaryPath = resolveQemuBinary();
    if (qemuBinaryPath.isEmpty()) {
        emit debugMessageReceived("[QEMU] qemu-system-xtensa not found. Set ESP32S3_QEMU_BIN or build qemu/build/qemu-system-xtensa");
        return;
    }

    if (uartMasterFd < 0 && !setupUartPty()) {
        emit debugMessageReceived("[Serial] PTY bridge unavailable, continuing with integrated GUI serial only");
    }

    flushUartBridgeBuffers();

    QStringList args;
    QString machineArg = (bootMode == 1)
        ? QString("esp32s3,boot-mode=download")
        : QString("esp32s3,boot-mode=flash");

    if (!customBaseMac.isEmpty()) {
        machineArg += QString(",mac=%1").arg(customBaseMac);
    }
    if (chipRevisionEnabled) {
        machineArg += QString(",chip-revision=%1").arg(chipRevision);
    }
    if (psramEnabled) {
        machineArg += QString(",psram-mode=%1").arg(psramMode);
    }

    args << "-M" << "esp32s3"
            << "-accel" << "tcg,thread=multi,tb-size=1024"
         << "-display" << "none"
         << "-monitor" << "none"
            << "-serial" << "stdio"
         ;

#if !defined(Q_OS_WIN)
    QFile::remove(qmpSocketPath());
    args << "-qmp" << QString("unix:%1,server=on,wait=off").arg(qmpSocketPath());
#else
    emit debugMessageReceived("[QMP] disabled on Windows in this build; CPU debug controls are unavailable");
#endif

    args[1] = machineArg;

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
            return;
        }

        QString flashPath;
        if (!prepareSpiFlashImage(firmwarePath, flashPath)) {
            return;
        }

        args << "-drive" << QString("file=%1,if=mtd,format=raw").arg(flashPath);
        emit debugMessageReceived(QString("[QEMU] SPI flash image prepared: %1 (%2MB)")
                                .arg(flashPath)
                                .arg(spiFlashSizeMB));
    } else {
        emit debugMessageReceived("[QEMU] Unsupported firmware type; use .elf or .bin");
        return;
    }

    if (gdbEnabled) {
        args << "-gdb" << QString("tcp::%1").arg(gdbPort);
        if (gdbWaitForAttach) {
            args << "-S";
        }
    }

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

    emit debugMessageReceived(QString("[QEMU] Launch: %1 %2").arg(qemuBinaryPath, args.join(' ')));
    if (gdbEnabled) {
        const QString endpoint = QString("127.0.0.1:%1").arg(gdbPort);
        const QString mode = gdbWaitForAttach ? "waiting for debugger" : "running";
        emit debugStatusUpdated(QString("[GDB] endpoint %1 (%2)").arg(endpoint, mode));
    } else {
        emit debugStatusUpdated("[GDB] disabled");
    }
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

    if (fwData.size() >= 4) {
        const quint8 magic = static_cast<quint8>(fwData.at(0));
        const quint8 flashCfg = static_cast<quint8>(fwData.at(3));
        if (magic == 0xE9) {
            const quint8 sizeNibble = (flashCfg >> 4) & 0x0F;
            int imageFlashSizeMB = 0;
            switch (sizeNibble) {
            case 0x0: imageFlashSizeMB = 1; break;
            case 0x1: imageFlashSizeMB = 2; break;
            case 0x2: imageFlashSizeMB = 4; break;
            case 0x3: imageFlashSizeMB = 8; break;
            case 0x4: imageFlashSizeMB = 16; break;
            case 0x5: imageFlashSizeMB = 32; break;
            case 0x6: imageFlashSizeMB = 64; break;
            case 0x7: imageFlashSizeMB = 128; break;
            default: break;
            }

            if (imageFlashSizeMB > 0 && spiFlashSizeMB < imageFlashSizeMB) {
                emit debugMessageReceived(QString("[SPI Flash] warning: firmware header indicates %1MB flash, but current simulator flash is %2MB")
                                        .arg(imageFlashSizeMB)
                                        .arg(spiFlashSizeMB));
            }
        }
    }

    const qint64 flashSizeBytes = static_cast<qint64>(spiFlashSizeMB) * 1024 * 1024;
    if (fwData.size() > flashSizeBytes) {
        emit debugMessageReceived(QString("[SPI Flash] firmware too large (%1 bytes) for %2MB flash")
                                .arg(fwData.size())
                                .arg(spiFlashSizeMB));
        return false;
    }

    spiFlashImagePath = QDir::tempPath() + QString("/esp32s3_gui_flash_%1.bin").arg(QCoreApplication::applicationPid());
    QFile flashFile(spiFlashImagePath);
    if (!flashFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit debugMessageReceived(QString("[SPI Flash] failed to create flash image: %1").arg(spiFlashImagePath));
        return false;
    }

    QByteArray fillChunk(1024 * 1024, static_cast<char>(0xFF));
    int remainingMB = spiFlashSizeMB;
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
        emit debugMessageReceived("[SPI Flash] failed writing firmware into flash image");
        flashFile.close();
        return false;
    }

    flashFile.close();
    flashPathOut = spiFlashImagePath;
    return true;
}

void QemuController::stopQemu()
{
    disconnectQmp();

    if (qemuProcess->state() == QProcess::NotRunning) {
        return;
    }

    qemuProcess->terminate();
    if (!qemuProcess->waitForFinished(2000)) {
        qemuProcess->kill();
        qemuProcess->waitForFinished(1000);
    }
}

QString QemuController::resolveQemuBinary() const
{
    const QString envBin = qEnvironmentVariable("ESP32S3_QEMU_BIN");
    if (!envBin.isEmpty() && QFileInfo::exists(envBin)) {
        return QFileInfo(envBin).absoluteFilePath();
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName =
#if defined(Q_OS_WIN)
        QStringLiteral("qemu-system-xtensa.exe");
#else
        QStringLiteral("qemu-system-xtensa");
#endif

    const QStringList candidates = {
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

void QemuController::connectQmp()
{
#if defined(Q_OS_WIN)
    return;
#endif

    if (qemuProcess->state() != QProcess::Running) {
        return;
    }
    if (qmpSocket->state() == QLocalSocket::ConnectedState) {
        return;
    }

    qmpBuffer.clear();
    qmpReady = false;
    qmpSocket->abort();
    qmpSocket->connectToServer(qmpSocketPath());
}

void QemuController::disconnectQmp()
{
    qmpReady = false;
    liveTimer->stop();
    qmpSocket->abort();
    qmpBuffer.clear();
    pendingSnapshotCb = -1;
    pendingRegsCb = -1;
    pendingMemCb = -1;
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
        sendQmpCommand("qmp_capabilities");
        return;
    }

    if (obj.contains("return") && !qmpReady) {
        qmpReady = true;
        emit debugMessageReceived("[QMP] capabilities enabled");
        /* Push any pending I2C bridge addresses now that QMP is live */
        pushAllI2cBridgeAddresses();
        return;
    }

    if (!obj.contains("id")) {
        return;
    }

    const int id = obj.value("id").toInt(-1);
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
        emit debugMessageReceived(QString("[QMP] command error for id=%1").arg(id));
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
    if (qmpSocket->state() != QLocalSocket::ConnectedState) {
        return;
    }

    QJsonObject obj;
    obj["execute"] = execute;
    if (!arguments.isEmpty()) {
        obj["arguments"] = arguments;
    }
    if (callbackId >= 0) {
        obj["id"] = callbackId;
    }

    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    qmpSocket->write(data);
    qmpSocket->flush();
}

void QemuController::pollLiveState()
{
    requestCpuSnapshot();
}

QString QemuController::qmpSocketPath() const
{
    return QDir::tempPath() + QString("/esp32s3_gui_qmp_%1.sock").arg(QCoreApplication::applicationPid());
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
    out.fill("0x00000000", 8);

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
