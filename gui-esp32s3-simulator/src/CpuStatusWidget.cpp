#include "CpuStatusWidget.h"

#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "QemuController.h"

CpuStatusWidget::CpuStatusWidget(QWidget *parent)
    : QWidget(parent),
      controller(nullptr),
            pcLabel(new QLabel("PC: 0x00000000", this)),
      scalarRegsTable(new QTableWidget(this)),
      vectorRegsTable(new QTableWidget(this)),
      memoryInspectTable(new QTableWidget(this)),
            refreshButton(new QPushButton("Refresh", this)),
            liveButton(new QPushButton("Live", this)),
            pauseButton(new QPushButton("Pause", this)),
            continueButton(new QPushButton("Continue", this)),
            stepButton(new QPushButton("Step PC", this)),
            memoryBaseLine(new QLineEdit(this)),
            applyMemoryBaseButton(new QPushButton("Apply Mem Base", this)),
            breakpointLine(new QLineEdit(this)),
            addBreakpointButton(new QPushButton("Add Breakpoint", this)),
            clearBreakpointsButton(new QPushButton("Clear Breakpoints", this))
{
        liveButton->setCheckable(true);
        memoryBaseLine->setText("0x3FC80000");
        memoryBaseLine->setPlaceholderText("Memory base address (e.g. 0x3FC80000)");
        breakpointLine->setPlaceholderText("Breakpoint address (e.g. 0x40000000)");

    setupScalarTable();
    setupVectorTable();

    memoryInspectTable->setColumnCount(2);
    memoryInspectTable->setHorizontalHeaderLabels({"Address", "Value"});
    memoryInspectTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    memoryInspectTable->setRowCount(8);
    for (int i = 0; i < 8; ++i) {
        memoryInspectTable->setItem(i, 0, new QTableWidgetItem(QString("0x3FC8%1").arg(i * 4, 4, 16, QLatin1Char('0')).toUpper()));
        memoryInspectTable->setItem(i, 1, new QTableWidgetItem("0x00000000"));
    }

    auto *scalarBox = new QGroupBox("Scalar Registers", this);
    auto *scalarLayout = new QVBoxLayout(scalarBox);
    scalarLayout->addWidget(scalarRegsTable);

    auto *vectorBox = new QGroupBox("Vector Registers", this);
    auto *vectorLayout = new QVBoxLayout(vectorBox);
    vectorLayout->addWidget(vectorRegsTable);

    auto *memoryBox = new QGroupBox("Memory Inspect", this);
    auto *memoryLayout = new QVBoxLayout(memoryBox);
    memoryLayout->addWidget(memoryInspectTable);

    auto *debugControls = new QHBoxLayout();
    debugControls->addWidget(refreshButton);
    debugControls->addWidget(liveButton);
    debugControls->addWidget(pauseButton);
    debugControls->addWidget(continueButton);
    debugControls->addWidget(stepButton);

    auto *memControls = new QHBoxLayout();
    memControls->addWidget(memoryBaseLine);
    memControls->addWidget(applyMemoryBaseButton);

    auto *bpControls = new QHBoxLayout();
    bpControls->addWidget(breakpointLine);
    bpControls->addWidget(addBreakpointButton);
    bpControls->addWidget(clearBreakpointsButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(pcLabel);
    layout->addLayout(debugControls);
    layout->addLayout(memControls);
    layout->addLayout(bpControls);
    layout->addWidget(scalarBox);
    layout->addWidget(vectorBox);
    layout->addWidget(memoryBox);

    connect(refreshButton, &QPushButton::clicked, this, &CpuStatusWidget::refreshStatus);
    connect(liveButton, &QPushButton::clicked, this, &CpuStatusWidget::onLiveToggled);
    connect(pauseButton, &QPushButton::clicked, this, &CpuStatusWidget::onPauseClicked);
    connect(continueButton, &QPushButton::clicked, this, &CpuStatusWidget::onContinueClicked);
    connect(stepButton, &QPushButton::clicked, this, &CpuStatusWidget::onStepClicked);
    connect(applyMemoryBaseButton, &QPushButton::clicked, this, &CpuStatusWidget::onMemoryBaseApply);
    connect(addBreakpointButton, &QPushButton::clicked, this, &CpuStatusWidget::onAddBreakpoint);
    connect(clearBreakpointsButton, &QPushButton::clicked, this, &CpuStatusWidget::onClearBreakpoints);
}

void CpuStatusWidget::setController(QemuController *ctrl)
{
    controller = ctrl;
    if (!controller) {
        return;
    }

    connect(controller, &QemuController::cpuSnapshotUpdated,
            this, &CpuStatusWidget::onSnapshotUpdated);
}

void CpuStatusWidget::refreshStatus()
{
    if (!controller) {
        return;
    }
    controller->requestCpuSnapshot();
}

void CpuStatusWidget::onLiveToggled()
{
    if (!controller) {
        return;
    }
    controller->startLiveUpdates(liveButton->isChecked());
}

void CpuStatusWidget::onPauseClicked()
{
    if (controller) {
        controller->pauseExecution();
    }
}

void CpuStatusWidget::onContinueClicked()
{
    if (controller) {
        controller->continueExecution();
    }
}

void CpuStatusWidget::onStepClicked()
{
    if (controller) {
        controller->stepInstruction();
    }
}

void CpuStatusWidget::onMemoryBaseApply()
{
    if (controller) {
        controller->setMemoryInspectBase(memoryBaseLine->text());
        controller->requestCpuSnapshot();
    }
}

void CpuStatusWidget::onAddBreakpoint()
{
    if (!controller || breakpointLine->text().isEmpty()) {
        return;
    }
    controller->addBreakpoint(breakpointLine->text());
}

void CpuStatusWidget::onClearBreakpoints()
{
    if (controller) {
        controller->clearBreakpoints();
    }
}

void CpuStatusWidget::onSnapshotUpdated(const QString &pc,
                                        const QStringList &scalarRegs,
                                        const QStringList &vectorRegs,
                                        const QStringList &memoryWords)
{
    pcLabel->setText(QString("PC: %1").arg(pc));

    const int scalarCount = qMin(scalarRegsTable->rowCount(), scalarRegs.size());
    for (int i = 0; i < scalarCount; ++i) {
        scalarRegsTable->item(i, 1)->setText(scalarRegs[i]);
    }

    const int vectorCount = qMin(vectorRegsTable->rowCount(), vectorRegs.size());
    for (int i = 0; i < vectorCount; ++i) {
        vectorRegsTable->item(i, 1)->setText(vectorRegs[i]);
    }

    const int memCount = qMin(memoryInspectTable->rowCount(), memoryWords.size());
    bool ok = false;
    const quint32 base = memoryBaseLine->text().toUInt(&ok, 0);
    for (int i = 0; i < memCount; ++i) {
        if (ok) {
            memoryInspectTable->item(i, 0)->setText(
                QString("0x%1").arg(base + (i * 4), 8, 16, QLatin1Char('0')).toUpper());
        }
        memoryInspectTable->item(i, 1)->setText(memoryWords[i]);
    }
}

void CpuStatusWidget::setupScalarTable()
{
    scalarRegsTable->setColumnCount(2);
    scalarRegsTable->setHorizontalHeaderLabels({"Reg", "Value"});
    scalarRegsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    const int regCount = 16;
    scalarRegsTable->setRowCount(regCount);
    for (int i = 0; i < regCount; ++i) {
        scalarRegsTable->setItem(i, 0, new QTableWidgetItem(QString("A%1").arg(i)));
        scalarRegsTable->setItem(i, 1, new QTableWidgetItem("0x00000000"));
    }
}

void CpuStatusWidget::setupVectorTable()
{
    vectorRegsTable->setColumnCount(2);
    vectorRegsTable->setHorizontalHeaderLabels({"Reg", "Value"});
    vectorRegsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    const int vecCount = 8;
    vectorRegsTable->setRowCount(vecCount);
    for (int i = 0; i < vecCount; ++i) {
        vectorRegsTable->setItem(i, 0, new QTableWidgetItem(QString("F%1").arg(i)));
        vectorRegsTable->setItem(i, 1, new QTableWidgetItem("0x00000000"));
    }
}
