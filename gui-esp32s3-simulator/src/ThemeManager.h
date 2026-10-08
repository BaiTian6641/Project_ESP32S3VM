#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>

class QApplication;

// Semantic Carbon Gray 10 / Gray 100 colors shared by widgets and scene items.
struct ThemeTokens {
    QColor background, layer, layerAlt, field;
    QColor textPrimary, textSecondary, textHelper, textDisabled, textOnColor;
    QColor borderSubtle, borderStrong, focus;
    QColor buttonPrimary, buttonPrimaryHover, buttonSecondary, hover, selected;
    QColor supportError, supportSuccess, supportWarning, supportInfo;
    QColor busI2c, busSpi, busUart, busGpio, canvasGrid;
    bool dark = false;
};

class ThemeManager final : public QObject {
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };
    Q_ENUM(Mode)

    static ThemeManager *instance();
    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    const ThemeTokens &tokens() const { return m_tokens; }
    void apply(QApplication *application);
    bool systemAppearanceSupported() const { return m_systemSupported; }
    QString appearanceReason() const { return m_appearanceReason; }

signals:
    void themeChanged();

private:
    ThemeManager();
    void detectSystemAppearance();
    void refresh();
    Mode m_mode = Mode::System;
    ThemeTokens m_tokens;
    QPointer<QApplication> m_application;
    bool m_systemDark = false;
    bool m_systemSupported = false;
    bool m_fallbackCaptured = false;
    QString m_appearanceReason;
};
