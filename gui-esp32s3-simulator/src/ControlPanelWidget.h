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
    void setFirmwarePath(const QString &path);
    QString firmwarePath() const { return selectedFirmware; }

public slots:
    void chooseFirmware();
    void loadFirmware();

signals:
    void firmwareChanged(const QString &path);

private slots:
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
    QString selectedFirmware;
    QPushButton *copyEsptoolButton;
};
