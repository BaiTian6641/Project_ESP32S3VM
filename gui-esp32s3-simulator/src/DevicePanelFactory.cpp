#include "DevicePanelFactory.h"

#include "SchemaDevicePanel.h"
#include "GenericDevicePanel.h"

DevicePanelBase *DevicePanelFactory::createPanel(const QString &deviceId,
                                                  const QString &deviceType,
                                                  const QJsonObject &rawConfig,
                                                  const QJsonObject &capabilities,
                                                  QWidget *parent)
{
    if (!capabilities.value("panel").toObject().isEmpty()) {
        return new SchemaDevicePanel(deviceId, deviceType, rawConfig, parent);
    }

    // Generic fallback for unknown/unspecified panel kinds.
    return new GenericDevicePanel(deviceId, deviceType, rawConfig, parent);
}

