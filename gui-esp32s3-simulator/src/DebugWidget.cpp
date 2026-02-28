#include "DebugWidget.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "QemuController.h"

DebugWidget::DebugWidget(QWidget *parent)
    : QWidget(parent),
      controller(nullptr),
      enableGdbCheck(new QCheckBox("Enable GDB server", this)),
      gdbPortLine(new QLineEdit(this)),
      waitForAttachCheck(new QCheckBox("Wait for debugger attach (-S)", this)),
      applyGdbButton(new QPushButton("Apply GDB Config", this)),
    firmwarePathLine(new QLineEdit(this)),
    browseFirmwareButton(new QPushButton("Browse", this)),
    startWithGdbButton(new QPushButton("Start with GDB", this)),
      pauseButton(new QPushButton("Pause", this)),
      continueButton(new QPushButton("Continue", this)),
      stepButton(new QPushButton("Step PC", this)),
      breakpointLine(new QLineEdit(this)),
      addBreakpointButton(new QPushButton("Add Breakpoint", this)),
      clearBreakpointsButton(new QPushButton("Clear Breakpoints", this)),
      statusLabel(new QLabel("[GDB] disabled", this)),
      gdbHintLabel(new QLabel(this))
{
    enableGdbCheck->setChecked(true);
    gdbPortLine->setText("1234");
    breakpointLine->setPlaceholderText("0x40000000");
    firmwarePathLine->setPlaceholderText("Select firmware (.elf/.bin)");

    gdbHintLabel->setText("gdb -ex \"target remote 127.0.0.1:1234\"");

    auto *gdbForm = new QFormLayout();
    gdbForm->addRow(enableGdbCheck);
    gdbForm->addRow("GDB Port", gdbPortLine);
    gdbForm->addRow(waitForAttachCheck);

    auto *fwRow = new QHBoxLayout();
    fwRow->addWidget(firmwarePathLine);
    fwRow->addWidget(browseFirmwareButton);
    gdbForm->addRow("Firmware", fwRow);

    auto *gdbLayout = new QVBoxLayout();
    gdbLayout->addLayout(gdbForm);
    gdbLayout->addWidget(applyGdbButton);
    gdbLayout->addWidget(startWithGdbButton);
    gdbLayout->addWidget(gdbHintLabel);

    auto *gdbBox = new QGroupBox("GDB Server", this);
    gdbBox->setLayout(gdbLayout);

    auto *execLayout = new QHBoxLayout();
    execLayout->addWidget(pauseButton);
    execLayout->addWidget(continueButton);
    execLayout->addWidget(stepButton);

    auto *execBoxLayout = new QVBoxLayout();
    execBoxLayout->addLayout(execLayout);

    auto *execBox = new QGroupBox("Execution Control", this);
    execBox->setLayout(execBoxLayout);

    auto *bpLayout = new QHBoxLayout();
    bpLayout->addWidget(breakpointLine);
    bpLayout->addWidget(addBreakpointButton);
    bpLayout->addWidget(clearBreakpointsButton);

    auto *bpBox = new QGroupBox("Breakpoints", this);
    auto *bpBoxLayout = new QVBoxLayout();
    bpBoxLayout->addLayout(bpLayout);
    bpBox->setLayout(bpBoxLayout);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(gdbBox);
    layout->addWidget(execBox);
    layout->addWidget(bpBox);
    layout->addWidget(statusLabel);
    layout->addStretch();

    connect(applyGdbButton, &QPushButton::clicked, this, &DebugWidget::applyGdbConfig);
    connect(browseFirmwareButton, &QPushButton::clicked, this, &DebugWidget::browseFirmware);
    connect(startWithGdbButton, &QPushButton::clicked, this, &DebugWidget::startWithGdbClicked);
    connect(pauseButton, &QPushButton::clicked, this, &DebugWidget::pauseClicked);
    connect(continueButton, &QPushButton::clicked, this, &DebugWidget::continueClicked);
    connect(stepButton, &QPushButton::clicked, this, &DebugWidget::stepClicked);
    connect(addBreakpointButton, &QPushButton::clicked, this, &DebugWidget::addBreakpointClicked);
    connect(clearBreakpointsButton, &QPushButton::clicked, this, &DebugWidget::clearBreakpointsClicked);
}

void DebugWidget::setController(QemuController *ctrl)
{
    controller = ctrl;
    if (!controller) {
        return;
    }

    connect(controller, &QemuController::debugStatusUpdated,
            this, &DebugWidget::onDebugStatusUpdated);
    connect(controller, &QemuController::gdbAttachCommandUpdated,
            this, &DebugWidget::onGdbAttachCommandUpdated);
}

void DebugWidget::browseFirmware()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Select Firmware",
        QString(),
        "Firmware Files (*.elf *.bin);;All Files (*)");
    if (!path.isEmpty()) {
        firmwarePathLine->setText(path);
    }
}

void DebugWidget::startWithGdbClicked()
{
    if (!controller) {
        return;
    }

    bool ok = false;
    const int port = gdbPortLine->text().toInt(&ok);
    const int safePort = ok ? port : 1234;

    const QString firmwarePath = firmwarePathLine->text().trimmed();
    if (firmwarePath.isEmpty()) {
        browseFirmware();
    }

    if (firmwarePathLine->text().trimmed().isEmpty()) {
        return;
    }

    enableGdbCheck->setChecked(true);
    controller->startWithGdb(firmwarePathLine->text().trimmed(),
                             safePort,
                             waitForAttachCheck->isChecked());
}

void DebugWidget::applyGdbConfig()
{
    if (!controller) {
        return;
    }

    bool ok = false;
    const int port = gdbPortLine->text().toInt(&ok);
    const int safePort = ok ? port : 1234;

    controller->setGdbServerConfig(enableGdbCheck->isChecked(),
                                   safePort,
                                   waitForAttachCheck->isChecked());

    gdbHintLabel->setText(QString("gdb -ex \"target remote 127.0.0.1:%1\"").arg(safePort));
}

void DebugWidget::pauseClicked()
{
    if (controller) {
        controller->pauseExecution();
    }
}

void DebugWidget::continueClicked()
{
    if (controller) {
        controller->continueExecution();
    }
}

void DebugWidget::stepClicked()
{
    if (controller) {
        controller->stepInstruction();
    }
}

void DebugWidget::addBreakpointClicked()
{
    if (!controller || breakpointLine->text().isEmpty()) {
        return;
    }
    controller->addBreakpoint(breakpointLine->text().trimmed());
}

void DebugWidget::clearBreakpointsClicked()
{
    if (controller) {
        controller->clearBreakpoints();
    }
}

void DebugWidget::onDebugStatusUpdated(const QString &status)
{
    statusLabel->setText(status);
}

void DebugWidget::onGdbAttachCommandUpdated(const QString &command)
{
    gdbHintLabel->setText(command);
}
