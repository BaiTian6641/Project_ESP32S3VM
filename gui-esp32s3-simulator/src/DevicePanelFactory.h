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
    /// Create a panel for the given device config. Returns a specialized panel
    /// (DisplayPanel, SensorPanel, etc.) or a GenericDevicePanel as fallback.
    static DevicePanelBase *createPanel(const QString &deviceId,
                                        const QString &deviceType,
                                        const QJsonObject &rawConfig,
                                        QWidget *parent = nullptr);

private:
    static bool isDisplayDevice(const QString &type, const QJsonObject &config);
    static bool isSensorDevice(const QString &type, const QJsonObject &config);
    static bool isSpiFlashDevice(const QString &type, const QJsonObject &config);
    static bool isUartDevice(const QString &type, const QJsonObject &config);
};
