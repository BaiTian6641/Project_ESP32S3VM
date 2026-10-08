#pragma once

#include <QMainWindow>

class QTabWidget;
class SerialConsoleWidget;
class CpuStatusWidget;
class ControlPanelWidget;
class DebugWidget;
class QemuController;
class PeripheralManager;
class PeripheralsWidget;
class BoardWorkspace;
class QLabel;
class QPushButton;
struct RuntimeStatus;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void loadFirmware(const QString &path);

private:
    void syncI2cBridgeAddresses();
    void updateRuntimeStatus(const RuntimeStatus &status);
    void showRuntimeCapabilities();

    QTabWidget *tabWidget;
    BoardWorkspace *boardWorkspace;
    SerialConsoleWidget *serialWidget;
    CpuStatusWidget *cpuWidget;
    ControlPanelWidget *controlWidget;
    DebugWidget *debugWidget;
    PeripheralsWidget *peripheralsWidget;
    QemuController *controller;
    PeripheralManager *peripheralManager;
    QLabel *firmwareLabel = nullptr;
    QLabel *runtimeLabel = nullptr;
    QPushButton *openFirmwareButton = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *resetButton = nullptr;
};
