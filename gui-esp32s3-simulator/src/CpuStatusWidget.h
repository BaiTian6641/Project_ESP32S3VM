#pragma once

#include <QWidget>

class QTableWidget;
class QPushButton;
class QLabel;
class QLineEdit;
class QemuController;

class CpuStatusWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CpuStatusWidget(QWidget *parent = nullptr);
    void setController(QemuController *ctrl);

private slots:
    void refreshStatus();
    void onLiveToggled();
    void onPauseClicked();
    void onContinueClicked();
    void onStepClicked();
    void onMemoryBaseApply();
    void onAddBreakpoint();
    void onClearBreakpoints();
    void onSnapshotUpdated(const QString &pc,
                           const QStringList &scalarRegs,
                           const QStringList &vectorRegs,
                           const QStringList &memoryWords);

private:
    void setupScalarTable();
    void setupVectorTable();

    QemuController *controller;
    QLabel *pcLabel;
    QTableWidget *scalarRegsTable;
    QTableWidget *vectorRegsTable;
    QTableWidget *memoryInspectTable;
    QPushButton *refreshButton;
    QPushButton *liveButton;
    QPushButton *pauseButton;
    QPushButton *continueButton;
    QPushButton *stepButton;
    QLineEdit *memoryBaseLine;
    QPushButton *applyMemoryBaseButton;
    QLineEdit *breakpointLine;
    QPushButton *addBreakpointButton;
    QPushButton *clearBreakpointsButton;
};
