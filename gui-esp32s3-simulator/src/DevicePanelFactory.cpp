#include "DevicePanelFactory.h"

#include "DisplayPanel.h"
#include "SensorPanel.h"
#include "SpiFlashPanel.h"
#include "UartDevicePanel.h"
#include "GenericDevicePanel.h"

#include <QJsonArray>

DevicePanelBase *DevicePanelFactory::createPanel(const QString &deviceId,
                                                  const QString &deviceType,
                                                  const QJsonObject &rawConfig,
                                                  QWidget *parent)
{
    if (isDisplayDevice(deviceType, rawConfig)) {
        return new DisplayPanel(deviceId, deviceType, rawConfig, parent);
    }
    if (isSensorDevice(deviceType, rawConfig)) {
        return new SensorPanel(deviceId, deviceType, rawConfig, parent);
    }
    if (isSpiFlashDevice(deviceType, rawConfig)) {
        return new SpiFlashPanel(deviceId, deviceType, rawConfig, parent);
    }
    if (isUartDevice(deviceType, rawConfig)) {
        return new UartDevicePanel(deviceId, deviceType, rawConfig, parent);
    }
    return new GenericDevicePanel(deviceId, deviceType, rawConfig, parent);
}

bool DevicePanelFactory::isDisplayDevice(const QString &type, const QJsonObject &config)
{
    const QString lower = type.toLower();
    if (lower.contains("ssd1306") || lower.contains("ssd1309") ||
        lower.contains("ssd1351") || lower.contains("sh1106") ||
        lower.contains("st7735") || lower.contains("st7789") ||
        lower.contains("ili9341") || lower.contains("ili9488") ||
        lower.contains("display") || lower.contains("oled") ||
        lower.contains("lcd") || lower.contains("tft")) {
        return true;
    }

    // Check compatible list
    const QJsonArray compat = config.value("compatible").toArray();
    for (const QJsonValue &v : compat) {
        const QString c = v.toString().toLower();
        if (c.contains("ssd1306") || c.contains("display") ||
            c.contains("oled") || c.contains("lcd") ||
            c.contains("sh1106") || c.contains("st7789") ||
            c.contains("ili9341")) {
            return true;
        }
    }

    return false;
}

bool DevicePanelFactory::isSensorDevice(const QString &type, const QJsonObject &config)
{
    const QString lower = type.toLower();
    if (lower.contains("sht") || lower.contains("bme") || lower.contains("bmp") ||
        lower.contains("dht") || lower.contains("htu") || lower.contains("shtc") ||
        lower.contains("aht") || lower.contains("lm75") || lower.contains("tmp") ||
        lower.contains("mpu") || lower.contains("adxl") || lower.contains("lis") ||
        lower.contains("sensor") || lower.contains("accel") ||
        lower.contains("gyro") || lower.contains("mag")) {
        return true;
    }

    const QJsonArray compat = config.value("compatible").toArray();
    for (const QJsonValue &v : compat) {
        const QString c = v.toString().toLower();
        if (c.contains("sensirion") || c.contains("bosch") ||
            c.contains("sensor") || c.contains("temp") ||
            c.contains("humidity") || c.contains("pressure") ||
            c.contains("accel") || c.contains("invensense")) {
            return true;
        }
    }

    return false;
}

bool DevicePanelFactory::isSpiFlashDevice(const QString &type, const QJsonObject &config)
{
    const QString lower = type.toLower();
    if (lower.contains("flash") || lower.contains("spi.flash") ||
        lower.contains("eeprom") || lower.contains("w25q") ||
        lower.contains("at25") || lower.contains("spi-nor")) {
        return true;
    }

    const QJsonArray compat = config.value("compatible").toArray();
    for (const QJsonValue &v : compat) {
        const QString c = v.toString().toLower();
        if (c.contains("spi-nor") || c.contains("flash") || c.contains("eeprom")) {
            return true;
        }
    }

    return false;
}

bool DevicePanelFactory::isUartDevice(const QString &type, const QJsonObject &config)
{
    const QString lower = type.toLower();
    if (lower.contains("uart") || lower.contains("serial") ||
        lower.contains("loopback") || lower.contains("gps") ||
        lower.contains("modem") || lower.contains("gsm") ||
        lower.contains("lora") || lower.contains("rs485")) {
        return true;
    }

    const QJsonObject bus = config.value("bus").toObject();
    if (bus.value("kind").toString().toLower() == "uart") {
        return true;
    }

    return false;
}
