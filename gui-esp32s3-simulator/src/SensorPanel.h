#pragma once

#include "DevicePanelBase.h"

#include <QJsonObject>
#include <QList>

class QLabel;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QProgressBar;

/// Panel for scalar sensor devices (e.g. SHT21 temperature/humidity).
/// Shows real-time telemetry values, allows manual injection of values,
/// and supports CSV playback controls.
class SensorPanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit SensorPanel(const QString &deviceId,
                         const QString &deviceType,
                         const QJsonObject &rawConfig,
                         QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    struct SensorControl {
        QString name;
        QString type;       // "number", "bool", "enum"
        double min = 0;
        double max = 100;
        QStringList enumValues;
        bool writable = false;

        QLabel *valueLabel = nullptr;
        QDoubleSpinBox *spinBox = nullptr;
        QProgressBar *bar = nullptr;
        QComboBox *comboBox = nullptr;
    };

    void buildUI();
    void buildControlRow(SensorControl &ctrl, int row);
    void updateControlValue(SensorControl &ctrl, const QVariant &value);

    QList<SensorControl> m_controls;

    // Telemetry display
    QLabel *m_telemetryLabel = nullptr;

    // CSV playback
    QLabel *m_playbackStatusLabel = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QPushButton *m_startCsvButton = nullptr;
    QPushButton *m_pauseCsvButton = nullptr;
    QPushButton *m_stopCsvButton = nullptr;
    QLabel *m_playbackIndexLabel = nullptr;
};
