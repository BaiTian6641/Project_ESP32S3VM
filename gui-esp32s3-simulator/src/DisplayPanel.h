#pragma once

#include "DevicePanelBase.h"

#include <QImage>
#include <QJsonObject>

class QLabel;
class QCheckBox;
class QSlider;
class QSpinBox;

/// Panel for OLED/LCD display devices (e.g. SSD1306).  Renders the
/// framebuffer content received from the device simulator as a live screen
/// preview.  Also shows display controls (on/off, contrast, invert).
class DisplayPanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit DisplayPanel(const QString &deviceId,
                          const QString &deviceType,
                          const QJsonObject &rawConfig,
                          QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    void buildUI();
    void renderFramebuffer(const QJsonObject &bufferObj);
    void renderPageMajorMono(const QList<int> &data, int w, int h);
    QImage scaleForDisplay(const QImage &source) const;

    // Display screen
    QLabel *m_screenLabel = nullptr;
    QImage m_screenImage;

    // Controls
    QCheckBox *m_displayOnCheck = nullptr;
    QCheckBox *m_invertedCheck = nullptr;
    QSlider *m_contrastSlider = nullptr;
    QLabel *m_contrastValueLabel = nullptr;
    QLabel *m_geometryLabel = nullptr;
    QLabel *m_cursorLabel = nullptr;

    int m_width = 128;
    int m_height = 64;
    int m_scaleFactor = 3;
};
