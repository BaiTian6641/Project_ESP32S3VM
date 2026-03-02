#include "SensorPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

SensorPanel::SensorPanel(const QString &deviceId,
                         const QString &deviceType,
                         const QJsonObject &rawConfig,
                         QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    buildUI();
}

void SensorPanel::buildUI()
{
    auto *layout = contentLayout();

    // === Telemetry display ===
    auto *telemetryGroup = new QGroupBox("Live Telemetry", this);
    auto *telemetryLayout = new QVBoxLayout(telemetryGroup);

    m_telemetryLabel = new QLabel(this);
    m_telemetryLabel->setAlignment(Qt::AlignCenter);
    m_telemetryLabel->setStyleSheet(
        "font-size: 22px; font-weight: bold; color: #40ff40; "
        "background: #0a0a0a; border: 1px solid #333; border-radius: 6px; "
        "padding: 12px; font-family: 'Courier New', monospace;");
    m_telemetryLabel->setText("--- Waiting for data ---");
    telemetryLayout->addWidget(m_telemetryLabel);

    layout->addWidget(telemetryGroup);

    // === Sensor controls group (populated dynamically from capabilities) ===
    // This will be rebuilt when capabilities arrive, but provide a placeholder
    auto *controlGroup = new QGroupBox("Sensor Controls", this);
    controlGroup->setObjectName("sensorControlGroup");
    auto *controlGrid = new QGridLayout(controlGroup);

    // Mode selector
    controlGrid->addWidget(new QLabel("Mode:", this), 0, 0);
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItems({"manual", "csv"});
    controlGrid->addWidget(m_modeCombo, 0, 1, 1, 2);
    connect(m_modeCombo, &QComboBox::currentTextChanged, this, [this](const QString &mode) {
        emit parameterChangeRequested(deviceId(), "mode", mode);
    });

    layout->addWidget(controlGroup);

    // === CSV Playback ===
    auto *csvGroup = new QGroupBox("CSV Playback", this);
    auto *csvLayout = new QHBoxLayout(csvGroup);

    m_startCsvButton = new QPushButton("Start", this);
    m_pauseCsvButton = new QPushButton("Pause", this);
    m_stopCsvButton = new QPushButton("Stop", this);
    m_playbackStatusLabel = new QLabel("Idle", this);
    m_playbackStatusLabel->setStyleSheet("color: gray;");
    m_playbackIndexLabel = new QLabel("", this);
    m_playbackIndexLabel->setStyleSheet("color: #888; font-size: 10px;");

    csvLayout->addWidget(m_startCsvButton);
    csvLayout->addWidget(m_pauseCsvButton);
    csvLayout->addWidget(m_stopCsvButton);
    csvLayout->addWidget(m_playbackStatusLabel, 1);
    csvLayout->addWidget(m_playbackIndexLabel);

    // CSV buttons emit RPCs via parameterChangeRequested with special naming
    connect(m_startCsvButton, &QPushButton::clicked, this, [this]() {
        // Use the CSV path from config properties
        const QJsonObject props = rawConfig().value("properties").toObject();
        const QString csvPath = props.value("csv_example").toString();
        QJsonObject params;
        params["path"] = csvPath;
        params["loop"] = true;
        params["speed"] = 1.0;
        emit parameterChangeRequested(deviceId(), "__rpc:start_csv_playback",
                                      QVariant::fromValue(params));
    });
    connect(m_pauseCsvButton, &QPushButton::clicked, this, [this]() {
        emit parameterChangeRequested(deviceId(), "__rpc:pause_csv_playback", QVariant());
    });
    connect(m_stopCsvButton, &QPushButton::clicked, this, [this]() {
        emit parameterChangeRequested(deviceId(), "__rpc:stop_csv_playback", QVariant());
    });

    layout->addWidget(csvGroup);
}

void SensorPanel::updateCapabilities(const QJsonObject &caps)
{
    const QJsonObject panel = caps.value("panel").toObject();
    const QJsonArray controls = panel.value("controls").toArray();

    // Find the sensor control group and rebuild it
    auto *controlGroup = findChild<QGroupBox *>("sensorControlGroup");
    if (!controlGroup) return;

    auto *grid = qobject_cast<QGridLayout *>(controlGroup->layout());
    if (!grid) return;

    // Clear old dynamic controls (keep row 0 = mode)
    m_controls.clear();

    int row = 1;
    for (const QJsonValue &v : controls) {
        const QJsonObject cObj = v.toObject();
        const QString name = cObj.value("name").toString();
        if (name == "mode") continue; // already handled

        SensorControl ctrl;
        ctrl.name = name;
        ctrl.type = cObj.value("type").toString("number");
        ctrl.min = cObj.value("min").toDouble(0);
        ctrl.max = cObj.value("max").toDouble(100);
        ctrl.writable = cObj.value("writable").toBool(false);

        if (cObj.contains("enum")) {
            const QJsonArray enumArr = cObj.value("enum").toArray();
            for (const QJsonValue &e : enumArr) {
                ctrl.enumValues.append(e.toString());
            }
        }

        buildControlRow(ctrl, row);
        m_controls.append(ctrl);
        row++;
    }
}

