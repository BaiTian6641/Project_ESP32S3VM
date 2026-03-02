#include "DevicePanelBase.h"
#include "PinConnectionWidget.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>

DevicePanelBase::DevicePanelBase(const QString &deviceId,
                                 const QString &deviceType,
                                 const QJsonObject &rawConfig,
                                 QWidget *parent)
    : QWidget(parent),
      m_deviceId(deviceId),
      m_deviceType(deviceType),
      m_rawConfig(rawConfig)
{
    buildBaseLayout();
}

void DevicePanelBase::buildBaseLayout()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    // --- Header row: title + status ---
    auto *headerLayout = new QHBoxLayout();
    m_titleLabel = new QLabel(
        QString("<b>%1</b>  <i>(%2)</i>").arg(m_deviceId, m_deviceType), this);
    m_statusLabel = new QLabel("loaded", this);
    m_statusLabel->setStyleSheet("color: gray; font-weight: bold;");
    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_statusLabel);
    root->addLayout(headerLayout);

    // --- Pin connection visualization ---
    m_pinWidget = new PinConnectionWidget(m_rawConfig, this);
    root->addWidget(m_pinWidget);

    // --- Device-specific content area ---
    m_contentGroup = new QGroupBox("Device View", this);
    m_contentLayout = new QVBoxLayout(m_contentGroup);
    root->addWidget(m_contentGroup, 1);

    // --- Log area ---
    m_logGroup = new QGroupBox("Device Log", this);
    auto *logLayout = new QVBoxLayout(m_logGroup);
    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(120);
    m_logText->setPlaceholderText("Device log output...");
    logLayout->addWidget(m_logText);
    root->addWidget(m_logGroup);
}

void DevicePanelBase::updateState(const QJsonObject &state)
{
    Q_UNUSED(state);
}

void DevicePanelBase::updateCapabilities(const QJsonObject &caps)
{
    Q_UNUSED(caps);
}

void DevicePanelBase::updateStatus(const QString &status, const QString &lastError)
{
    QString color = "gray";
    if (status == "running") color = "green";
    else if (status == "error") color = "red";
    else if (status == "stopped") color = "orange";

    QString text = status;
    if (!lastError.isEmpty()) {
        text += " — " + lastError;
    }
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));
}

void DevicePanelBase::appendLog(const QString &line)
{
    if (m_logText) {
        m_logText->append(line);
    }
}
