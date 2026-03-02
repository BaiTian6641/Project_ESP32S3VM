#include "PeripheralsWidget.h"

#include "DevicePanelBase.h"
#include "DevicePanelFactory.h"
#include "PeripheralManager.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

PeripheralsWidget::PeripheralsWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);

    // === Config controls bar ===
    auto *controlsLayout = new QHBoxLayout();

    controlsLayout->addWidget(new QLabel("Config:", this));

    configPathEdit = new QLineEdit(this);
    configPathEdit->setPlaceholderText("peripherals/peripherals.example.json");
    controlsLayout->addWidget(configPathEdit, 1);

    browseButton = new QPushButton("Browse", this);
    loadButton = new QPushButton("Load", this);
    startButton = new QPushButton("Start all", this);
    stopButton = new QPushButton("Stop all", this);
    refreshButton = new QPushButton("Refresh", this);

    controlsLayout->addWidget(browseButton);
    controlsLayout->addWidget(loadButton);
    controlsLayout->addWidget(startButton);
    controlsLayout->addWidget(stopButton);
    controlsLayout->addWidget(refreshButton);

    rootLayout->addLayout(controlsLayout);

    // Device count status
    auto *statusLayout = new QHBoxLayout();
    deviceCountLabel = new QLabel("No devices loaded", this);
    deviceCountLabel->setStyleSheet("color: #888; font-size: 11px;");
    statusLayout->addWidget(deviceCountLabel);
    statusLayout->addStretch();
    rootLayout->addLayout(statusLayout);

    // === Main area: splitter with device tabs on top, global log on bottom ===
    auto *splitter = new QSplitter(Qt::Vertical, this);

    // Device panel tabs
    deviceTabWidget = new QTabWidget(this);
    deviceTabWidget->setTabPosition(QTabWidget::North);
    deviceTabWidget->setDocumentMode(true);
    splitter->addWidget(deviceTabWidget);

    // Global log
    logText = new QTextEdit(this);
    logText->setReadOnly(true);
    logText->setMaximumHeight(150);
    logText->setPlaceholderText("Manager / device log output...");
    splitter->addWidget(logText);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(splitter, 1);

    // Connections
    connect(browseButton, &QPushButton::clicked, this, &PeripheralsWidget::onBrowseConfig);
    connect(loadButton, &QPushButton::clicked, this, &PeripheralsWidget::onLoadConfig);
    connect(startButton, &QPushButton::clicked, this, &PeripheralsWidget::onStartAll);
    connect(stopButton, &QPushButton::clicked, this, &PeripheralsWidget::onStopAll);
    connect(refreshButton, &QPushButton::clicked, this, &PeripheralsWidget::onRefresh);
}

void PeripheralsWidget::setManager(PeripheralManager *manager)
{
    if (peripheralManager == manager) return;

    if (peripheralManager) {
        disconnect(peripheralManager, nullptr, this, nullptr);
    }

    peripheralManager = manager;

    if (peripheralManager) {
        connect(peripheralManager, &PeripheralManager::devicesChanged,
                this, &PeripheralsWidget::onDevicesChanged);
        connect(peripheralManager, &PeripheralManager::deviceLogLine,
                this, &PeripheralsWidget::onDeviceLogLine);
        connect(peripheralManager, &PeripheralManager::deviceTraceLine,
                this, &PeripheralsWidget::onDeviceTraceLine);
        connect(peripheralManager, &PeripheralManager::managerMessage,
                this, &PeripheralsWidget::onManagerMessage);

        onDevicesChanged();

        if (configPathEdit->text().isEmpty()) {
            if (!peripheralManager->configPath().isEmpty()) {
                configPathEdit->setText(peripheralManager->configPath());
            } else {
                const QString defaultPath = peripheralManager->findDefaultConfigPath();
                if (!defaultPath.isEmpty()) {
                    configPathEdit->setText(defaultPath);
                }
            }
        }
    }
}

void PeripheralsWidget::onBrowseConfig()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Select peripheral config",
        configPathEdit->text(),
        "JSON files (*.json)");
    if (!path.isEmpty()) {
        configPathEdit->setText(path);
    }
}

void PeripheralsWidget::onLoadConfig()
{
    if (!peripheralManager) return;
    const QString path = configPathEdit->text().trimmed();
    if (!path.isEmpty()) {
        peripheralManager->loadConfig(path);
    }
}

void PeripheralsWidget::onStartAll()
{
    if (peripheralManager) peripheralManager->startAll();
}

void PeripheralsWidget::onStopAll()
{
    if (peripheralManager) peripheralManager->stopAll();
}

void PeripheralsWidget::onRefresh()
{
    if (peripheralManager) peripheralManager->refreshStates();
}

void PeripheralsWidget::onDevicesChanged()
{
    if (!peripheralManager) {
        lastSnapshot = QJsonArray();
    } else {
        lastSnapshot = peripheralManager->devicesSnapshot();
    }

    // Check if device set has changed (need to rebuild panels)
    QSet<QString> currentIds;
    for (const QJsonValue &v : lastSnapshot) {
        currentIds.insert(v.toObject().value("id").toString());
    }

    QSet<QString> existingIds;
    for (auto it = devicePanels.constBegin(); it != devicePanels.constEnd(); ++it) {
        existingIds.insert(it.key());
    }

    bool panelKindChanged = false;
    if (currentIds == existingIds) {
        for (const QJsonValue &v : lastSnapshot) {
            const QJsonObject obj = v.toObject();
            const QString id = obj.value("id").toString();
            const QString kind = panelKindFromSnapshot(obj);
            if (devicePanelKinds.value(id) != kind) {
                panelKindChanged = true;
                break;
            }
        }
    }

    if (currentIds != existingIds || panelKindChanged) {
        rebuildDevicePanels();
    } else {
        updateDevicePanels();
    }
}

