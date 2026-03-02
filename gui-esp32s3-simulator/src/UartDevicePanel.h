#pragma once

#include "DevicePanelBase.h"

class QTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;

/// Panel for UART loopback / passthrough devices.  Shows a mini terminal
/// with TX/RX traffic and lets the user send data manually.
class UartDevicePanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit UartDevicePanel(const QString &deviceId,
                             const QString &deviceType,
                             const QJsonObject &rawConfig,
                             QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    void buildUI();

    QTextEdit *m_trafficView = nullptr;
    QLineEdit *m_sendInput = nullptr;
    QPushButton *m_sendButton = nullptr;
    QLabel *m_configLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    int m_rxCount = 0;
    int m_txCount = 0;
};
