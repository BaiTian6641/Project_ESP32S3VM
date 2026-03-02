#include "PeripheralManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

PeripheralManager::PeripheralManager(QObject *parent)
    : QObject(parent)
{
    autoRefreshTimer = new QTimer(this);
    autoRefreshTimer->setInterval(500);
    connect(autoRefreshTimer, &QTimer::timeout,
            this, &PeripheralManager::refreshStates);
    autoRefreshTimer->start();
}

PeripheralManager::~PeripheralManager()
{
    stopAll();
    qDeleteAll(devices);
    devices.clear();
}

void PeripheralManager::setWorkspaceRoot(const QString &path)
{
    workspaceRoot = QFileInfo(path).absoluteFilePath();
}

bool PeripheralManager::loadConfig(const QString &path)
{
    stopAll();
    qDeleteAll(devices);
    devices.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit managerMessage(QString("[Peripherals] failed to open config: %1").arg(path));
        emit devicesChanged();
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();

    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        emit managerMessage(QString("[Peripherals] invalid config JSON: %1").arg(err.errorString()));
        emit devicesChanged();
        return false;
    }

    const QJsonObject root = doc.object();

    QStringList validationErrors;
    QStringList validationWarnings;
    if (!validateConfigDocument(root, validationErrors, validationWarnings)) {
        emit managerMessage(QString("[Peripherals][Validation] failed: %1").arg(path));
        for (const QString &msg : validationErrors) {
            emit managerMessage(QString("[Peripherals][Validation][ERROR] %1").arg(msg));
        }
        for (const QString &msg : validationWarnings) {
            emit managerMessage(QString("[Peripherals][Validation][WARN] %1").arg(msg));
        }
        emit devicesChanged();
        return false;
    }

    for (const QString &msg : validationWarnings) {
        emit managerMessage(QString("[Peripherals][Validation][WARN] %1").arg(msg));
    }

    loadedConfigPath = QFileInfo(path).absoluteFilePath();
    configDir = QFileInfo(loadedConfigPath).absolutePath();

    const QJsonArray arr = root.value("devices").toArray();
    for (const QJsonValue &value : arr) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject obj = value.toObject();
        auto *runtime = new DeviceRuntime();
        runtime->rawConfig = obj;
        runtime->id = obj.value("id").toString();
        runtime->type = obj.value("type").toString();
        runtime->status = obj.value("status").toString("loaded");

        const QJsonObject bus = obj.value("bus").toObject();
        runtime->busKind = bus.value("kind").toString();
        runtime->busController = bus.value("controller").toString();
        runtime->busAddress = bus.value("address").toString(obj.value("reg").toString());

        const QJsonObject sim = obj.value("simulator").toObject();
        runtime->simExec = sim.value("exec").toString();
        const QString protocol = sim.value("protocol").toString().trimmed().toLower();
        const QJsonArray args = sim.value("args").toArray();
        for (const QJsonValue &arg : args) {
            runtime->simArgs.append(arg.toString());
        }

        if (runtime->id.isEmpty() || runtime->type.isEmpty() || runtime->simExec.isEmpty()) {
            runtime->status = "invalid";
            runtime->lastError = "missing required fields";
        } else if (!protocol.isEmpty() && !busKindMatchesProtocol(runtime->busKind, protocol)) {
            runtime->status = "invalid";
            runtime->lastError = QString("bus/protocol mismatch: bus=%1 protocol=%2")
                                     .arg(runtime->busKind, protocol);
        }

        devices.append(runtime);
    }

    emit managerMessage(QString("[Peripherals] loaded %1 device(s) from %2")
                        .arg(devices.size())
                        .arg(loadedConfigPath));
    emit devicesChanged();

    /* Auto-start all valid devices so they are ready before QEMU boots */
    startAll();

    return true;
}

