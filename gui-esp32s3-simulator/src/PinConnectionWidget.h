#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QGridLayout;

/// Displays the pin connections between an ESP32-S3 GPIO and a peripheral
/// device.  Parses the "bus.pins" section of the device JSON config and
/// renders a compact wiring diagram with pin numbers and signal names.
class PinConnectionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PinConnectionWidget(const QJsonObject &rawConfig,
                                 QWidget *parent = nullptr);

    /// Rebuild the pin display from new config data.
    void setConfig(const QJsonObject &rawConfig);

private:
    struct PinEntry {
        QString signalName;   // e.g. "SCL", "MOSI", "TX"
        int gpioNum;          // ESP32-S3 GPIO number
        QString direction;    // "bidir", "out", "in"
    };

    void rebuild();
    QList<PinEntry> extractPins(const QJsonObject &config) const;
    QString busKindLabel(const QString &kind) const;
    QString pinDirectionForSignal(const QString &busKind, const QString &signal) const;

    QJsonObject m_config;
    QGridLayout *m_grid = nullptr;
};
