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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void syncI2cBridgeAddresses();

    QTabWidget *tabWidget;
    SerialConsoleWidget *serialWidget;
    CpuStatusWidget *cpuWidget;
    ControlPanelWidget *controlWidget;
    DebugWidget *debugWidget;
    PeripheralsWidget *peripheralsWidget;
    QemuController *controller;
    PeripheralManager *peripheralManager;
};
