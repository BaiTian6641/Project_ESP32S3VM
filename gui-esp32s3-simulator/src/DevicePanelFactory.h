#pragma once

#include <QJsonObject>
#include <QString>

class DevicePanelBase;
class QWidget;

/// Factory that creates the appropriate DevicePanel subclass based on the
/// device type, compatible list, and bus kind from the peripheral config.
class DevicePanelFactory
{
public:
    /// Create a panel for the given device.
    /// Selection is driven by device-declared capabilities (caps.panel.kind)
    /// so virtual devices can self-register their panel behavior.
    static DevicePanelBase *createPanel(const QString &deviceId,
                                        const QString &deviceType,
                                        const QJsonObject &rawConfig,
                                        const QJsonObject &capabilities,
                                        QWidget *parent = nullptr);
};