void PeripheralsWidget::rebuildDevicePanels()
{
    // Remove old panels
    while (deviceTabWidget->count() > 0) {
        QWidget *w = deviceTabWidget->widget(0);
        deviceTabWidget->removeTab(0);
        w->deleteLater();
    }
    devicePanels.clear();
    devicePanelKinds.clear();

    if (lastSnapshot.isEmpty()) {
        deviceCountLabel->setText("No devices loaded");
        return;
    }

    deviceCountLabel->setText(
        QString("%1 device(s) loaded").arg(lastSnapshot.size()));

    // Create a panel for each device
    for (const QJsonValue &v : lastSnapshot) {
        const QJsonObject obj = v.toObject();
        const QString id = obj.value("id").toString();
        const QString type = obj.value("type").toString();
        const QJsonObject rawConfig = obj.value("raw").toObject();
        const QJsonObject caps = obj.value("capabilities").toObject();

        DevicePanelBase *panel = DevicePanelFactory::createPanel(
            id, type, rawConfig, caps, this);

        // Wire up parameter changes
        connect(panel, &DevicePanelBase::parameterChangeRequested,
                this, &PeripheralsWidget::onPanelParameterChange);

        // Wrap panel in scroll area for large content
        auto *scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setWidget(panel);

        // Add tab with icon indicator
        QString tabLabel = QString("%1 %2").arg(iconForDeviceType(type), id);
        deviceTabWidget->addTab(scrollArea, tabLabel);

        devicePanels[id] = panel;
        devicePanelKinds[id] = panelKindFromSnapshot(obj);

        // Push initial state
        panel->updateStatus(obj.value("status").toString(),
                            obj.value("last_error").toString());

        if (!caps.isEmpty()) {
            panel->updateCapabilities(caps);
        }

        const QJsonObject state = obj.value("state").toObject();
        if (!state.isEmpty()) {
            panel->updateState(state);
        }
    }
}

QString PeripheralsWidget::panelKindFromSnapshot(const QJsonObject &obj) const
{
    return obj.value("capabilities")
        .toObject()
        .value("panel")
        .toObject()
        .value("kind")
        .toString()
        .trimmed()
        .toLower();
}

void PeripheralsWidget::updateDevicePanels()
{
    for (const QJsonValue &v : lastSnapshot) {
        const QJsonObject obj = v.toObject();
        const QString id = obj.value("id").toString();

        DevicePanelBase *panel = devicePanels.value(id, nullptr);
        if (!panel) continue;

        panel->updateStatus(obj.value("status").toString(),
                            obj.value("last_error").toString());

        const QJsonObject caps = obj.value("capabilities").toObject();
        if (!caps.isEmpty()) {
            panel->updateCapabilities(caps);
        }

        const QJsonObject state = obj.value("state").toObject();
        if (!state.isEmpty()) {
            panel->updateState(state);
        }
    }
}

void PeripheralsWidget::onDeviceLogLine(const QString &deviceId, const QString &line)
{
    // Route to device-specific panel log
    DevicePanelBase *panel = devicePanels.value(deviceId, nullptr);
    if (panel) {
        panel->appendLog(line);
    }
    logText->append(QString("[%1] %2").arg(deviceId, line));
}

void PeripheralsWidget::onDeviceTraceLine(const QString &deviceId, const QString &line)
{
    DevicePanelBase *panel = devicePanels.value(deviceId, nullptr);
    if (panel) {
        panel->appendLog(QString("[trace] %1").arg(line));
    }
    logText->append(QString("[%1][trace] %2").arg(deviceId, line));
}

void PeripheralsWidget::onManagerMessage(const QString &line)
{
    logText->append(line);
}

void PeripheralsWidget::onPanelParameterChange(const QString &deviceId,
                                                const QString &paramName,
                                                const QVariant &value)
{
    if (!peripheralManager) return;

    // Special RPC calls prefixed with "__rpc:"
    if (paramName.startsWith("__rpc:")) {
        const QString method = paramName.mid(6);  // strip "__rpc:"
        QJsonObject params;
        if (value.canConvert<QJsonObject>()) {
            params = value.toJsonObject();
        }
        peripheralManager->sendDeviceRpc(deviceId, method, params);
        return;
    }

    // Normal parameter change
    peripheralManager->setDeviceParameter(deviceId, paramName, value);
}

QString PeripheralsWidget::iconForDeviceType(const QString &type) const
{
    const QString lower = type.toLower();
    if (lower.contains("ssd1306") || lower.contains("display") ||
        lower.contains("oled") || lower.contains("lcd")) {
        return "\U0001F4FA";  // TV/display
    }
    if (lower.contains("sht") || lower.contains("bme") || lower.contains("sensor") ||
        lower.contains("temp") || lower.contains("humidity")) {
        return "\U0001F321";  // thermometer
    }
    if (lower.contains("flash") || lower.contains("eeprom") || lower.contains("spi")) {
        return "\U0001F4BE";  // floppy disk
    }
    if (lower.contains("uart") || lower.contains("serial") || lower.contains("loopback")) {
        return "\U0001F50C";  // plug
    }
    return "\U0001F527";  // wrench (generic)
}
