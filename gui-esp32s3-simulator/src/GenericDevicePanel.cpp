#include "GenericDevicePanel.h"

#include <QGroupBox>
#include <QJsonDocument>
#include <QTextEdit>
#include <QVBoxLayout>

GenericDevicePanel::GenericDevicePanel(const QString &deviceId,
                                      const QString &deviceType,
                                      const QJsonObject &rawConfig,
                                      QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    buildUI();
}

void GenericDevicePanel::buildUI()
{
    auto *layout = contentLayout();

    // Capabilities view
    auto *capsGroup = new QGroupBox("Capabilities", this);
    auto *capsLayout = new QVBoxLayout(capsGroup);
    m_capsView = new QTextEdit(this);
    m_capsView->setReadOnly(true);
    m_capsView->setFont(QFont("Courier New", 10));
    m_capsView->setMaximumHeight(150);
    m_capsView->setPlaceholderText("Waiting for capabilities...");
    capsLayout->addWidget(m_capsView);
    layout->addWidget(capsGroup);

    // State view
    auto *stateGroup = new QGroupBox("Device State", this);
    auto *stateLayout = new QVBoxLayout(stateGroup);
    m_stateView = new QTextEdit(this);
    m_stateView->setReadOnly(true);
    m_stateView->setFont(QFont("Courier New", 10));
    m_stateView->setStyleSheet(
        "background: #0a0a0a; color: #c0c0c0; border: 1px solid #333;");
    m_stateView->setPlaceholderText("Waiting for state...");
    stateLayout->addWidget(m_stateView);
    layout->addWidget(stateGroup, 1);
}

void GenericDevicePanel::updateState(const QJsonObject &state)
{
    m_stateView->setPlainText(
        QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Indented)));
}

void GenericDevicePanel::updateCapabilities(const QJsonObject &caps)
{
    m_capsView->setPlainText(
        QString::fromUtf8(QJsonDocument(caps).toJson(QJsonDocument::Indented)));
}