bool PeripheralManager::validateConfigDocument(const QJsonObject &root,
                                               QStringList &errors,
                                               QStringList &warnings) const
{
    if (!root.contains("board") || !root.value("board").isObject()) {
        errors.append("missing object: board");
    } else {
        const QJsonObject board = root.value("board").toObject();
        if (board.value("name").toString().trimmed().isEmpty()) {
            errors.append("board.name is required and must be non-empty");
        }
    }

    if (!root.contains("devices") || !root.value("devices").isArray()) {
        errors.append("missing array: devices");
        return false;
    }

    const QJsonArray buses = root.value("buses").toArray();
    QSet<QString> busIds;
    for (int i = 0; i < buses.size(); ++i) {
        const QJsonObject bus = buses.at(i).toObject();
        const QString id = bus.value("id").toString().trimmed();
        if (id.isEmpty()) {
            warnings.append(QString("buses[%1] has empty id").arg(i));
            continue;
        }
        if (busIds.contains(id)) {
            errors.append(QString("duplicate buses id: %1").arg(id));
        }
        busIds.insert(id);
    }

    const QJsonArray devicesArr = root.value("devices").toArray();
    if (devicesArr.isEmpty()) {
        errors.append("devices array is empty");
    }

    QSet<QString> deviceIds;
    for (int i = 0; i < devicesArr.size(); ++i) {
        if (!devicesArr.at(i).isObject()) {
            errors.append(QString("devices[%1] is not an object").arg(i));
            continue;
        }

        const QJsonObject dev = devicesArr.at(i).toObject();
        const QString id = dev.value("id").toString().trimmed();
        const QString type = dev.value("type").toString().trimmed();

        if (id.isEmpty()) {
            errors.append(QString("devices[%1].id is required").arg(i));
        } else {
            if (deviceIds.contains(id)) {
                errors.append(QString("duplicate device id: %1").arg(id));
            }
            deviceIds.insert(id);
        }

        if (type.isEmpty()) {
            errors.append(QString("devices[%1].type is required").arg(i));
        }

        const QJsonObject bus = dev.value("bus").toObject();
        const QString busKind = bus.value("kind").toString().trimmed().toLower();
        const QString controller = bus.value("controller").toString().trimmed();
        if (busKind.isEmpty()) {
            errors.append(QString("devices[%1].bus.kind is required").arg(i));
        }
        if (controller.isEmpty()) {
            errors.append(QString("devices[%1].bus.controller is required").arg(i));
        }

        const QString busRef = dev.value("bus_ref").toString().trimmed();
        if (!busRef.isEmpty() && !busIds.contains(busRef)) {
            warnings.append(QString("devices[%1].bus_ref '%2' not found in buses[]")
                            .arg(i)
                            .arg(busRef));
        }

        const QJsonObject simulator = dev.value("simulator").toObject();
        const QString exec = simulator.value("exec").toString().trimmed();
        if (exec.isEmpty()) {
            errors.append(QString("devices[%1].simulator.exec is required").arg(i));
        }

        const QString protocol = simulator.value("protocol").toString().trimmed().toLower();
        if (!protocol.isEmpty() && !busKind.isEmpty() && !busKindMatchesProtocol(busKind, protocol)) {
            warnings.append(QString("devices[%1] protocol '%2' may not match bus.kind '%3'")
                            .arg(i)
                            .arg(protocol, busKind));
        }
    }

    return errors.isEmpty();
}

bool PeripheralManager::busKindMatchesProtocol(const QString &busKind, const QString &protocol) const
{
    const QString b = busKind.trimmed().toLower();
    const QString p = protocol.trimmed().toLower();
    if (p.isEmpty() || b.isEmpty()) {
        return true;
    }
    if (p.contains("i2c")) {
        return b == "i2c";
    }
    if (p.contains("spi")) {
        return b == "spi";
    }
    if (p.contains("uart")) {
        return b == "uart";
    }
    return true;
}

QString PeripheralManager::normalizeAddressText(const QString &value) const
{
    const QString text = value.trimmed().toLower();
    if (text.isEmpty()) {
        return text;
    }

    bool ok = false;
    int base = 10;
    QString payload = text;
    if (payload.startsWith("0x")) {
        base = 16;
        payload = payload.mid(2);
    }

    const int num = payload.toInt(&ok, base);
    if (!ok) {
        return text;
    }
    return QString("0x%1").arg(num, 0, 16);
}

