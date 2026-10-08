#include "ThemeManager.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>
#include <QMap>
#include <algorithm>

namespace {
ThemeTokens colors(bool dark)
{
    ThemeTokens t;
    t.dark = dark;
    t.background = QColor(dark ? "#161616" : "#f4f4f4");
    t.layer = QColor(dark ? "#262626" : "#ffffff");
    t.layerAlt = QColor(dark ? "#393939" : "#e0e0e0");
    t.field = t.layer;
    t.textPrimary = QColor(dark ? "#f4f4f4" : "#161616");
    t.textSecondary = QColor(dark ? "#c6c6c6" : "#525252");
    t.textHelper = QColor(dark ? "#a8a8a8" : "#6f6f6f");
    t.textDisabled = QColor(dark ? "#6f6f6f" : "#8d8d8d");
    t.textOnColor = QColor("#ffffff");
    t.borderSubtle = QColor(dark ? "#393939" : "#c6c6c6");
    t.borderStrong = QColor(dark ? "#8d8d8d" : "#8d8d8d");
    t.focus = QColor(dark ? "#ffffff" : "#0f62fe");
    t.buttonPrimary = QColor("#0f62fe");
    t.buttonPrimaryHover = QColor("#0050e6");
    t.buttonSecondary = QColor(dark ? "#393939" : "#e0e0e0");
    t.hover = QColor(dark ? "#474747" : "#e8e8e8");
    t.selected = QColor(dark ? "#525252" : "#d0e2ff");
    t.supportError = QColor(dark ? "#ff8389" : "#da1e28");
    t.supportSuccess = QColor(dark ? "#42be65" : "#198038");
    t.supportWarning = QColor(dark ? "#f1c21b" : "#8e6a00");
    t.supportInfo = QColor(dark ? "#78a9ff" : "#0043ce");
    t.busI2c = QColor(dark ? "#3ddbd9" : "#007d79");
    t.busSpi = QColor(dark ? "#be95ff" : "#6929c4");
    t.busUart = t.supportInfo;
    t.busGpio = t.supportSuccess;
    t.canvasGrid = t.borderSubtle;
    return t;
}

QString styleSheet(const ThemeTokens &t)
{
    QString result = QStringLiteral(R"(
        QWidget { background: @background; color: @textPrimary; }
        QLabel { background: transparent; }
        QLabel#shellHeader { background: @layer; border-bottom: 1px solid @borderSubtle; font-size: 16px; }
        QLabel#pageTitle { font-size: 28px; font-weight: 400; padding-bottom: 8px; }
        QLabel#notice { background: @layer; border-left: 3px solid @supportInfo; padding: 12px; color: @textSecondary; }
        QLabel[tone="secondary"] { color: @textSecondary; }
        QLabel[tone="helper"] { color: @textHelper; }
        QLabel[tone="success"] { color: @supportSuccess; }
        QLabel[tone="warning"] { color: @supportWarning; }
        QLabel[tone="error"] { color: @supportError; }
        QLabel[tone="info"] { color: @supportInfo; }
        QLabel[role="metric"] { background: @layer; border: 1px solid @borderSubtle; padding: 12px; font-size: 22px; font-weight: 600; }
        QLabel[role="metricValue"] { color: @supportInfo; font-size: 16px; font-weight: 600; }
        QLabel[role="pinHeader"] { background: @layerAlt; color: @textPrimary; padding: 6px; font-weight: 600; }
        QLabel[role="pinBadge"] { background: @layer; border: 1px solid @borderStrong; padding: 4px 6px; font-size: 11px; }
        QLabel[role="display"] { background: @layer; border: 2px solid @borderStrong; padding: 4px; }
        QPushButton, QToolButton { background: @buttonSecondary; color: @textPrimary; border: 2px solid transparent; padding: 10px 14px; min-height: 16px; }
        QPushButton:hover, QToolButton:hover { background: @hover; }
        QPushButton:pressed, QToolButton:pressed { background: @selected; }
        QPushButton:focus, QToolButton:focus { border-color: @focus; }
        QPushButton#primary { background: @buttonPrimary; color: @textOnColor; }
        QPushButton#primary:hover { background: @buttonPrimaryHover; }
        QPushButton#primary:pressed { background: #002d9c; }
        QPushButton#primary:focus { border-color: @textOnColor; }
        QPushButton:disabled, QToolButton:disabled { background: @layerAlt; color: @textDisabled; }
        QToolButton:checked, QPushButton:checked { background: @selected; }
        QPushButton#primary:disabled, QToolButton:checked:disabled, QPushButton:checked:disabled { background: @layerAlt; color: @textDisabled; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: @field; border: 2px solid transparent; border-bottom-color: @borderStrong; padding: 6px; min-height: 20px; selection-background-color: @selected; selection-color: @textPrimary; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: @focus; }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: @textDisabled; background: @layerAlt; }
        QComboBox QAbstractItemView { background: @layer; color: @textPrimary; selection-background-color: @selected; selection-color: @textPrimary; border: 1px solid @borderStrong; }
        QTextEdit, QPlainTextEdit, QTableWidget, QTreeWidget, QGraphicsView { background: @background; color: @textPrimary; border: 1px solid @borderSubtle; selection-background-color: @selected; selection-color: @textPrimary; }
        QTextEdit:focus, QPlainTextEdit:focus, QTableWidget:focus, QTreeWidget:focus, QGraphicsView:focus { border-color: @focus; }
        QTableWidget { gridline-color: @borderSubtle; alternate-background-color: @layer; }
        QHeaderView::section { background: @layerAlt; color: @textPrimary; border: none; padding: 10px; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { padding: 12px 20px; background: @layer; color: @textSecondary; border-bottom: 2px solid @borderSubtle; }
        QTabBar::tab:selected { background: @background; color: @textPrimary; border-bottom-color: @supportInfo; }
        QTabBar::tab:hover { background: @hover; }
        QTabBar::tab:focus { border: 2px solid @focus; padding: 10px 18px; }
        QGroupBox { border: 1px solid @borderSubtle; margin-top: 16px; padding: 16px; }
        QGroupBox::title { subcontrol-origin: margin; padding: 0 8px; }
        QScrollBar:vertical { background: @layer; width: 12px; margin: 0; }
        QScrollBar::handle:vertical { background: @borderStrong; min-height: 24px; }
        QScrollBar:horizontal { background: @layer; height: 12px; margin: 0; }
        QScrollBar::handle:horizontal { background: @borderStrong; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QStatusBar, QMenuBar { background: @layer; color: @textSecondary; }
        QMenu { background: @layer; color: @textPrimary; border: 1px solid @borderStrong; }
        QMenu::item:selected, QMenuBar::item:selected { background: @selected; }
        QMenu::item:disabled { color: @textDisabled; }
        QToolTip { background: @layer; color: @textPrimary; border: 1px solid @borderStrong; padding: 6px; }
        QSplitter::handle { background: @borderSubtle; }
        QCheckBox, QRadioButton { spacing: 8px; border: 2px solid transparent; padding: 2px; }
        QCheckBox:focus, QRadioButton:focus { border-color: @focus; }
    )");
    const QMap<QString, QColor> substitutions {
        {"background", t.background}, {"layer", t.layer}, {"layerAlt", t.layerAlt},
        {"field", t.field}, {"textPrimary", t.textPrimary}, {"textSecondary", t.textSecondary},
        {"textHelper", t.textHelper}, {"textDisabled", t.textDisabled}, {"textOnColor", t.textOnColor},
        {"borderSubtle", t.borderSubtle}, {"borderStrong", t.borderStrong}, {"focus", t.focus},
        {"buttonPrimary", t.buttonPrimary}, {"buttonPrimaryHover", t.buttonPrimaryHover},
        {"buttonSecondary", t.buttonSecondary}, {"hover", t.hover}, {"selected", t.selected},
        {"supportInfo", t.supportInfo}, {"supportError", t.supportError},
        {"supportSuccess", t.supportSuccess}, {"supportWarning", t.supportWarning}
    };
    // Replace longer token names first so @layer does not corrupt @layerAlt.
    auto names = substitutions.keys();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const auto &name : names) result.replace("@" + name, substitutions[name].name());
    return result;
}
}

ThemeManager *ThemeManager::instance()
{
    static ThemeManager manager;
    return &manager;
}

ThemeManager::ThemeManager()
{
    const QString saved = QSettings().value("appearance/mode", "system").toString();
    if (saved == "light") m_mode = Mode::Light;
    else if (saved == "dark") m_mode = Mode::Dark;
    m_tokens = colors(m_mode == Mode::Dark);
}

void ThemeManager::setMode(Mode mode)
{
    if (mode != Mode::System && mode != Mode::Light && mode != Mode::Dark) return;
    m_mode = mode;
    QSettings settings;
    settings.setValue("appearance/mode", mode == Mode::Light ? "light" : mode == Mode::Dark ? "dark" : "system");
    settings.sync();
    refresh();
}

void ThemeManager::apply(QApplication *application)
{
    if (!application) return;
    if (m_application != application) {
        m_application = application;
        // Capture the platform palette before applying either of our palettes.
        m_systemDark = application->palette().color(QPalette::Window).lightness() < 128;
        m_fallbackCaptured = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
            if (m_mode == Mode::System) refresh();
        });
#endif
    }
    refresh();
}

