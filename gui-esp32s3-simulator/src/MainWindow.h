#pragma once

#include <QMainWindow>

class QTabWidget;
class SerialConsoleWidget;
class CpuStatusWidget;
class ControlPanelWidget;
class DebugWidget;
class QemuController;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    QTabWidget *tabWidget;
    SerialConsoleWidget *serialWidget;
    CpuStatusWidget *cpuWidget;
    ControlPanelWidget *controlWidget;
    DebugWidget *debugWidget;
    QemuController *controller;
};