void PeripheralManager::emitTrace(DeviceRuntime *device,
                                  const QString &dir,
                                  const QJsonObject &obj)
{
    if (!device) {
        return;
    }
    const QString compact = QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
    emit deviceTraceLine(device->id, QString("[%1] %2").arg(dir, compact));
}

QString PeripheralManager::findDefaultConfigPath() const
{
    const QString envCfg = qEnvironmentVariable("ESP32S3_PERIPHERALS_CONFIG");
    if (!envCfg.isEmpty() && QFileInfo::exists(envCfg)) {
        return QFileInfo(envCfg).absoluteFilePath();
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::cleanPath(appDir + "/../peripherals/peripherals.example.json"),
        QDir::cleanPath(appDir + "/../../peripherals/peripherals.example.json"),
        QDir::cleanPath(QDir::currentPath() + "/peripherals/peripherals.example.json"),
        QDir::cleanPath(QDir::currentPath() + "/../peripherals/peripherals.example.json"),
        QDir::cleanPath(QDir::currentPath() + "/../../peripherals/peripherals.example.json"),
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    if (!workspaceRoot.isEmpty()) {
        const QString fromWorkspace =
            QDir(workspaceRoot).absoluteFilePath("peripherals/peripherals.example.json");
        if (QFileInfo::exists(fromWorkspace)) {
            return QFileInfo(fromWorkspace).absoluteFilePath();
        }
    }

    return QString();
}

bool PeripheralManager::loadDefaultConfig()
{
    if (!loadedConfigPath.isEmpty() && QFileInfo::exists(loadedConfigPath)) {
        return true;
    }

    const QString path = findDefaultConfigPath();
    if (path.isEmpty()) {
        emit managerMessage("[Peripherals] no default config found (set ESP32S3_PERIPHERALS_CONFIG or load manually)");
        return false;
    }

    return loadConfig(path);
}

void PeripheralManager::startAll()
{
    for (DeviceRuntime *device : devices) {
        if (device->status == "disabled" || device->status == "invalid") {
            continue;
        }
        startDevice(device);
    }
    emit devicesChanged();
}

void PeripheralManager::ensureAllRunning()
{
    for (DeviceRuntime *device : devices) {
        if (!device || device->status == "disabled" || device->status == "invalid") {
            continue;
        }
        if (device->process && device->process->state() == QProcess::Running) {
            continue;
        }
        startDevice(device);
    }
    emit devicesChanged();
}

void PeripheralManager::stopAll()
{
    for (DeviceRuntime *device : devices) {
        stopDevice(device);
    }
    emit devicesChanged();
}

void PeripheralManager::refreshStates()
{
    for (DeviceRuntime *device : devices) {
        if (device->process && device->process->state() == QProcess::Running) {
            sendRpc(device, "get_state", QJsonObject(), true);
        }
    }
}

void PeripheralManager::sendDeviceRpc(const QString &deviceId,
                                      const QString &method,
                                      const QJsonObject &params)
{
    for (DeviceRuntime *device : devices) {
        if (device->id == deviceId) {
            if (device->process && device->process->state() == QProcess::Running) {
                sendRpc(device, method, params, true);
            } else {
                emit managerMessage(QString("[Peripherals] %1 not running, cannot call %2")
                                    .arg(deviceId, method));
            }
            return;
        }
    }
    emit managerMessage(QString("[Peripherals] device not found: %1").arg(deviceId));
}

void PeripheralManager::setDeviceParameter(const QString &deviceId,
                                           const QString &paramName,
                                           const QVariant &value)
{
    QJsonObject params;
    params["name"] = paramName;
    if (value.typeId() == QMetaType::Bool) {
        params["value"] = value.toBool();
    } else if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float) {
        params["value"] = value.toDouble();
    } else if (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong) {
        params["value"] = value.toInt();
    } else {
        params["value"] = value.toString();
    }
    sendDeviceRpc(deviceId, "set_parameter", params);
}

QJsonArray PeripheralManager::devicesSnapshot() const
{
    QJsonArray out;
    for (const DeviceRuntime *device : devices) {
        QJsonObject o;
        o["id"] = device->id;
        o["type"] = device->type;
        o["status"] = device->status;
        o["last_error"] = device->lastError;
        o["bus_kind"] = device->busKind;
        o["bus_controller"] = device->busController;
        o["bus_address"] = device->busAddress;
        o["capabilities"] = device->capabilities;
        o["state"] = device->state;
        o["raw"] = device->rawConfig;
        out.append(o);
    }
    return out;
}

