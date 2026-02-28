#pragma once

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QemuController;

class DebugWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DebugWidget(QWidget *parent = nullptr);
    void setController(QemuController *ctrl);

private slots:
    void browseFirmware();
    void startWithGdbClicked();
    void applyGdbConfig();
    void pauseClicked();
    void continueClicked();
    void stepClicked();
    void addBreakpointClicked();
    void clearBreakpointsClicked();
    void onDebugStatusUpdated(const QString &status);
    void onGdbAttachCommandUpdated(const QString &command);

private:
    QemuController *controller;

    QCheckBox *enableGdbCheck;
    QLineEdit *gdbPortLine;
    QCheckBox *waitForAttachCheck;
    QPushButton *applyGdbButton;
    QLineEdit *firmwarePathLine;
    QPushButton *browseFirmwareButton;
    QPushButton *startWithGdbButton;

    QPushButton *pauseButton;
    QPushButton *continueButton;
    QPushButton *stepButton;

    QLineEdit *breakpointLine;
    QPushButton *addBreakpointButton;
    QPushButton *clearBreakpointsButton;

    QLabel *statusLabel;
    QLabel *gdbHintLabel;
};
