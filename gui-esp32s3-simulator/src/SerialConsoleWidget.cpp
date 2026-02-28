#include "SerialConsoleWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "QemuController.h"

SerialConsoleWidget::SerialConsoleWidget(QWidget *parent)
    : QWidget(parent),
      controller(nullptr),
      outputView(new QTextEdit(this)),
      inputLine(new QLineEdit(this)),
            sendButton(new QPushButton("Send", this)),
            commTypeCombo(new QComboBox(this)),
            baudRateCombo(new QComboBox(this)),
            dataBitsCombo(new QComboBox(this)),
            parityCombo(new QComboBox(this)),
            stopBitsCombo(new QComboBox(this)),
            flowControlCombo(new QComboBox(this)),
            lineEndingCombo(new QComboBox(this)),
            applySettingsButton(new QPushButton("Apply Serial Settings", this))
{
    outputView->setReadOnly(true);
    outputView->setPlaceholderText("UART0 output will appear here...");
    inputLine->setPlaceholderText("Type data to send to UART0...");

        commTypeCombo->addItems({"UART TTL", "RS-232", "RS-485"});

        baudRateCombo->setEditable(true);
        baudRateCombo->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
        baudRateCombo->setCurrentText("115200");

        dataBitsCombo->addItems({"5", "6", "7", "8"});
        dataBitsCombo->setCurrentText("8");

        parityCombo->addItems({"None", "Even", "Odd", "Mark", "Space"});
        parityCombo->setCurrentText("None");
        stopBitsCombo->addItems({"1", "2"});
        stopBitsCombo->setCurrentText("1");
        flowControlCombo->addItems({"None", "RTS/CTS", "XON/XOFF"});
        flowControlCombo->setCurrentText("None");
        lineEndingCombo->addItems({"LF", "CRLF", "CR", "None"});
        lineEndingCombo->setCurrentText("LF");

        auto *settingsForm = new QFormLayout();
        settingsForm->addRow("Communication", commTypeCombo);
        settingsForm->addRow("Baud Rate", baudRateCombo);
        settingsForm->addRow("Data Bits", dataBitsCombo);
        settingsForm->addRow("Parity", parityCombo);
        settingsForm->addRow("Stop Bits", stopBitsCombo);
        settingsForm->addRow("Flow Control", flowControlCombo);
        settingsForm->addRow("Line Ending", lineEndingCombo);

        auto *settingsBox = new QGroupBox("UART0 Serial Settings", this);
        auto *settingsLayout = new QVBoxLayout(settingsBox);
        settingsLayout->addLayout(settingsForm);
        settingsLayout->addWidget(applySettingsButton);

    auto *inputLayout = new QHBoxLayout();
    inputLayout->addWidget(inputLine);
    inputLayout->addWidget(sendButton);

    auto *layout = new QVBoxLayout(this);
        layout->addWidget(settingsBox);
    layout->addWidget(outputView);
    layout->addLayout(inputLayout);

    connect(sendButton, &QPushButton::clicked, this, &SerialConsoleWidget::onSendClicked);
    connect(inputLine, &QLineEdit::returnPressed, this, &SerialConsoleWidget::onSendClicked);
        connect(applySettingsButton, &QPushButton::clicked, this, &SerialConsoleWidget::applySerialSettings);
}

void SerialConsoleWidget::setController(QemuController *ctrl)
{
    controller = ctrl;
    if (!controller) {
        return;
    }

    connect(controller, &QemuController::serialLineReceived,
            this, &SerialConsoleWidget::appendSerialLine);

    applySerialSettings();
}

void SerialConsoleWidget::onSendClicked()
{
    if (!controller) {
        return;
    }

    const QString text = inputLine->text();
    if (text.isEmpty()) {
        return;
    }

    QString payload = text;
    const QString ending = lineEndingCombo->currentText();
    if (ending == "LF") {
        payload += "\n";
    } else if (ending == "CRLF") {
        payload += "\r\n";
    } else if (ending == "CR") {
        payload += "\r";
    }

    controller->sendUart0(payload);
    inputLine->clear();
}

void SerialConsoleWidget::applySerialSettings()
{
    if (!controller) {
        return;
    }

    bool ok = false;
    const int baud = baudRateCombo->currentText().toInt(&ok);
    const int safeBaud = ok ? baud : 115200;

    controller->setSerialConfig(
        commTypeCombo->currentText(),
        safeBaud,
        dataBitsCombo->currentText().toInt(),
        parityCombo->currentText(),
        stopBitsCombo->currentText().toInt(),
        flowControlCombo->currentText(),
        lineEndingCombo->currentText());
}

void SerialConsoleWidget::appendSerialLine(const QString &line)
{
    outputView->append(line);
}