QString PeripheralManager::configPath() const
{
    return loadedConfigPath;
}

QMap<int, QSet<QString>> PeripheralManager::getI2cBusAddresses() const
{
    QMap<int, QSet<QString>> result;

    for (const DeviceRuntime *device : devices) {
        if (device->busKind.compare("i2c", Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (device->status == "invalid" || device->status == "disabled") {
            continue;
        }

        /* Determine bus index from controller name like "i2c0" / "i2c1" */
        int busIdx = 0;
        const QString ctrl = device->busController.trimmed().toLower();
        if (ctrl.startsWith("i2c") && ctrl.length() > 3) {
            bool ok = false;
            int idx = ctrl.mid(3).toInt(&ok);
            if (ok) {
                busIdx = idx;
            }
        }

        /* Strip "0x" prefix so we get bare hex like "3c" */
        QString addr = normalizeAddressText(device->busAddress);
        if (addr.startsWith("0x")) {
            addr = addr.mid(2);
        }
        if (!addr.isEmpty()) {
            result[busIdx].insert(addr);
        }
    }

    return result;
}

void PeripheralManager::dispatchI2cTransfer(const QJsonObject &request)
{
    const QString controller = request.value("controller").toString().trimmed();
    const QString address = normalizeAddressText(request.value("address").toString());

    for (DeviceRuntime *device : devices) {
        if (!device || !device->process || device->process->state() != QProcess::Running) {
            continue;
        }
        if (device->busKind != "i2c") {
            continue;
        }
        if (!controller.isEmpty() && device->busController != controller) {
            continue;
        }
        if (!address.isEmpty() && normalizeAddressText(device->busAddress) != address) {
            continue;
        }

        sendRpc(device, "i2c_transfer", request, true,
            QJsonObject{{"bridge_bus", "i2c"}, {"request", request}});
        return;
    }

    emit managerMessage(QString("[Bridge][I2C] no matching running device for controller=%1 address=%2")
                        .arg(controller, address));
}

void PeripheralManager::dispatchSpiTransfer(const QJsonObject &request)
{
    const QString controller = request.value("controller").toString().trimmed();
    const QString csText = request.value("chip_select").toVariant().toString().trimmed();

    for (DeviceRuntime *device : devices) {
        if (!device || !device->process || device->process->state() != QProcess::Running) {
            continue;
        }
        if (device->busKind != "spi") {
            continue;
        }
        if (!controller.isEmpty() && device->busController != controller) {
            continue;
        }
        if (!csText.isEmpty() && device->busAddress != csText) {
            continue;
        }

        sendRpc(device, "spi_transfer", request, true,
            QJsonObject{{"bridge_bus", "spi"}, {"request", request}});
        return;
    }

    emit managerMessage(QString("[Bridge][SPI] no matching running device for controller=%1 cs=%2")
                        .arg(controller, csText));
}

void PeripheralManager::dispatchUartTx(const QJsonObject &request)
{
    const QString controller = request.value("controller").toString().trimmed();
    const QString unitText = request.value("unit").toVariant().toString().trimmed();

    for (DeviceRuntime *device : devices) {
        if (!device || !device->process || device->process->state() != QProcess::Running) {
            continue;
        }
        if (device->busKind != "uart") {
            continue;
        }
        if (!controller.isEmpty() && device->busController != controller) {
            continue;
        }
        if (!unitText.isEmpty() && device->busAddress != unitText) {
            continue;
        }

        if (request.contains("baud") || request.contains("data_bits")
            || request.contains("parity") || request.contains("stop_bits")) {
            QJsonObject lineCfg;
            if (request.contains("baud")) {
                lineCfg["baud"] = request.value("baud");
            }
            if (request.contains("data_bits")) {
                lineCfg["data_bits"] = request.value("data_bits");
            }
            if (request.contains("parity")) {
                lineCfg["parity"] = request.value("parity");
            }
            if (request.contains("stop_bits")) {
                lineCfg["stop_bits"] = request.value("stop_bits");
            }
                sendRpc(device, "uart_set_line", lineCfg, true,
                    QJsonObject{{"bridge_bus", "uart"}, {"request", lineCfg}});
        }

        QJsonObject tx;
        tx["data"] = request.value("data").toArray();
        sendRpc(device, "uart_tx", tx, true,
            QJsonObject{{"bridge_bus", "uart"}, {"request", request}});
        return;
    }

    emit managerMessage(QString("[Bridge][UART] no matching running device for controller=%1 unit=%2")
                        .arg(controller, unitText));
}

QString PeripheralManager::resolvePathForConfig(const QString &raw) const
{
    if (raw.isEmpty()) {
        return raw;
    }

    QFileInfo fi(raw);
    if (fi.isAbsolute()) {
        return fi.absoluteFilePath();
    }

    if (raw.startsWith("./") || raw.startsWith("../")) {
        const QString fromConfig = QDir(configDir).absoluteFilePath(raw);
        if (QFileInfo::exists(fromConfig)) {
            return fromConfig;
        }

        if (!workspaceRoot.isEmpty()) {
            const QString fromRoot = QDir(workspaceRoot).absoluteFilePath(raw);
            if (QFileInfo::exists(fromRoot)) {
                return fromRoot;
            }
        }

        return fromConfig;
    }

    return raw;
}

QString PeripheralManager::resolvePythonExecutable(QStringList *bootstrapArgs) const
{
    if (bootstrapArgs) {
        bootstrapArgs->clear();
    }

    const QString envPython = qEnvironmentVariable("ESP32S3_PYTHON");
    if (!envPython.isEmpty()) {
        QFileInfo fi(envPython);
        if ((fi.isAbsolute() && fi.exists()) || !fi.isAbsolute()) {
            return fi.isAbsolute() ? fi.absoluteFilePath() : envPython;
        }
    }

    const QStringList venvCandidates = {
        QDir::cleanPath(QDir::currentPath() + "/.venv/bin/python3"),
        QDir::cleanPath(QDir::currentPath() + "/.venv/bin/python"),
        QDir::cleanPath(QDir::currentPath() + "/.venv/Scripts/python.exe"),
        workspaceRoot.isEmpty() ? QString() : QDir::cleanPath(workspaceRoot + "/.venv/bin/python3"),
        workspaceRoot.isEmpty() ? QString() : QDir::cleanPath(workspaceRoot + "/.venv/bin/python"),
        workspaceRoot.isEmpty() ? QString() : QDir::cleanPath(workspaceRoot + "/.venv/Scripts/python.exe"),
    };

    for (const QString &candidate : venvCandidates) {
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

#if defined(Q_OS_WIN)
    const QString pyLauncher = QStandardPaths::findExecutable("py");
    if (!pyLauncher.isEmpty()) {
        if (bootstrapArgs) {
            bootstrapArgs->append("-3");
        }
        return pyLauncher;
    }

    const QString pyExe = QStandardPaths::findExecutable("python.exe");
    if (!pyExe.isEmpty()) {
        return pyExe;
    }

    const QString pyCmd = QStandardPaths::findExecutable("python");
    if (!pyCmd.isEmpty()) {
        return pyCmd;
    }
#else
    const QString py3 = QStandardPaths::findExecutable("python3");
    if (!py3.isEmpty()) {
        return py3;
    }

    const QString py = QStandardPaths::findExecutable("python");
    if (!py.isEmpty()) {
        return py;
    }
#endif

    return QString();
}

bool PeripheralManager::isPythonScriptPath(const QString &path) const
{
    const QString lower = path.trimmed().toLower();
    return lower.endsWith(".py") || lower.endsWith(".pyw");
}

void PeripheralManager::startDevice(DeviceRuntime *device)
{
    if (!device || device->simExec.isEmpty()) {
        return;
    }

    stopDevice(device);

    auto *process = new QProcess(this);
    device->process = process;
    device->stdoutBuffer.clear();
    device->pendingMethods.clear();
    device->rpcSeq = 1;

    QString execPath = resolvePathForConfig(device->simExec);
    QStringList args = device->simArgs;
    for (QString &arg : args) {
        arg = resolvePathForConfig(arg);
    }

    QString finalExec = execPath;
    QStringList finalArgs = args;

    const QString execBase = QFileInfo(execPath).fileName().toLower();
    const bool execIsPythonCmd =
        execBase == "python" || execBase == "python3" || execBase == "python.exe" ||
        execBase == "py" || execBase == "py.exe";

    if (isPythonScriptPath(execPath)) {
        QStringList bootstrapArgs;
        const QString pythonExec = resolvePythonExecutable(&bootstrapArgs);
        if (pythonExec.isEmpty()) {
            device->status = "error";
            device->lastError = "python interpreter not found";
            emit managerMessage(QString("[Peripherals] %1 cannot start: python interpreter not found")
                                .arg(device->id));
            emit devicesChanged();
            process->deleteLater();
            device->process = nullptr;
            return;
        }

        finalExec = pythonExec;
        finalArgs = bootstrapArgs + QStringList{execPath} + args;
    } else if (execIsPythonCmd) {
        QStringList bootstrapArgs;
        const QString pythonExec = resolvePythonExecutable(&bootstrapArgs);
        if (!pythonExec.isEmpty()) {
            finalExec = pythonExec;
            finalArgs = bootstrapArgs + args;
        }
    } else if (!QFileInfo(execPath).isAbsolute()) {
        const QString foundExec = QStandardPaths::findExecutable(execPath);
        if (!foundExec.isEmpty()) {
            finalExec = foundExec;
        }
    }

    QString workDir = configDir;
    if (!workspaceRoot.isEmpty()) {
        workDir = workspaceRoot;
    }
    process->setWorkingDirectory(workDir);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, device]() {
        parseStdout(device, device->process->readAllStandardOutput());
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, device]() {
        parseStderr(device, device->process->readAllStandardError());
    });

    connect(process, &QProcess::started, this, [this, device]() {
        device->status = "running";
        device->lastError.clear();
        emit managerMessage(QString("[Peripherals] %1 started").arg(device->id));
        sendRpc(device, "get_capabilities", QJsonObject(), true);
        sendRpc(device, "get_state", QJsonObject(), true);
        emit devicesChanged();
    });

    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, device](int code, QProcess::ExitStatus status) {
        device->status = "stopped";
        if (status == QProcess::CrashExit) {
            device->lastError = QString("process crashed (code=%1)").arg(code);
        }
        emit managerMessage(QString("[Peripherals] %1 exited code=%2 status=%3")
                            .arg(device->id)
                            .arg(code)
                            .arg(status == QProcess::NormalExit ? "normal" : "crash"));
        emit devicesChanged();
    });

    connect(process, &QProcess::errorOccurred, this, [this, device](QProcess::ProcessError e) {
        device->status = "error";
        device->lastError = QString("process error=%1").arg(static_cast<int>(e));
        emit managerMessage(QString("[Peripherals] %1 process error=%2")
                            .arg(device->id)
                            .arg(static_cast<int>(e)));
        emit devicesChanged();
    });

    process->start(finalExec, finalArgs);
}

