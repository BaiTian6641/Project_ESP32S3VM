#pragma once

#include "DevicePanelBase.h"

class QLabel;
class QTextEdit;
class QSpinBox;

/// Panel for SPI flash/memory devices. Shows a hex viewer of the
/// memory contents and allows read/write operations.
class SpiFlashPanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit SpiFlashPanel(const QString &deviceId,
                           const QString &deviceType,
                           const QJsonObject &rawConfig,
                           QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    void buildUI();

    QLabel *m_sizeLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    QTextEdit *m_hexView = nullptr;
    QSpinBox *m_addressBox = nullptr;
    int m_totalSize = 0;
};
