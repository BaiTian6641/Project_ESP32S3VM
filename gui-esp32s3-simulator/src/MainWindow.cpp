#include "MainWindow.h"

#include <QTabWidget>
#include <QCoreApplication>
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

    const QString workspaceRoot =
        QFileInfo(QCoreApplication::applicationDirPath() + "/../").absoluteFilePath();
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
    peripheralManager->loadDefaultConfig();
    peripheralsWidget->setManager(peripheralManager);

        connect(controller, &QemuController::qemuStarted,
            peripheralManager, &PeripheralManager::startAll);
        connect(controller, &QemuController::qemuStopped,
            peripheralManager, &PeripheralManager::stopAll);
            connect(controller, &QemuController::i2cTransferRequested,
                peripheralManager, &PeripheralManager::dispatchI2cTransfer);
            connect(controller, &QemuController::spiTransferRequested,
                peripheralManager, &PeripheralManager::dispatchSpiTransfer);
            connect(controller, &QemuController::uartTxRequested,
                peripheralManager, &PeripheralManager::dispatchUartTx);
                connect(peripheralManager, &PeripheralManager::bridgeResponseReady,
                    controller, &QemuController::handleBridgeResponse);
}
