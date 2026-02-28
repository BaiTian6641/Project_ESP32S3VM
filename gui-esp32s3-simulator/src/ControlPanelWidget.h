#pragma once

#include <QWidget>

class QComboBox;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QemuController;

class ControlPanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlPanelWidget(QWidget *parent = nullptr);
    void setController(QemuController *ctrl);

private slots:
    void chooseFirmware();
    void doReset();
    void applyBootMode();
    void loadFirmware();
    void copyEsptoolCommand();

private:
    QemuController *controller;
    QComboBox *bootModeCombo;
    QCheckBox *spiFlashEnableCheck;
    QComboBox *spiFlashSizeCombo;
    QCheckBox *psramEnableCheck;
    QComboBox *psramSizeCombo;
    QComboBox *psramModeCombo;
    QLineEdit *baseMacLine;
    QCheckBox *chipRevisionEnableCheck;
    QSpinBox *chipRevisionSpin;
    QLineEdit *firmwarePathLine;
    QPushButton *browseButton;
    QPushButton *resetButton;
    QPushButton *applyBootModeButton;
    QPushButton *loadFirmwareButton;
    QPushButton *copyEsptoolButton;
};
