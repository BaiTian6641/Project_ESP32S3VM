#include "MainWindow.h"

#include <QTabWidget>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QDebug>
#include <QComboBox>
#include <QPushButton>
#include <QSettings>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QStyle>
#include <QKeySequence>
#include "BoardWorkspace.h"
#include "ThemeManager.h"

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
      boardWorkspace(new BoardWorkspace(this)),
      serialWidget(new SerialConsoleWidget(this)),
      cpuWidget(new CpuStatusWidget(this)),
      controlWidget(new ControlPanelWidget(this)),
      debugWidget(new DebugWidget(this)),
      peripheralsWidget(new PeripheralsWidget(this)),
      controller(new QemuController(this)),
      peripheralManager(new PeripheralManager(this))
{
    setWindowTitle("ESP32S3VM | Circuit studio");
    resize(1440, 960);
    if (qEnvironmentVariableIntValue("ESP32S3_DIAGNOSTICS") == 1) {
        connect(controller, &QemuController::debugMessageReceived, this, [](const QString &line) { qInfo().noquote() << line; });
        connect(controller, &QemuController::serialLineReceived, this, [](const QString &line) { qInfo().noquote() << line; });
    }

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

    tabWidget->setDocumentMode(true);
    tabWidget->addTab(boardWorkspace, "Circuit");
    tabWidget->addTab(serialWidget, "Serial");
    tabWidget->addTab(cpuWidget, "Processor Status");
    tabWidget->addTab(controlWidget, "Hardware profile");
    tabWidget->addTab(debugWidget, "Debugger");
    tabWidget->addTab(peripheralsWidget, "Peripherals");
    auto *shell = new QWidget(this);
    auto *shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);
    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 8, 16, 8);
    auto *brand = new QLabel("ESP32S3VM   /   Circuit studio", header);
    brand->setObjectName("shellHeader");
    headerLayout->addWidget(brand);
    headerLayout->addStretch();
    auto *capabilitiesButton = new QPushButton("Runtime support", header);
    capabilitiesButton->setToolTip("Inspect available interfaces, validation and limitations for the selected runtime.");
    connect(capabilitiesButton, &QPushButton::clicked, this, &MainWindow::showRuntimeCapabilities);
    headerLayout->addWidget(capabilitiesButton);
    auto *appearanceLabel = new QLabel("Appearance", header);
    auto *appearance = new QComboBox(header);
    appearance->setObjectName("appearanceMode");
    appearance->setAccessibleName("Appearance");
    appearanceLabel->setBuddy(appearance);
    appearance->addItem("System", static_cast<int>(ThemeManager::Mode::System));
    appearance->addItem("Light", static_cast<int>(ThemeManager::Mode::Light));
    appearance->addItem("Dark", static_cast<int>(ThemeManager::Mode::Dark));
    auto *theme = ThemeManager::instance();
    appearance->setCurrentIndex(appearance->findData(static_cast<int>(theme->mode())));
    appearance->setToolTip(theme->appearanceReason());
    headerLayout->addWidget(appearanceLabel);
    headerLayout->addWidget(appearance);
    auto *appearanceInfo = new QLabel(header);
    appearanceInfo->setProperty("tone", "helper");
    headerLayout->addWidget(appearanceInfo);
    auto refreshAppearanceInfo = [theme, appearanceInfo, appearance]() {
        appearance->setToolTip(theme->appearanceReason());
        appearanceInfo->setText(theme->mode() == ThemeManager::Mode::System && !theme->systemAppearanceSupported()
            ? "Palette fallback" : QString());
        appearanceInfo->setToolTip(theme->appearanceReason());
    };
    connect(appearance, &QComboBox::currentIndexChanged, this, [theme, appearance](int index) {
        theme->setMode(static_cast<ThemeManager::Mode>(appearance->itemData(index).toInt()));
    });
    connect(theme, &ThemeManager::themeChanged, this, refreshAppearanceInfo);
    refreshAppearanceInfo();
    header->setObjectName("shellHeader");
    header->setMinimumHeight(48);
    shellLayout->addWidget(header);
    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(16, 8, 16, 8);
    openFirmwareButton = new QPushButton("Open firmware…", toolbar);
    openFirmwareButton->setShortcut(QKeySequence::Open);
    openFirmwareButton->setToolTip("Select a merged flash image or an ELF debug image (Ctrl+O).");
    firmwareLabel = new QLabel("No firmware selected", toolbar);
    firmwareLabel->setObjectName("selectedFirmware");
    firmwareLabel->setProperty("tone", "secondary");
    firmwareLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    runButton = new QPushButton("Run", toolbar);
    runButton->setObjectName("primary");
    runButton->setShortcut(QKeySequence(Qt::Key_F5));
    runButton->setToolTip("Run, pause or resume (F5).");
    stopButton = new QPushButton("Stop", toolbar);
    stopButton->setObjectName("stopSimulation");
    stopButton->setShortcut(QKeySequence(Qt::Key_F6));
    stopButton->setToolTip("Stop the simulation (F6).");
    resetButton = new QPushButton("Reset", toolbar);
    resetButton->setShortcut(QKeySequence("Ctrl+Shift+R"));
    resetButton->setToolTip("Reset the selected target (Ctrl+Shift+R).");
    runtimeLabel = new QLabel(toolbar);
    runtimeLabel->setObjectName("runtimePhase");
    runtimeLabel->setAccessibleName("Simulation state");
    toolbarLayout->addWidget(openFirmwareButton);
    toolbarLayout->addWidget(firmwareLabel, 1);
    toolbarLayout->addWidget(runButton);
    toolbarLayout->addWidget(stopButton);
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(runtimeLabel);
    shellLayout->addWidget(toolbar);
    shellLayout->addWidget(tabWidget, 1);
    setCentralWidget(shell);
    statusBar()->showMessage("ESP32-S3 / QEMU runtime idle");

    serialWidget->setController(controller);
    cpuWidget->setController(controller);
    controlWidget->setController(controller);
    debugWidget->setController(controller);
    connect(openFirmwareButton, &QPushButton::clicked, controlWidget, &ControlPanelWidget::chooseFirmware);
    connect(runButton, &QPushButton::clicked, this, [this]() {
        const auto phase = controller->runtimeStatus().phase;
        if (phase == RuntimePhase::Running) controller->pauseExecution();
        else if (phase == RuntimePhase::Paused || phase == RuntimePhase::WaitingForDebugger) controller->continueExecution();
        else controlWidget->loadFirmware();
    });
    connect(stopButton, &QPushButton::clicked, controller, &QemuController::stopSimulation);
    connect(resetButton, &QPushButton::clicked, controller, &QemuController::resetTarget);
    connect(controlWidget, &ControlPanelWidget::firmwareChanged, this, [this](const QString &path) {
        firmwareLabel->setText(path.isEmpty() ? "No firmware selected" : QFileInfo(path).fileName());
        firmwareLabel->setToolTip(path + (QFileInfo(path).suffix().toLower() == "elf" ? "\nELF debug image; bypasses normal flash boot." : "\nMerged flash image; application-only BIN files are rejected."));
        QSettings().setValue("session/lastFirmware", path);
        updateRuntimeStatus(controller->runtimeStatus());
    });
    connect(controller, &QemuController::runtimeStatusChanged, this, &MainWindow::updateRuntimeStatus);
    connect(controller, &QemuController::runtimeStatusChanged,
            boardWorkspace, &BoardWorkspace::setRuntimeStatus);
    boardWorkspace->setRuntimeStatus(controller->runtimeStatus());
    connect(controller, &QemuController::nativeCircuitAvailableChanged,
            boardWorkspace, &BoardWorkspace::setNativeCircuitAvailable);
    connect(controller, &QemuController::nativeCircuitApplyInFlightChanged,
            boardWorkspace, &BoardWorkspace::setNativeApplyInFlight);
    connect(controller, &QemuController::nativeCircuitSnapshotUpdated,
            boardWorkspace, &BoardWorkspace::setNativeCircuitSnapshot);
    connect(controller, &QemuController::nativeCircuitSnapshotUnavailable, this,
            [this](const QString &reason) { statusBar()->showMessage(reason); });
    connect(boardWorkspace, &BoardWorkspace::nativeApplyRequested, this,
            [this](const QJsonObject &document) { controller->applyNativeCircuit(document); });
    boardWorkspace->setNativeCircuitAvailable(controller->nativeCircuitAvailable(),
                                               controller->nativeCircuitUnavailableReason());
    const QString previousFirmware = QSettings().value("session/lastFirmware").toString();
    if (QFileInfo::exists(previousFirmware)) controlWidget->setFirmwarePath(previousFirmware);
    updateRuntimeStatus(controller->runtimeStatus());

    /* Load & auto-start peripheral config before QEMU launches */
    peripheralManager->loadDefaultConfig();
    peripheralsWidget->setManager(peripheralManager);
    boardWorkspace->setManager(peripheralManager);
    connect(controller, &QemuController::peripheralBridgeAvailable, boardWorkspace, &BoardWorkspace::setBridgeAvailable);

    /* Pre-load I2C bridge addresses from peripheral config so that
     * when QMP becomes ready the addresses are immediately pushed. */
    syncI2cBridgeAddresses();

    /* ---- Route debug/status messages to peripherals log, not serial ---- */
    connect(controller, &QemuController::debugMessageReceived,
            peripheralManager, &PeripheralManager::managerMessage);

    /* ---- QEMU lifecycle → peripheral sims ---- */
    connect(controller, &QemuController::qemuStarted,
            peripheralManager, &PeripheralManager::ensureAllRunning);
    connect(controller, &QemuController::qemuStopped,
            peripheralManager, &PeripheralManager::stopAll);

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

