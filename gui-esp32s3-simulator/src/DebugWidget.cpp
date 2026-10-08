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
      pauseButton(new QPushButton("Pause", this)),
      continueButton(new QPushButton("Continue", this)),
      stepButton(new QPushButton("Step PC", this)),
      breakpointLine(new QLineEdit(this)),
      addBreakpointButton(new QPushButton("Add Breakpoint", this)),
      clearBreakpointsButton(new QPushButton("Clear Breakpoints", this)),
      statusLabel(new QLabel("[GDB] disabled", this)),
      gdbHintLabel(new QLabel(this))
{
    enableGdbCheck->setChecked(false);
    gdbPortLine->setText("1234");
    breakpointLine->setPlaceholderText("0x40000000");

    gdbHintLabel->setText("gdb -ex \"target remote 127.0.0.1:1234\"");

    auto *gdbForm = new QFormLayout();
    gdbForm->addRow(enableGdbCheck);
    gdbForm->addRow("GDB Port", gdbPortLine);
    gdbForm->addRow(waitForAttachCheck);

    auto *gdbLayout = new QVBoxLayout();
    gdbLayout->addLayout(gdbForm);
    gdbLayout->addWidget(applyGdbButton);
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
    auto *notice = new QLabel("Apply debugger settings, then run the selected firmware from the header. Step and breakpoint controls become available after a supported debugger path is verified.", this);
    notice->setObjectName("notice");
    notice->setWordWrap(true);
    layout->addWidget(notice);
    layout->addWidget(gdbBox);
    layout->addWidget(execBox);
    layout->addWidget(bpBox);
    layout->addWidget(statusLabel);
    layout->addStretch();

    connect(applyGdbButton, &QPushButton::clicked, this, &DebugWidget::applyGdbConfig);
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
    controller->setGdbServerConfig(false, 1234, false);
    auto updateControls = [this]() {
        bool step = false, breakpoints = false;
        for (const auto &capability : controller->runtimeCapabilities()) {
            if (capability.id == "debug.step") step = capability.available;
            if (capability.id == "debug.breakpoints") breakpoints = capability.available;
        }
        const auto phase = controller->runtimeStatus().phase;
        pauseButton->setEnabled(phase == RuntimePhase::Running);
        continueButton->setEnabled(phase == RuntimePhase::Paused || phase == RuntimePhase::WaitingForDebugger);
        stepButton->setEnabled(step && phase == RuntimePhase::Paused);
        addBreakpointButton->setEnabled(breakpoints);
        clearBreakpointsButton->setEnabled(breakpoints);
        breakpointLine->setEnabled(breakpoints);
        stepButton->setToolTip(step ? "Single instruction step" : "A verified GDB stepping path is not available in this runtime.");
        breakpointLine->setToolTip(breakpoints ? "Breakpoint address" : "A verified breakpoint path is not available in this runtime.");
    };
    connect(controller, &QemuController::runtimeStatusChanged, this, [updateControls](const RuntimeStatus &) { updateControls(); });
    connect(controller, &QemuController::runtimeCapabilitiesChanged, this, [updateControls](const QList<RuntimeCapability> &) { updateControls(); });
    updateControls();
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