void PeripheralManager::stopDevice(DeviceRuntime *device)
{
    if (!device || !device->process) {
        return;
    }

    if (device->process->state() != QProcess::NotRunning) {
        device->process->terminate();
        if (!device->process->waitForFinished(1000)) {
            device->process->kill();
            device->process->waitForFinished(1000);
        }
    }

    device->process->deleteLater();
    device->process = nullptr;
    if (device->status != "invalid" && device->status != "disabled") {
        device->status = "stopped";
    }
}

void PeripheralManager::sendRpc(DeviceRuntime *device,
                                const QString &method,
                                const QJsonObject &params,
                                bool withId,
                                const QJsonObject &meta)
{
    if (!device || !device->process || device->process->state() != QProcess::Running) {
        return;
    }

    QJsonObject obj;
    obj["jsonrpc"] = "2.0";
    obj["method"] = method;
    if (!params.isEmpty()) {
        obj["params"] = params;
    }

    if (withId) {
        const int id = device->rpcSeq++;
        obj["id"] = id;
        QJsonObject pending = meta;
        pending["method"] = method;
        device->pendingMethods[id] = pending;
    }

    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    emitTrace(device, "TX", obj);
    device->process->write(line);
}

void PeripheralManager::parseStdout(DeviceRuntime *device, const QByteArray &bytes)
{
    if (!device || bytes.isEmpty()) {
        return;
    }

    device->stdoutBuffer += bytes;

    int newline = device->stdoutBuffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray lineBytes = device->stdoutBuffer.left(newline);
        device->stdoutBuffer.remove(0, newline + 1);

        const QString line = QString::fromUtf8(lineBytes).trimmed();
        if (!line.isEmpty()) {
            emit deviceLogLine(device->id, line);
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(lineBytes, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            emitTrace(device, "RX", doc.object());
            handleJsonMessage(device, doc.object());
        }

        newline = device->stdoutBuffer.indexOf('\n');
    }
}

