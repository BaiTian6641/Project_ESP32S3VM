#include "ControlPanelWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QClipboard>

#include "QemuController.h"

ControlPanelWidget::ControlPanelWidget(QWidget *parent)
    : QWidget(parent),
      controller(nullptr),
      bootModeCombo(new QComboBox(this)),
            spiFlashEnableCheck(new QCheckBox("Enable SPI Flash", this)),
            spiFlashSizeCombo(new QComboBox(this)),
        psramEnableCheck(new QCheckBox("Enable PSRAM", this)),
        psramSizeCombo(new QComboBox(this)),
        psramModeCombo(new QComboBox(this)),
    baseMacLine(new QLineEdit(this)),
    chipRevisionEnableCheck(new QCheckBox("Override Chip Revision", this)),
    chipRevisionSpin(new QSpinBox(this)),
      firmwarePathLine(new QLineEdit(this)),
      browseButton(new QPushButton("Browse", this)),
      resetButton(new QPushButton("Reset", this)),
      applyBootModeButton(new QPushButton("Apply Boot Mode", this)),
    loadFirmwareButton(new QPushButton("Load Firmware", this)),
    copyEsptoolButton(new QPushButton("Copy esptool Command", this))
{
    bootModeCombo->addItem("Normal Boot");
    bootModeCombo->addItem("Download Boot");

        spiFlashEnableCheck->setChecked(true);
        spiFlashSizeCombo->addItems({"2", "4", "8", "16"});
        spiFlashSizeCombo->setCurrentText("16");

    psramEnableCheck->setChecked(false);
    psramSizeCombo->addItems({"2", "4", "8", "16", "32"});
    psramSizeCombo->setCurrentText("8");
    psramModeCombo->addItems({"QSPI", "OPI"});
    psramModeCombo->setCurrentText("QSPI");
    psramSizeCombo->setEnabled(false);
    psramModeCombo->setEnabled(false);

    baseMacLine->setPlaceholderText("AA:BB:CC:DD:EE:FF (optional)");
    chipRevisionSpin->setRange(0, 399);
    chipRevisionSpin->setValue(0);
    chipRevisionSpin->setEnabled(false);

    auto *fwLayout = new QHBoxLayout();
    fwLayout->addWidget(firmwarePathLine);
    fwLayout->addWidget(browseButton);

    auto *formLayout = new QFormLayout();
    formLayout->addRow("Boot Mode", bootModeCombo);
        formLayout->addRow(spiFlashEnableCheck);
        formLayout->addRow("SPI Flash Size (MB)", spiFlashSizeCombo);
    formLayout->addRow(psramEnableCheck);
    formLayout->addRow("PSRAM Size (MB)", psramSizeCombo);
    formLayout->addRow("PSRAM Mode", psramModeCombo);
    formLayout->addRow("Base MAC", baseMacLine);
    formLayout->addRow(chipRevisionEnableCheck);
    formLayout->addRow("Chip Revision", chipRevisionSpin);
    formLayout->addRow("Firmware", fwLayout);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(resetButton);
    buttonLayout->addWidget(applyBootModeButton);
    buttonLayout->addWidget(loadFirmwareButton);
    buttonLayout->addWidget(copyEsptoolButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(formLayout);
    layout->addLayout(buttonLayout);
    layout->addStretch();

    connect(browseButton, &QPushButton::clicked, this, &ControlPanelWidget::chooseFirmware);
    connect(resetButton, &QPushButton::clicked, this, &ControlPanelWidget::doReset);
    connect(applyBootModeButton, &QPushButton::clicked, this, &ControlPanelWidget::applyBootMode);
    connect(loadFirmwareButton, &QPushButton::clicked, this, &ControlPanelWidget::loadFirmware);
    connect(copyEsptoolButton, &QPushButton::clicked, this, &ControlPanelWidget::copyEsptoolCommand);
    connect(chipRevisionEnableCheck, &QCheckBox::toggled, chipRevisionSpin, &QSpinBox::setEnabled);
    connect(psramEnableCheck, &QCheckBox::toggled, psramSizeCombo, &QComboBox::setEnabled);
    connect(psramEnableCheck, &QCheckBox::toggled, psramModeCombo, &QComboBox::setEnabled);
}

void ControlPanelWidget::setController(QemuController *ctrl)
{
    controller = ctrl;
}

void ControlPanelWidget::chooseFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this, "Select Firmware", QString(), "Binary Files (*.bin *.elf);;All Files (*)");
    if (!path.isEmpty()) {
        firmwarePathLine->setText(path);
    }
}

void ControlPanelWidget::doReset()
{
    if (controller) {
        controller->resetTarget();
    }
}

void ControlPanelWidget::applyBootMode()
{
    if (!controller) {
        return;
    }
    controller->setBootMode(bootModeCombo->currentIndex());
}

void ControlPanelWidget::loadFirmware()
{
    if (!controller || firmwarePathLine->text().isEmpty()) {
        return;
    }

    controller->setSpiFlashConfig(
        spiFlashEnableCheck->isChecked(),
        spiFlashSizeCombo->currentText().toInt());

    controller->setPsramConfig(
        psramEnableCheck->isChecked(),
        psramSizeCombo->currentText().toInt(),
        psramModeCombo->currentText());

    controller->setChipIdentityConfig(
        baseMacLine->text(),
        chipRevisionEnableCheck->isChecked(),
        chipRevisionSpin->value());

    controller->loadFirmware(firmwarePathLine->text());
}

void ControlPanelWidget::copyEsptoolCommand()
{
    if (!controller) {
        return;
    }

    const QString command = controller->recommendedEsptoolCommand(firmwarePathLine->text());
    QGuiApplication::clipboard()->setText(command);
}
