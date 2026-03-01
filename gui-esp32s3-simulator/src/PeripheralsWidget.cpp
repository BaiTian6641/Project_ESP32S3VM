#include "PeripheralsWidget.h"

#include "PeripheralManager.h"

#include <QFileDialog>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

PeripheralsWidget::PeripheralsWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
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

    devicesTable = new QTableWidget(this);
    devicesTable->setColumnCount(6);
    devicesTable->setHorizontalHeaderLabels({"ID", "Type", "Bus", "Address", "Status", "Error"});
    devicesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    devicesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    devicesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    devicesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rootLayout->addWidget(devicesTable, 1);

    auto *lowerLayout = new QHBoxLayout();

    detailsText = new QTextEdit(this);
    detailsText->setReadOnly(true);
    detailsText->setPlaceholderText("Select a device to inspect raw config, capabilities, and state.");

    logText = new QTextEdit(this);
    logText->setReadOnly(true);
    logText->setPlaceholderText("Manager/device log output.");

    lowerLayout->addWidget(detailsText, 1);
    lowerLayout->addWidget(logText, 1);
    rootLayout->addLayout(lowerLayout, 1);

    connect(browseButton, &QPushButton::clicked, this, &PeripheralsWidget::onBrowseConfig);
    connect(loadButton, &QPushButton::clicked, this, &PeripheralsWidget::onLoadConfig);
    connect(startButton, &QPushButton::clicked, this, &PeripheralsWidget::onStartAll);
    connect(stopButton, &QPushButton::clicked, this, &PeripheralsWidget::onStopAll);
    connect(refreshButton, &QPushButton::clicked, this, &PeripheralsWidget::onRefresh);
    connect(devicesTable, &QTableWidget::itemSelectionChanged, this, &PeripheralsWidget::onTableSelectionChanged);

}

void PeripheralsWidget::setManager(PeripheralManager *manager)
{
    if (peripheralManager == manager) {
        return;
    }

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
        this,
        "Select peripheral config",
        configPathEdit->text(),
        "JSON files (*.json)");

    if (!path.isEmpty()) {
        configPathEdit->setText(path);
    }
}

void PeripheralsWidget::onLoadConfig()
{
    if (!peripheralManager) {
        return;
    }

    const QString path = configPathEdit->text().trimmed();
    if (path.isEmpty()) {
        return;
    }

    peripheralManager->loadConfig(path);
}

void PeripheralsWidget::onStartAll()
{
    if (peripheralManager) {
        peripheralManager->startAll();
    }
}

void PeripheralsWidget::onStopAll()
{
    if (peripheralManager) {
        peripheralManager->stopAll();
    }
}

void PeripheralsWidget::onRefresh()
{
    if (peripheralManager) {
        peripheralManager->refreshStates();
    }
}

void PeripheralsWidget::onDevicesChanged()
{
    if (!peripheralManager) {
        lastSnapshot = QJsonArray();
    } else {
        lastSnapshot = peripheralManager->devicesSnapshot();
    }

    rebuildTable();
    showSelectedDetails();
}

void PeripheralsWidget::onDeviceLogLine(const QString &deviceId, const QString &line)
{
    logText->append(QString("[%1] %2").arg(deviceId, line));
}

void PeripheralsWidget::onDeviceTraceLine(const QString &deviceId, const QString &line)
{
    logText->append(QString("[%1][trace] %2").arg(deviceId, line));
}

void PeripheralsWidget::onManagerMessage(const QString &line)
{
    logText->append(line);
}

void PeripheralsWidget::onTableSelectionChanged()
{
    showSelectedDetails();
}

void PeripheralsWidget::rebuildTable()
{
    const int previousRow = devicesTable->currentRow();

    devicesTable->setRowCount(lastSnapshot.size());
    for (int i = 0; i < lastSnapshot.size(); ++i) {
        const QJsonObject obj = lastSnapshot.at(i).toObject();

        auto *idItem = new QTableWidgetItem(obj.value("id").toString());
        auto *typeItem = new QTableWidgetItem(obj.value("type").toString());
        auto *busItem = new QTableWidgetItem(obj.value("bus_kind").toString());
        auto *addressItem = new QTableWidgetItem(obj.value("bus_address").toString());
        auto *statusItem = new QTableWidgetItem(obj.value("status").toString());
        auto *errorItem = new QTableWidgetItem(obj.value("last_error").toString());

        devicesTable->setItem(i, 0, idItem);
        devicesTable->setItem(i, 1, typeItem);
        devicesTable->setItem(i, 2, busItem);
        devicesTable->setItem(i, 3, addressItem);
        devicesTable->setItem(i, 4, statusItem);
        devicesTable->setItem(i, 5, errorItem);
    }

    if (lastSnapshot.isEmpty()) {
        return;
    }

    int restoreRow = previousRow;
    if (restoreRow < 0 || restoreRow >= lastSnapshot.size()) {
        restoreRow = 0;
    }
    devicesTable->selectRow(restoreRow);
}

void PeripheralsWidget::showSelectedDetails()
{
    const int row = devicesTable->currentRow();
    if (row < 0 || row >= lastSnapshot.size()) {
        detailsText->clear();
        return;
    }

    const QJsonObject obj = lastSnapshot.at(row).toObject();

    QJsonObject detail;
    detail["id"] = obj.value("id").toString();
    detail["type"] = obj.value("type").toString();
    detail["status"] = obj.value("status").toString();
    detail["bus"] = QJsonObject{
        {"kind", obj.value("bus_kind").toString()},
        {"controller", obj.value("bus_controller").toString()},
        {"address", obj.value("bus_address").toString()}
    };
    detail["capabilities"] = obj.value("capabilities").toObject();
    detail["state"] = obj.value("state").toObject();
    detail["raw"] = obj.value("raw").toObject();

    detailsText->setPlainText(QString::fromUtf8(
        QJsonDocument(detail).toJson(QJsonDocument::Indented)));
}
