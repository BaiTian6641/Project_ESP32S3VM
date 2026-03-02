#include "UartDevicePanel.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QJsonArray>

UartDevicePanel::UartDevicePanel(const QString &deviceId,
                                 const QString &deviceType,
                                 const QJsonObject &rawConfig,
                                 QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    buildUI();
}

void UartDevicePanel::buildUI()
{
    auto *layout = contentLayout();

    // UART configuration info
    auto *cfgGroup = new QGroupBox("UART Configuration", this);
    auto *cfgLayout = new QHBoxLayout(cfgGroup);

    const QJsonObject bus = rawConfig().value("bus").toObject();
    int baud = bus.value("baud").toInt(115200);
    int dataBits = bus.value("data_bits").toInt(8);
    QString parity = bus.value("parity").toString("none");
    int stopBits = bus.value("stop_bits").toInt(1);

    m_configLabel = new QLabel(
        QString("%1 %2%3%4")
            .arg(baud).arg(dataBits).arg(parity[0].toUpper()).arg(stopBits),
        this);
    m_configLabel->setStyleSheet("font-weight: bold; font-family: monospace; font-size: 14px;");
    cfgLayout->addWidget(m_configLabel);

    m_statsLabel = new QLabel("TX: 0  RX: 0", this);
    m_statsLabel->setStyleSheet("color: #888;");
    cfgLayout->addWidget(m_statsLabel, 1, Qt::AlignRight);

    layout->addWidget(cfgGroup);

    // Traffic view
    auto *trafficGroup = new QGroupBox("Traffic Monitor", this);
    auto *trafficLayout = new QVBoxLayout(trafficGroup);

    m_trafficView = new QTextEdit(this);
    m_trafficView->setReadOnly(true);
    m_trafficView->setFont(QFont("Courier New", 10));
    m_trafficView->setStyleSheet(
        "background: #0a0a0a; color: #00ff00; border: 1px solid #333;");
    m_trafficView->setPlaceholderText("UART traffic will appear here...");
    trafficLayout->addWidget(m_trafficView, 1);

    // Send bar
    auto *sendLayout = new QHBoxLayout();
    m_sendInput = new QLineEdit(this);
    m_sendInput->setPlaceholderText("Type data to send...");
    m_sendButton = new QPushButton("Send", this);
    sendLayout->addWidget(m_sendInput, 1);
    sendLayout->addWidget(m_sendButton);
    trafficLayout->addLayout(sendLayout);

    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        const QString text = m_sendInput->text();
        if (text.isEmpty()) return;
        m_trafficView->append(QString("<span style='color: #ffcc00;'>[TX] %1</span>").arg(text));
        m_txCount += text.size();
        m_statsLabel->setText(QString("TX: %1  RX: %2").arg(m_txCount).arg(m_rxCount));
        // Convert text to byte array for RPC
        QJsonArray data;
        for (const QChar &c : text) {
            data.append(static_cast<int>(c.toLatin1()));
        }
        QJsonObject params;
        params["data"] = data;
        emit parameterChangeRequested(deviceId(), "__rpc:uart_tx", QVariant::fromValue(params));
        m_sendInput->clear();
    });

    connect(m_sendInput, &QLineEdit::returnPressed, m_sendButton, &QPushButton::click);

    layout->addWidget(trafficGroup, 1);
}

void UartDevicePanel::updateState(const QJsonObject &state)
{
    if (state.contains("uart_rx")) {
        const QJsonObject rx = state.value("uart_rx").toObject();
        const QJsonArray data = rx.value("data").toArray();
        QString text;
        for (const QJsonValue &v : data) {
            int byte = v.toInt();
            if (byte >= 32 && byte < 127) {
                text += QChar(byte);
            } else {
                text += QString("\\x%1").arg(byte, 2, 16, QChar('0'));
            }
        }
        if (!text.isEmpty()) {
            m_trafficView->append(QString("<span style='color: #80ff80;'>[RX] %1</span>").arg(text));
            m_rxCount += data.size();
            m_statsLabel->setText(QString("TX: %1  RX: %2").arg(m_txCount).arg(m_rxCount));
        }
    }
}

void UartDevicePanel::updateCapabilities(const QJsonObject &caps)
{
    Q_UNUSED(caps);
}
