#include "SpiFlashPanel.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QJsonArray>

SpiFlashPanel::SpiFlashPanel(const QString &deviceId,
                             const QString &deviceType,
                             const QJsonObject &rawConfig,
                             QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    // Get size from simulator args
    const QJsonObject sim = rawConfig.value("simulator").toObject();
    const QJsonArray args = sim.value("args").toArray();
    for (int i = 0; i + 1 < args.size(); i += 2) {
        if (args.at(i).toString() == "--size")
            m_totalSize = args.at(i + 1).toString().toInt();
    }
    buildUI();
}

void SpiFlashPanel::buildUI()
{
    auto *layout = contentLayout();

    // Info bar
    auto *infoGroup = new QGroupBox("Flash Info", this);
    auto *infoLayout = new QHBoxLayout(infoGroup);

    m_sizeLabel = new QLabel(this);
    if (m_totalSize > 0) {
        double kb = m_totalSize / 1024.0;
        if (kb >= 1024) {
            m_sizeLabel->setText(QString("Size: %1 MB").arg(kb / 1024.0, 0, 'f', 1));
        } else {
            m_sizeLabel->setText(QString("Size: %1 KB").arg(kb, 0, 'f', 0));
        }
    } else {
        m_sizeLabel->setText("Size: unknown");
    }
    m_sizeLabel->setStyleSheet("font-weight: bold;");
    infoLayout->addWidget(m_sizeLabel);

    m_statsLabel = new QLabel("Reads: 0  Writes: 0  Erases: 0", this);
    m_statsLabel->setStyleSheet("color: #888;");
    infoLayout->addWidget(m_statsLabel, 1);

    layout->addWidget(infoGroup);

    // Hex view
    auto *hexGroup = new QGroupBox("Memory Contents", this);
    auto *hexLayout = new QVBoxLayout(hexGroup);

    auto *addrLayout = new QHBoxLayout();
    addrLayout->addWidget(new QLabel("Base Address:", this));
    m_addressBox = new QSpinBox(this);
    m_addressBox->setRange(0, qMax(0, m_totalSize - 256));
    m_addressBox->setPrefix("0x");
    m_addressBox->setDisplayIntegerBase(16);
    m_addressBox->setSingleStep(256);
    addrLayout->addWidget(m_addressBox);
    addrLayout->addStretch();
    hexLayout->addLayout(addrLayout);

    m_hexView = new QTextEdit(this);
    m_hexView->setReadOnly(true);
    m_hexView->setFont(QFont("Courier New", 10));
    m_hexView->setStyleSheet(
        "background: #0a0a0a; color: #80ff80; border: 1px solid #333;");
    m_hexView->setPlaceholderText("Flash contents will appear here after read operations...");
    hexLayout->addWidget(m_hexView, 1);

    layout->addWidget(hexGroup, 1);
}

void SpiFlashPanel::updateState(const QJsonObject &state)
{
    if (state.contains("stats")) {
        const QJsonObject stats = state.value("stats").toObject();
        m_statsLabel->setText(QString("Reads: %1  Writes: %2  Erases: %3")
            .arg(stats.value("reads").toInt())
            .arg(stats.value("writes").toInt())
            .arg(stats.value("erases").toInt()));
    }
}

void SpiFlashPanel::updateCapabilities(const QJsonObject &caps)
{
    const QJsonObject device = caps.value("device").toObject();
    if (device.contains("size")) {
        m_totalSize = device.value("size").toInt();
        double kb = m_totalSize / 1024.0;
        if (kb >= 1024) {
            m_sizeLabel->setText(QString("Size: %1 MB").arg(kb / 1024.0, 0, 'f', 1));
        } else {
            m_sizeLabel->setText(QString("Size: %1 KB").arg(kb, 0, 'f', 0));
        }
        m_addressBox->setRange(0, qMax(0, m_totalSize - 256));
    }
}
