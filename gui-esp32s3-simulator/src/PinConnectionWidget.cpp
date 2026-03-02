#include "PinConnectionWidget.h"

#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QVBoxLayout>

PinConnectionWidget::PinConnectionWidget(const QJsonObject &rawConfig,
                                         QWidget *parent)
    : QWidget(parent), m_config(rawConfig)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_grid = new QGridLayout();
    m_grid->setSpacing(2);
    outer->addLayout(m_grid);

    rebuild();
}

void PinConnectionWidget::setConfig(const QJsonObject &rawConfig)
{
    m_config = rawConfig;
    rebuild();
}

QList<PinConnectionWidget::PinEntry>
PinConnectionWidget::extractPins(const QJsonObject &config) const
{
    QList<PinEntry> result;
    const QJsonObject bus = config.value("bus").toObject();
    const QString busKind = bus.value("kind").toString().toLower();
    const QJsonObject pins = bus.value("pins").toObject();

    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        PinEntry entry;
        entry.signalName = it.key().toUpper();
        entry.gpioNum = it.value().toInt(-1);
        entry.direction = pinDirectionForSignal(busKind, it.key().toLower());
        result.append(entry);
    }

    // Also check interrupt and reset GPIOs
    if (config.contains("interrupts")) {
        const QJsonArray irqs = config.value("interrupts").toArray();
        for (int i = 0; i < irqs.size(); ++i) {
            PinEntry entry;
            entry.signalName = QString("IRQ%1").arg(i);
            entry.gpioNum = irqs.at(i).toInt(-1);
            entry.direction = "in";
            result.append(entry);
        }
    }

    if (config.contains("reset-gpios")) {
        const QJsonArray resets = config.value("reset-gpios").toArray();
        for (int i = 0; i < resets.size(); ++i) {
            PinEntry entry;
            entry.signalName = "RST";
            entry.gpioNum = resets.at(i).toInt(-1);
            entry.direction = "out";
            result.append(entry);
        }
    }

    return result;
}

QString PinConnectionWidget::busKindLabel(const QString &kind) const
{
    if (kind == "i2c") return "I\u00B2C";
    if (kind == "spi") return "SPI";
    if (kind == "uart") return "UART";
    if (kind == "gpio") return "GPIO";
    return kind.toUpper();
}

QString PinConnectionWidget::pinDirectionForSignal(const QString &busKind,
                                                    const QString &signal) const
{
    // I2C: SDA is bidirectional, SCL is output from master
    if (busKind == "i2c") {
        if (signal == "sda") return "bidir";
        if (signal == "scl") return "out";
    }
    // SPI: MOSI out, MISO in, SCLK out, CS out
    if (busKind == "spi") {
        if (signal == "mosi") return "out";
        if (signal == "miso") return "in";
        if (signal == "sclk") return "out";
        if (signal == "cs") return "out";
    }
    // UART: TX out, RX in
    if (busKind == "uart") {
        if (signal == "tx") return "out";
        if (signal == "rx") return "in";
    }
    return "bidir";
}

