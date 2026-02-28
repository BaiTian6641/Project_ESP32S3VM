#include "MainWindow.h"

#include <QTabWidget>

#include "SerialConsoleWidget.h"
#include "CpuStatusWidget.h"
#include "ControlPanelWidget.h"
#include "DebugWidget.h"
#include "QemuController.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      tabWidget(new QTabWidget(this)),
      serialWidget(new SerialConsoleWidget(this)),
      cpuWidget(new CpuStatusWidget(this)),
      controlWidget(new ControlPanelWidget(this)),
    debugWidget(new DebugWidget(this)),
      controller(new QemuController(this))
{
    setWindowTitle("ESP32-S3 GUI Simulator (MVP)");
    resize(1200, 800);

    tabWidget->addTab(serialWidget, "Serial");
    tabWidget->addTab(cpuWidget, "Processor Status");
    tabWidget->addTab(controlWidget, "Control");
    tabWidget->addTab(debugWidget, "Debug");
    setCentralWidget(tabWidget);

    serialWidget->setController(controller);
    cpuWidget->setController(controller);
    controlWidget->setController(controller);
    debugWidget->setController(controller);
}
