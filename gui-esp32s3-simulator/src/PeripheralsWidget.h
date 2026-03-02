#pragma once

#include <QHash>
#include <QJsonArray>
#include <QWidget>

class DevicePanelBase;
class PeripheralManager;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QVBoxLayout;

/// Peripherals tab: shows a config toolbar at the top, then a tabbed area
/// where each loaded device gets its own dedicated panel (display screen,
/// sensor gauges, SPI hex view, UART terminal, etc.) with pin-connection
/// wiring diagrams.
class PeripheralsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PeripheralsWidget(QWidget *parent = nullptr);

    void setManager(PeripheralManager *manager);

private slots:
    void onBrowseConfig();
    void onLoadConfig();
    void onStartAll();
    void onStopAll();
    void onRefresh();

    void onDevicesChanged();
    void onDeviceLogLine(const QString &deviceId, const QString &line);
    void onDeviceTraceLine(const QString &deviceId, const QString &line);
    void onManagerMessage(const QString &line);

    void onPanelParameterChange(const QString &deviceId,
                                const QString &paramName,
                                const QVariant &value);

private:
    void rebuildDevicePanels();
    void updateDevicePanels();
    QString panelKindFromSnapshot(const QJsonObject &obj) const;
    QString iconForDeviceType(const QString &type) const;

    PeripheralManager *peripheralManager = nullptr;
    QJsonArray lastSnapshot;

    // Controls bar
    QLineEdit *configPathEdit = nullptr;
    QPushButton *browseButton = nullptr;
    QPushButton *loadButton = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *refreshButton = nullptr;

    // Device panel tabs
    QTabWidget *deviceTabWidget = nullptr;
    QHash<QString, DevicePanelBase *> devicePanels;  // deviceId -> panel
    QHash<QString, QString> devicePanelKinds;        // deviceId -> capabilities.panel.kind

    // Global log
    QTextEdit *logText = nullptr;
    QLabel *deviceCountLabel = nullptr;
};