void PinConnectionWidget::rebuild()
{
    // Clear old items
    while (m_grid->count() > 0) {
        QLayoutItem *item = m_grid->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const QList<PinEntry> pins = extractPins(m_config);
    if (pins.isEmpty()) {
        auto *noPin = new QLabel("<i>No pin connections defined</i>", this);
        noPin->setStyleSheet("color: gray;");
        m_grid->addWidget(noPin, 0, 0, 1, 5);
        return;
    }

    const QJsonObject bus = m_config.value("bus").toObject();
    const QString busKind = bus.value("kind").toString().toLower();
    const QString controller = bus.value("controller").toString();
    const QString address = bus.value("address").toString(
        m_config.value("reg").toVariant().toString());

    // Header label
    QString headerText = QString("<b>%1</b> bus <b>%2</b>")
        .arg(busKindLabel(busKind), controller.toUpper());
    if (!address.isEmpty()) {
        headerText += QString("  addr: <b>%1</b>").arg(address);
    }
    auto *headerLabel = new QLabel(headerText, this);
    headerLabel->setStyleSheet(
        "background: #2d2d3d; color: #a0c0ff; padding: 3px 6px; "
        "border-radius: 3px; font-size: 11px;");
    m_grid->addWidget(headerLabel, 0, 0, 1, 5);

    // Column headers
    auto makeHeaderLabel = [this](const QString &text) {
        auto *lbl = new QLabel(text, this);
        lbl->setStyleSheet("font-weight: bold; font-size: 10px; color: #888;");
        lbl->setAlignment(Qt::AlignCenter);
        return lbl;
    };

    m_grid->addWidget(makeHeaderLabel("ESP32-S3"), 1, 0);
    m_grid->addWidget(makeHeaderLabel("GPIO"), 1, 1);
    m_grid->addWidget(makeHeaderLabel(""), 1, 2);
    m_grid->addWidget(makeHeaderLabel("Signal"), 1, 3);
    m_grid->addWidget(makeHeaderLabel("Device"), 1, 4);

    const QString deviceId = m_config.value("id").toString();
    const QString deviceType = m_config.value("type").toString();

    for (int i = 0; i < pins.size(); ++i) {
        const PinEntry &pin = pins.at(i);
        const int row = i + 2;

        // ESP32-S3 chip side
        auto *chipLabel = new QLabel("ESP32-S3", this);
        chipLabel->setAlignment(Qt::AlignCenter);
        chipLabel->setStyleSheet(
            "background: #1a3a1a; color: #80ff80; padding: 2px 4px; "
            "border: 1px solid #408040; border-radius: 2px; font-size: 10px;");
        m_grid->addWidget(chipLabel, row, 0);

        // GPIO number
        QString gpioText = (pin.gpioNum >= 0)
            ? QString("GPIO%1").arg(pin.gpioNum) : "N/A";
        auto *gpioLabel = new QLabel(gpioText, this);
        gpioLabel->setAlignment(Qt::AlignCenter);
        gpioLabel->setStyleSheet(
            "background: #2a2a2a; color: #ffcc00; padding: 2px 6px; "
            "border: 1px solid #555; border-radius: 2px; font-family: monospace; font-size: 11px;");
        m_grid->addWidget(gpioLabel, row, 1);

        // Direction arrow
        QString arrow;
        if (pin.direction == "out") arrow = "\u2192";        // →
        else if (pin.direction == "in") arrow = "\u2190";     // ←
        else arrow = "\u2194";                                // ↔
        auto *arrowLabel = new QLabel(arrow, this);
        arrowLabel->setAlignment(Qt::AlignCenter);
        arrowLabel->setStyleSheet("font-size: 14px; color: #80c0ff;");
        m_grid->addWidget(arrowLabel, row, 2);

        // Signal name
        auto *signalLabel = new QLabel(pin.signalName, this);
        signalLabel->setAlignment(Qt::AlignCenter);
        signalLabel->setStyleSheet(
            "background: #2a2a3a; color: #ff9060; padding: 2px 6px; "
            "border: 1px solid #555; border-radius: 2px; font-weight: bold; font-size: 11px;");
        m_grid->addWidget(signalLabel, row, 3);

        // Device side
        auto *devLabel = new QLabel(deviceType.toUpper(), this);
        devLabel->setAlignment(Qt::AlignCenter);
        devLabel->setStyleSheet(
            "background: #1a1a3a; color: #a0a0ff; padding: 2px 4px; "
            "border: 1px solid #4040a0; border-radius: 2px; font-size: 10px;");
        m_grid->addWidget(devLabel, row, 4);
    }

    // Electrical info
    const QJsonObject electrical = bus.value("electrical").toObject();
    if (!electrical.isEmpty()) {
        const int elecRow = pins.size() + 2;
        QString elecText;
        if (electrical.contains("voltage")) {
            elecText += QString("V<sub>IO</sub>: %1V").arg(electrical.value("voltage").toDouble());
        }
        if (electrical.contains("pullup_ohms")) {
            if (!elecText.isEmpty()) elecText += "  ";
            double kohms = electrical.value("pullup_ohms").toInt() / 1000.0;
            elecText += QString("Pull-up: %1k\u03A9").arg(kohms, 0, 'f', 1);
        }
        if (!elecText.isEmpty()) {
            auto *elecLabel = new QLabel(elecText, this);
            elecLabel->setStyleSheet("color: #888; font-size: 10px; padding-left: 4px;");
            m_grid->addWidget(elecLabel, elecRow, 0, 1, 5);
        }
    }

    // Bus speed info
    const int speedRow = pins.size() + 3;
    QString speedText;
    if (bus.contains("clock_hz")) {
        int hz = bus.value("clock_hz").toInt();
        if (hz >= 1000000) speedText = QString("Clock: %1 MHz").arg(hz / 1000000.0, 0, 'f', 1);
        else if (hz >= 1000) speedText = QString("Clock: %1 kHz").arg(hz / 1000.0, 0, 'f', 0);
        else speedText = QString("Clock: %1 Hz").arg(hz);
    }
    if (bus.contains("baud")) {
        speedText = QString("Baud: %1").arg(bus.value("baud").toInt());
    }
    if (bus.contains("spi_mode")) {
        if (!speedText.isEmpty()) speedText += "  ";
        speedText += QString("Mode: %1").arg(bus.value("spi_mode").toString().toUpper());
    }
    if (!speedText.isEmpty()) {
        auto *speedLabel = new QLabel(speedText, this);
        speedLabel->setStyleSheet("color: #888; font-size: 10px; padding-left: 4px;");
        m_grid->addWidget(speedLabel, speedRow, 0, 1, 5);
    }
}
