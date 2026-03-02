#pragma once

#include "DevicePanelBase.h"

class QTextEdit;

/// Fallback panel for devices that don't match any specialized type.
/// Shows raw JSON state and capabilities.
class GenericDevicePanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit GenericDevicePanel(const QString &deviceId,
                                const QString &deviceType,
                                const QJsonObject &rawConfig,
                                QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    void buildUI();

    QTextEdit *m_stateView = nullptr;
    QTextEdit *m_capsView = nullptr;
};