void ThemeManager::detectSystemAppearance()
{
    m_systemSupported = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (m_application) {
        const auto scheme = m_application->styleHints()->colorScheme();
        if (scheme != Qt::ColorScheme::Unknown) {
            m_systemSupported = true;
            m_systemDark = scheme == Qt::ColorScheme::Dark;
            m_appearanceReason = tr("System appearance follows the Qt platform theme.");
            return;
        }
    }
    m_appearanceReason = tr("This platform does not report system appearance; using the initial %1 palette.")
        .arg(m_systemDark ? tr("dark") : tr("light"));
#else
    m_appearanceReason = tr("Qt %1 cannot detect system appearance changes; System uses the initial %2 palette. Select Light or Dark to override.")
        .arg(QString::fromLatin1(qVersion()), m_systemDark ? tr("dark") : tr("light"));
#endif
    if (!m_fallbackCaptured) m_appearanceReason = tr("System appearance is unavailable until the application is initialized; using Light.");
}

void ThemeManager::refresh()
{
    detectSystemAppearance();
    m_tokens = colors(m_mode == Mode::Dark || (m_mode == Mode::System && m_systemDark));
    if (m_application) {
        QPalette palette;
        palette.setColor(QPalette::Window, m_tokens.background);
        palette.setColor(QPalette::WindowText, m_tokens.textPrimary);
        palette.setColor(QPalette::Base, m_tokens.field);
        palette.setColor(QPalette::AlternateBase, m_tokens.layerAlt);
        palette.setColor(QPalette::Text, m_tokens.textPrimary);
        palette.setColor(QPalette::PlaceholderText, m_tokens.textHelper);
        palette.setColor(QPalette::Button, m_tokens.buttonSecondary);
        palette.setColor(QPalette::ButtonText, m_tokens.textPrimary);
        palette.setColor(QPalette::Highlight, m_tokens.selected);
        palette.setColor(QPalette::HighlightedText, m_tokens.textPrimary);
        palette.setColor(QPalette::Link, m_tokens.supportInfo);
        palette.setColor(QPalette::ToolTipBase, m_tokens.layer);
        palette.setColor(QPalette::ToolTipText, m_tokens.textPrimary);
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, m_tokens.textDisabled);
        m_application->setPalette(palette);
        m_application->setStyleSheet(styleSheet(m_tokens));
    }
    emit themeChanged();
}
