#include "MainWindow.h"

#include <QTabWidget>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "SerialConsoleWidget.h"
#include "CpuStatusWidget.h"
#include "ControlPanelWidget.h"
#include "DebugWidget.h"
#include "QemuController.h"
#include "PeripheralsWidget.h"
#include "PeripheralManager.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      tabWidget(new QTabWidget(this)),
      serialWidget(new SerialConsoleWidget(this)),
      cpuWidget(new CpuStatusWidget(this)),
      controlWidget(new ControlPanelWidget(this)),
      debugWidget(new DebugWidget(this)),
      peripheralsWidget(new PeripheralsWidget(this)),
      controller(new QemuController(this)),
      peripheralManager(new PeripheralManager(this))
{
    setWindowTitle("ESP32-S3 GUI Simulator (MVP)");
    resize(1200, 800);

        const QString appDir = QCoreApplication::applicationDirPath();
        QString workspaceRoot = QFileInfo(appDir + "/../").absoluteFilePath();

        const QStringList candidateRoots = {
                QDir::cleanPath(appDir),
                QDir::cleanPath(appDir + "/../"),
                QDir::cleanPath(appDir + "/../../"),
                QDir::cleanPath(appDir + "/../../../"),
                QDir::cleanPath(appDir + "/../../../../"),
                QDir::cleanPath(QDir::currentPath())
        };

        for (const QString &candidate : candidateRoots) {
                const bool hasConfig = QFileInfo::exists(QDir(candidate).absoluteFilePath("peripherals/peripherals.example.json"));
                const bool hasSims = QFileInfo::exists(QDir(candidate).absoluteFilePath("device-sims"));
                if (hasConfig && hasSims) {
                        workspaceRoot = candidate;
                        break;
                }
        }

        peripheralManager->setWorkspaceRoot(workspaceRoot);

    tabWidget->addTab(serialWidget, "Serial");
    tabWidget->addTab(cpuWidget, "Processor Status");
    tabWidget->addTab(controlWidget, "Control");
    tabWidget->addTab(debugWidget, "Debug");
    tabWidget->addTab(peripheralsWidget, "Peripherals");
    setCentralWidget(tabWidget);

    serialWidget->setController(controller);
    cpuWidget->setController(controller);
    controlWidget->setController(controller);
    debugWidget->setController(controller);

    /* Load & auto-start peripheral config before QEMU launches */
    peripheralManager->loadDefaultConfig();
    peripheralsWidget->setManager(peripheralManager);

    /* Pre-load I2C bridge addresses from peripheral config so that
     * when QMP becomes ready the addresses are immediately pushed. */
    syncI2cBridgeAddresses();

    /* ---- Route debug/status messages to peripherals log, not serial ---- */
    connect(controller, &QemuController::debugMessageReceived,
            peripheralManager, &PeripheralManager::managerMessage);

    /* ---- QEMU lifecycle → peripheral sims ---- */
    connect(controller, &QemuController::qemuStarted,
            peripheralManager, &PeripheralManager::ensureAllRunning);

    /* ---- Bridge event routing ---- */
    connect(controller, &QemuController::i2cTransferRequested,
            peripheralManager, &PeripheralManager::dispatchI2cTransfer);
    connect(controller, &QemuController::spiTransferRequested,
            peripheralManager, &PeripheralManager::dispatchSpiTransfer);
    connect(controller, &QemuController::uartTxRequested,
            peripheralManager, &PeripheralManager::dispatchUartTx);
    connect(peripheralManager, &PeripheralManager::bridgeResponseReady,
            controller, &QemuController::handleBridgeResponse);
    connect(peripheralManager, &PeripheralManager::i2cResponseMapReady,
            controller, &QemuController::setI2cBridgeResponseMap);
    connect(peripheralManager, &PeripheralManager::spiDcGpioReady,
            controller, &QemuController::setSpiDcGpio);

    /* When peripheral config changes (reload, start, stop), re-sync bridge addresses.
     * Use deviceSetChanged (not devicesChanged) to avoid re-pushing addresses
     * on every 500ms state-poll cycle — a major performance bottleneck. */
    connect(peripheralManager, &PeripheralManager::deviceSetChanged,
            this, &MainWindow::syncI2cBridgeAddresses);

        /* Ensure sims are up once wiring is complete */
        peripheralManager->ensureAllRunning();
}

void MainWindow::syncI2cBridgeAddresses()
{
    controller->clearAllI2cBridgeAddresses();
    const auto busAddrs = peripheralManager->getI2cBusAddresses();
    for (auto it = busAddrs.cbegin(); it != busAddrs.cend(); ++it) {
        for (const QString &hex : it.value()) {
            controller->registerI2cBridgeAddress(it.key(), hex);
        }
    }
}