void SensorPanel::buildControlRow(SensorControl &ctrl, int row)
{
    auto *controlGroup = findChild<QGroupBox *>("sensorControlGroup");
    if (!controlGroup) return;
    auto *grid = qobject_cast<QGridLayout *>(controlGroup->layout());
    if (!grid) return;

    // Label
    QString displayName = ctrl.name;
    displayName.replace("_", " ");
    // Capitalize first letter
    if (!displayName.isEmpty())
        displayName[0] = displayName[0].toUpper();

    auto *nameLabel = new QLabel(displayName + ":", this);
    nameLabel->setStyleSheet("font-weight: bold;");
    grid->addWidget(nameLabel, row, 0);

    if (ctrl.type == "number") {
        // Value display
        ctrl.valueLabel = new QLabel("---", this);
        ctrl.valueLabel->setStyleSheet(
            "font-size: 16px; font-weight: bold; color: #ffcc00; "
            "font-family: monospace;");
        ctrl.valueLabel->setMinimumWidth(80);
        grid->addWidget(ctrl.valueLabel, row, 1);

        // Progress bar for visual range
        ctrl.bar = new QProgressBar(this);
        ctrl.bar->setRange(static_cast<int>(ctrl.min * 100),
                           static_cast<int>(ctrl.max * 100));
        ctrl.bar->setTextVisible(false);
        ctrl.bar->setMaximumHeight(12);
        grid->addWidget(ctrl.bar, row, 2);

        if (ctrl.writable) {
            ctrl.spinBox = new QDoubleSpinBox(this);
            ctrl.spinBox->setRange(ctrl.min, ctrl.max);
            ctrl.spinBox->setDecimals(2);
            ctrl.spinBox->setSingleStep(0.5);
            grid->addWidget(ctrl.spinBox, row, 3);

            const QString paramName = ctrl.name;
            connect(ctrl.spinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
                    this, [this, paramName](double val) {
                emit parameterChangeRequested(deviceId(), paramName, val);
            });
        }
    } else if (ctrl.type == "enum" || !ctrl.enumValues.isEmpty()) {
        ctrl.valueLabel = new QLabel("---", this);
        grid->addWidget(ctrl.valueLabel, row, 1);

        if (ctrl.writable) {
            ctrl.comboBox = new QComboBox(this);
            ctrl.comboBox->addItems(ctrl.enumValues);
            grid->addWidget(ctrl.comboBox, row, 2, 1, 2);

            const QString paramName = ctrl.name;
            connect(ctrl.comboBox, &QComboBox::currentTextChanged,
                    this, [this, paramName](const QString &val) {
                emit parameterChangeRequested(deviceId(), paramName, val);
            });
        }
    }
}

void SensorPanel::updateControlValue(SensorControl &ctrl, const QVariant &value)
{
    if (ctrl.type == "number") {
        double v = value.toDouble();
        if (ctrl.valueLabel) {
            QString unit;
            if (ctrl.name.contains("temperature") || ctrl.name.contains("temp"))
                unit = " \u00B0C";
            else if (ctrl.name.contains("humidity") || ctrl.name.contains("rh"))
                unit = " %RH";
            else if (ctrl.name.contains("pressure"))
                unit = " hPa";
            ctrl.valueLabel->setText(QString::number(v, 'f', 2) + unit);
        }
        if (ctrl.bar) {
            ctrl.bar->setValue(static_cast<int>(v * 100));
        }
    } else {
        if (ctrl.valueLabel) {
            ctrl.valueLabel->setText(value.toString());
        }
    }
}

void SensorPanel::updateState(const QJsonObject &state)
{
    // Update telemetry display
    QStringList lines;

    // Handle direct telemetry notification
    const QJsonObject telemetry = state.contains("telemetry")
        ? state.value("telemetry").toObject() : state;

    for (SensorControl &ctrl : m_controls) {
        if (telemetry.contains(ctrl.name)) {
            updateControlValue(ctrl, telemetry.value(ctrl.name).toVariant());
        } else if (state.contains(ctrl.name)) {
            updateControlValue(ctrl, state.value(ctrl.name).toVariant());
        }
    }

    // Build telemetry text
    if (telemetry.contains("temperature_c") || state.contains("temperature_c")) {
        double t = telemetry.contains("temperature_c")
            ? telemetry.value("temperature_c").toDouble()
            : state.value("temperature_c").toDouble();
        lines.append(QString("\U0001F321 %1 \u00B0C").arg(t, 0, 'f', 2));
    }
    if (telemetry.contains("humidity_rh") || state.contains("humidity_rh")) {
        double h = telemetry.contains("humidity_rh")
            ? telemetry.value("humidity_rh").toDouble()
            : state.value("humidity_rh").toDouble();
        lines.append(QString("\U0001F4A7 %1 %%RH").arg(h, 0, 'f', 2));
    }

    if (!lines.isEmpty()) {
        m_telemetryLabel->setText(lines.join("    "));
    }

    // Mode
    if (state.contains("mode")) {
        m_modeCombo->blockSignals(true);
        m_modeCombo->setCurrentText(state.value("mode").toString());
        m_modeCombo->blockSignals(false);
    }

    // Playback state
    if (state.contains("playback")) {
        const QJsonObject pb = state.value("playback").toObject();
        bool running = pb.value("running").toBool();
        int idx = pb.value("index").toInt();
        int rows = pb.value("rows").toInt();

        m_playbackStatusLabel->setText(running ? "Playing" : "Stopped");
        m_playbackStatusLabel->setStyleSheet(
            running ? "color: #40ff40; font-weight: bold;" : "color: gray;");
        m_playbackIndexLabel->setText(
            rows > 0 ? QString("%1 / %2").arg(idx).arg(rows) : "");
    }
}