MainWindow::~MainWindow()
{
    // Stop processes while the derived window and all panels still exist.
    // Their shutdown signals must not call MainWindow after its destructor.
    disconnect(controller, nullptr, nullptr, nullptr);
    disconnect(peripheralManager, nullptr, nullptr, nullptr);
    delete controller;
    delete peripheralManager;
}

void MainWindow::loadFirmware(const QString &path)
{
    controlWidget->setFirmwarePath(path);
    controlWidget->loadFirmware();
}

void MainWindow::updateRuntimeStatus(const RuntimeStatus &status)
{
    runtimeLabel->setText(runtimePhaseLabel(status.phase));
    runtimeLabel->setProperty("phase", runtimePhaseKey(status.phase));
    runtimeLabel->setToolTip(status.message);
    runtimeLabel->setProperty("tone", status.phase == RuntimePhase::Error ? "error" :
        status.phase == RuntimePhase::Running ? "success" : "info");
    runtimeLabel->style()->unpolish(runtimeLabel);
    runtimeLabel->style()->polish(runtimeLabel);
    const bool running = status.phase == RuntimePhase::Running;
    const bool paused = status.phase == RuntimePhase::Paused || status.phase == RuntimePhase::WaitingForDebugger;
    const bool idle = status.phase == RuntimePhase::Idle || status.phase == RuntimePhase::Stopped || status.phase == RuntimePhase::Error;
    runButton->setText(running ? "Pause" : paused ? "Resume" : "Run");
    runButton->setEnabled(running || paused || (idle && !controlWidget->firmwarePath().isEmpty()));
    openFirmwareButton->setEnabled(idle);
    stopButton->setEnabled(status.phase != RuntimePhase::Idle && status.phase != RuntimePhase::Stopped && status.phase != RuntimePhase::Stopping);
    resetButton->setEnabled(running || paused);
    statusBar()->showMessage(status.message.isEmpty() ? runtimePhaseLabel(status.phase) : status.message);
}

void MainWindow::showRuntimeCapabilities()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Runtime support and validation");
    dialog.resize(1040, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *notice = new QLabel("Detected interfaces describe this executable. Native firmware and hardware validation are recorded separately; unavailable features remain visible.", &dialog);
    notice->setWordWrap(true);
    notice->setObjectName("notice");
    layout->addWidget(notice);
    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({"Feature", "Availability", "Validation", "Details"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    auto fill = [table](const QList<RuntimeCapability> &capabilities) {
        table->setRowCount(capabilities.size());
        for (int row = 0; row < capabilities.size(); ++row) {
            const auto &capability = capabilities.at(row);
            const QStringList cells{capability.title, capability.available ? "Available" : "Unavailable",
                capabilityMaturityLabel(capability.maturity), capability.reason};
            for (int column = 0; column < cells.size(); ++column) {
                auto *item = new QTableWidgetItem(cells.at(column));
                item->setToolTip(column == 3 ? capability.reason : capability.evidence.join('\n'));
                table->setItem(row, column, item);
            }
        }
        table->resizeRowsToContents();
    };
    fill(controller->runtimeCapabilities());
    connect(controller, &QemuController::runtimeCapabilitiesChanged, &dialog, fill);
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
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
