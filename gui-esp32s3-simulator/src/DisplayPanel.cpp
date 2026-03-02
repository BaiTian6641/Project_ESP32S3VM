#include "DisplayPanel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

DisplayPanel::DisplayPanel(const QString &deviceId,
                           const QString &deviceType,
                           const QJsonObject &rawConfig,
                           QWidget *parent)
    : DevicePanelBase(deviceId, deviceType, rawConfig, parent)
{
    // Pre-read geometry from config properties
    const QJsonObject props = rawConfig.value("properties").toObject();
    const QJsonObject sim = rawConfig.value("simulator").toObject();
    const QJsonArray args = sim.value("args").toArray();
    for (int i = 0; i + 1 < args.size(); i += 2) {
        if (args.at(i).toString() == "--width")
            m_width = args.at(i + 1).toString().toInt();
        if (args.at(i).toString() == "--height")
            m_height = args.at(i + 1).toString().toInt();
    }

    buildUI();
}

void DisplayPanel::buildUI()
{
    auto *layout = contentLayout();

    // === Screen display area ===
    auto *screenGroup = new QGroupBox("Screen Output", this);
    auto *screenLayout = new QVBoxLayout(screenGroup);

    m_screenLabel = new QLabel(this);
    m_screenLabel->setAlignment(Qt::AlignCenter);
    m_screenLabel->setMinimumSize(m_width * m_scaleFactor + 8,
                                  m_height * m_scaleFactor + 8);
    m_screenLabel->setStyleSheet(
        "background: #000000; border: 2px solid #333; border-radius: 4px; padding: 4px;");

    // Initialize with blank screen
    m_screenImage = QImage(m_width, m_height, QImage::Format_Mono);
    m_screenImage.fill(0);
    m_screenLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(m_screenImage)));

    screenLayout->addWidget(m_screenLabel, 0, Qt::AlignCenter);

    m_geometryLabel = new QLabel(
        QString("Resolution: %1 x %2  |  Format: Monochrome 1bpp")
            .arg(m_width).arg(m_height), this);
    m_geometryLabel->setStyleSheet("color: #888; font-size: 10px;");
    m_geometryLabel->setAlignment(Qt::AlignCenter);
    screenLayout->addWidget(m_geometryLabel);

    m_cursorLabel = new QLabel("Cursor: page=0  col=0", this);
    m_cursorLabel->setStyleSheet("color: #666; font-size: 10px;");
    m_cursorLabel->setAlignment(Qt::AlignCenter);
    screenLayout->addWidget(m_cursorLabel);

    layout->addWidget(screenGroup);

    // === Controls ===
    auto *controlGroup = new QGroupBox("Display Controls", this);
    auto *controlGrid = new QGridLayout(controlGroup);

    m_displayOnCheck = new QCheckBox("Display ON", this);
    controlGrid->addWidget(m_displayOnCheck, 0, 0);
    connect(m_displayOnCheck, &QCheckBox::toggled, this, [this](bool checked) {
        emit parameterChangeRequested(deviceId(), "display_on", checked);
    });

    m_invertedCheck = new QCheckBox("Inverted", this);
    controlGrid->addWidget(m_invertedCheck, 0, 1);
    connect(m_invertedCheck, &QCheckBox::toggled, this, [this](bool checked) {
        emit parameterChangeRequested(deviceId(), "inverted", checked);
    });

    controlGrid->addWidget(new QLabel("Contrast:", this), 1, 0);
    auto *contrastLayout = new QHBoxLayout();
    m_contrastSlider = new QSlider(Qt::Horizontal, this);
    m_contrastSlider->setRange(0, 255);
    m_contrastSlider->setValue(127);
    m_contrastValueLabel = new QLabel("127", this);
    m_contrastValueLabel->setMinimumWidth(30);
    contrastLayout->addWidget(m_contrastSlider, 1);
    contrastLayout->addWidget(m_contrastValueLabel);
    controlGrid->addLayout(contrastLayout, 1, 1);

    connect(m_contrastSlider, &QSlider::valueChanged, this, [this](int value) {
        m_contrastValueLabel->setText(QString::number(value));
    });
    connect(m_contrastSlider, &QSlider::sliderReleased, this, [this]() {
        emit parameterChangeRequested(deviceId(), "contrast", m_contrastSlider->value());
    });

    // Scale factor
    controlGrid->addWidget(new QLabel("Zoom:", this), 2, 0);
    auto *scaleBox = new QSpinBox(this);
    scaleBox->setRange(1, 8);
    scaleBox->setValue(m_scaleFactor);
    scaleBox->setSuffix("x");
    controlGrid->addWidget(scaleBox, 2, 1);
    connect(scaleBox, qOverload<int>(&QSpinBox::valueChanged), this, [this](int val) {
        m_scaleFactor = val;
        m_screenLabel->setMinimumSize(m_width * m_scaleFactor + 8,
                                      m_height * m_scaleFactor + 8);
        if (!m_screenImage.isNull()) {
            m_screenLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(m_screenImage)));
        }
    });

    layout->addWidget(controlGroup);
}

