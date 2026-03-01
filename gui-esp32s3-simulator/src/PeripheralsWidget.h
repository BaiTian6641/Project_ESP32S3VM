#pragma once

#include <QJsonArray>
#include <QWidget>

class PeripheralManager;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTableWidgetItem;

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
    void onTableSelectionChanged();

private:
    void rebuildTable();
    void showSelectedDetails();

    PeripheralManager *peripheralManager = nullptr;
    QJsonArray lastSnapshot;

    QLineEdit *configPathEdit = nullptr;
    QPushButton *browseButton = nullptr;
    QPushButton *loadButton = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *stopButton = nullptr;
    QPushButton *refreshButton = nullptr;

    QTableWidget *devicesTable = nullptr;
    QTextEdit *detailsText = nullptr;
    QTextEdit *logText = nullptr;
};