void PeripheralManager::parseStderr(DeviceRuntime *device, const QByteArray &bytes)
{
    if (!device || bytes.isEmpty()) {
        return;
    }

    const QString text = QString::fromUtf8(bytes).trimmed();
    if (!text.isEmpty()) {
        emit deviceLogLine(device->id, QString("[stderr] %1").arg(text));
    }
}

void PeripheralManager::handleJsonMessage(DeviceRuntime *device, const QJsonObject &obj)
{
    if (!device) {
        return;
    }

    if (obj.contains("id")) {
        const int id = obj.value("id").toInt(-1);
        const QJsonObject pending = device->pendingMethods.take(id);
        const QString method = pending.value("method").toString();
        const QString bridgeBus = pending.value("bridge_bus").toString();

        if (obj.contains("error")) {
            device->lastError = QJsonDocument(obj.value("error").toObject()).toJson(QJsonDocument::Compact);
            if (!bridgeBus.isEmpty()) {
                QJsonObject payload;
                payload["ok"] = false;
                payload["bus"] = bridgeBus;
                payload["method"] = method;
                payload["device_id"] = device->id;
                payload["request"] = pending.value("request").toObject();
                payload["error"] = obj.value("error").toObject();
                emit bridgeResponseReady(bridgeBus, payload);
            }
            emit devicesChanged();
            return;
        }

        if (!bridgeBus.isEmpty()) {
            QJsonObject payload;
            payload["ok"] = true;
            payload["bus"] = bridgeBus;
            payload["method"] = method;
            payload["device_id"] = device->id;
            payload["request"] = pending.value("request").toObject();
            payload["result"] = obj.value("result").toObject();
            emit bridgeResponseReady(bridgeBus, payload);
        }

        if (method == "get_capabilities") {
            device->capabilities = obj.value("result").toObject();
            emit devicesChanged();
            return;
        }

        if (method == "get_state") {
            /* Preserve live notification keys that arrive
               asynchronously between get_state polls so the GUI
               never reverts to stale buffer data. */
            static const char *liveKeys[] = {
                "frame_update", "telemetry", "uart_rx", "playback"
            };
            QJsonObject saved;
            for (const char *key : liveKeys) {
                if (device->state.contains(QLatin1String(key))) {
                    saved[QLatin1String(key)] = device->state[QLatin1String(key)];
                }
            }
            device->state = obj.value("result").toObject();
            for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
                if (!device->state.contains(it.key())) {
                    device->state[it.key()] = it.value();
                }
            }
            emit devicesChanged();
            return;
        }

        return;
    }

    const QString notifyMethod = obj.value("method").toString();
    if (notifyMethod == "telemetry") {
        device->state["telemetry"] = obj.value("params").toObject();
        emit devicesChanged();
    } else if (notifyMethod == "frame_update") {
        device->state["frame_update"] = obj.value("params").toObject();
        emit devicesChanged();
    } else if (notifyMethod == "uart_rx") {
        device->state["uart_rx"] = obj.value("params").toObject();
        emit devicesChanged();
    } else if (notifyMethod == "playback_finished") {
        device->state["playback"] = obj.value("params").toObject();
        emit devicesChanged();
    }
}
