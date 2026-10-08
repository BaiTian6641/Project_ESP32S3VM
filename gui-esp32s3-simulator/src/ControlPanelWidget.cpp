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
#include <QLabel>

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
    copyEsptoolButton(new QPushButton("Copy esptool Command", this))
{
    bootModeCombo->addItem("Normal Boot");
    bootModeCombo->addItem("Download Boot");

        spiFlashEnableCheck->setChecked(true);
        spiFlashSizeCombo->addItems({"2", "4", "8", "16"});
        spiFlashSizeCombo->setCurrentText("4");

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

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(copyEsptoolButton);

    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel("Choose firmware and run it from the header. Boot mode, flash, PSRAM and chip settings apply on the next run.", this);
    notice->setWordWrap(true);
    notice->setObjectName("notice");
    layout->addWidget(notice);
    layout->addLayout(formLayout);
    layout->addLayout(buttonLayout);
    layout->addStretch();

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
        setFirmwarePath(path);
    }
}

void ControlPanelWidget::setFirmwarePath(const QString &path)
{
    if (selectedFirmware == path) return;
    selectedFirmware = path;
    emit firmwareChanged(path);
}

void ControlPanelWidget::loadFirmware()
{
    if (!controller || selectedFirmware.isEmpty()) {
        return;
    }

    controller->setBootMode(bootModeCombo->currentIndex());

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

    controller->loadFirmware(selectedFirmware);
}

void ControlPanelWidget::copyEsptoolCommand()
{
    if (!controller) {
        return;
    }

    const QString command = controller->recommendedEsptoolCommand(selectedFirmware);
    QGuiApplication::clipboard()->setText(command);
}
