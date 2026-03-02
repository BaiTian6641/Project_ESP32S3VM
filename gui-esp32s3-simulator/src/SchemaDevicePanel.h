#pragma once

#include "DevicePanelBase.h"

#include <QHash>
#include <QImage>

class QFormLayout;
class QLabel;
class QSpinBox;
class QTextEdit;
class QVBoxLayout;
class QWidget;

class SchemaDevicePanel : public DevicePanelBase
{
    Q_OBJECT

public:
    explicit SchemaDevicePanel(const QString &deviceId,
                               const QString &deviceType,
                               const QJsonObject &rawConfig,
                               QWidget *parent = nullptr);

    void updateState(const QJsonObject &state) override;
    void updateCapabilities(const QJsonObject &caps) override;

private:
    struct ControlBinding {
        QString type;
        QWidget *widget = nullptr;
    };

    struct MetricBinding {
        QString path;
        int decimals = -1;
        QString unit;
        QString boolTrueText;
        QString boolFalseText;
        QLabel *valueLabel = nullptr;
    };

    struct ScriptBinding {
        QString path;
        QTextEdit *view = nullptr;
    };

    void buildBaseUi();
    void rebuildControls(const QJsonObject &panelObj);
    QWidget *buildControlWidget(const QJsonObject &ctrlObj, const QString &name, const QString &type);
    QFormLayout *ensureSectionLayout(const QString &sectionName);
    QVariant valueFromState(const QJsonObject &state, const QString &key) const;
    QVariant valueFromPath(const QJsonObject &state, const QString &path) const;
    QString formatMetricValue(const QVariant &value, const MetricBinding &binding) const;

    void rebuildDisplayView(const QJsonObject &panelObj);
    void updateDisplayView(const QJsonObject &state);
    void renderDisplayBuffer(const QJsonObject &bufferObj);
    void renderPageMajorMono(const QList<int> &data, int w, int h);
    QImage scaleForDisplay(const QImage &source) const;

    void rebuildMetricsView(const QJsonObject &panelObj);
    void updateMetricsView(const QJsonObject &state);

    void rebuildScriptsView(const QJsonObject &panelObj);
    void updateScriptsView(const QJsonObject &state);

    QWidget *m_controlsHost = nullptr;
    QVBoxLayout *m_controlsRootLayout = nullptr;
    QLabel *m_panelTitleLabel = nullptr;
    QLabel *m_panelDescriptionLabel = nullptr;
    QTextEdit *m_stateView = nullptr;

    QWidget *m_displayHost = nullptr;
    QLabel *m_displayLabel = nullptr;
    QLabel *m_displayMetaLabel = nullptr;
    QSpinBox *m_displayZoom = nullptr;
    QImage m_displayImage;
    int m_displayWidth = 128;
    int m_displayHeight = 64;

    QString m_framePrimaryKey = "frame_update";
    QString m_frameFallbackKey = "buffer";
    QString m_frameLayout = "page-major";
    QString m_frameEncoding = "u8";

    QHash<QString, ControlBinding> m_bindings;
    QHash<QString, QFormLayout *> m_sectionLayouts;
    QList<MetricBinding> m_metrics;
    QList<ScriptBinding> m_scriptViews;
    QJsonObject m_lastPanel;
};