void DisplayPanel::updateState(const QJsonObject &state)
{
    // Update controls
    if (state.contains("display_on")) {
        m_displayOnCheck->blockSignals(true);
        m_displayOnCheck->setChecked(state.value("display_on").toBool());
        m_displayOnCheck->blockSignals(false);
    }
    if (state.contains("inverted")) {
        m_invertedCheck->blockSignals(true);
        m_invertedCheck->setChecked(state.value("inverted").toBool());
        m_invertedCheck->blockSignals(false);
    }
    if (state.contains("contrast")) {
        int c = state.value("contrast").toInt();
        m_contrastSlider->blockSignals(true);
        m_contrastSlider->setValue(c);
        m_contrastSlider->blockSignals(false);
        m_contrastValueLabel->setText(QString::number(c));
    }
    if (state.contains("cursor")) {
        const QJsonObject cursor = state.value("cursor").toObject();
        m_cursorLabel->setText(QString("Cursor: page=%1  col=%2")
            .arg(cursor.value("page").toInt())
            .arg(cursor.value("column").toInt()));
    }
    if (state.contains("geometry")) {
        const QJsonObject geo = state.value("geometry").toObject();
        m_width = geo.value("width").toInt(m_width);
        m_height = geo.value("height").toInt(m_height);
        m_geometryLabel->setText(
            QString("Resolution: %1 x %2  |  Format: Monochrome 1bpp")
                .arg(m_width).arg(m_height));
    }

    // Render framebuffer
    if (state.contains("buffer")) {
        renderFramebuffer(state.value("buffer").toObject());
    }

    // Handle frame_update notification (from live streaming)
    if (state.contains("frame_update")) {
        renderFramebuffer(state.value("frame_update").toObject());
    }
}

void DisplayPanel::updateCapabilities(const QJsonObject &caps)
{
    const QJsonObject panel = caps.value("panel").toObject();
    if (panel.contains("width")) m_width = panel.value("width").toInt(m_width);
    if (panel.contains("height")) m_height = panel.value("height").toInt(m_height);
    m_geometryLabel->setText(
        QString("Resolution: %1 x %2  |  Format: %3")
            .arg(m_width).arg(m_height)
            .arg(panel.value("pixel_format").toString("mono1")));
}

void DisplayPanel::renderFramebuffer(const QJsonObject &bufferObj)
{
    const QString encoding = bufferObj.value("encoding").toString("u8");
    const QString layout = bufferObj.value("layout").toString("page-major");
    const QJsonArray dataArr = bufferObj.value("data").toArray();

    if (dataArr.isEmpty()) return;

    QList<int> data;
    data.reserve(dataArr.size());
    for (const QJsonValue &v : dataArr) {
        data.append(v.toInt());
    }

    if (layout == "page-major" && encoding == "u8") {
        renderPageMajorMono(data, m_width, m_height);
    }
}

void DisplayPanel::renderPageMajorMono(const QList<int> &data, int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(0, 0, 0));

    const int pages = h / 8;
    const bool inverted = m_invertedCheck->isChecked();
    const int contrast = m_contrastSlider->value();

    // Scale pixel brightness by contrast
    int bright = qBound(40, contrast, 255);
    QColor onColor;
    if (inverted) {
        onColor = QColor(0, 0, 0);
    } else {
        // OLED-like blue-white glow
        onColor = QColor(bright, bright, qBound(0, bright + 30, 255));
    }
    QColor offColor = inverted ? QColor(bright, bright, bright) : QColor(0, 0, 0);

    for (int page = 0; page < pages && page * w < data.size(); ++page) {
        for (int col = 0; col < w && page * w + col < data.size(); ++col) {
            int byte = data.at(page * w + col);
            for (int bit = 0; bit < 8; ++bit) {
                int y = page * 8 + bit;
                if (y >= h) break;
                bool pixelOn = (byte >> bit) & 1;
                img.setPixelColor(col, y, pixelOn ? onColor : offColor);
            }
        }
    }

    // If display is off, dim the whole image
    if (!m_displayOnCheck->isChecked()) {
        img.fill(QColor(5, 5, 8));
    }

    m_screenImage = img;
    m_screenLabel->setPixmap(QPixmap::fromImage(scaleForDisplay(img)));
}

QImage DisplayPanel::scaleForDisplay(const QImage &source) const
{
    return source.scaled(source.width() * m_scaleFactor,
                         source.height() * m_scaleFactor,
                         Qt::KeepAspectRatio,
                         Qt::FastTransformation);
}
