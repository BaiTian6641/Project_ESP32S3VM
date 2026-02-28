#pragma once

#include <QWidget>

class QTextEdit;
class QLineEdit;
class QPushButton;
class QComboBox;
class QemuController;

class SerialConsoleWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SerialConsoleWidget(QWidget *parent = nullptr);
    void setController(QemuController *ctrl);

private slots:
    void onSendClicked();
    void applySerialSettings();
    void appendSerialLine(const QString &line);

private:
    QemuController *controller;
    QTextEdit *outputView;
    QLineEdit *inputLine;
    QPushButton *sendButton;
    QComboBox *commTypeCombo;
    QComboBox *baudRateCombo;
    QComboBox *dataBitsCombo;
    QComboBox *parityCombo;
    QComboBox *stopBitsCombo;
    QComboBox *flowControlCombo;
    QComboBox *lineEndingCombo;
    QPushButton *applySettingsButton;
};
