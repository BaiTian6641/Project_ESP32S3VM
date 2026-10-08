#pragma once
#include <QJsonObject>
#include <QHash>
#include <QWidget>
#include "RuntimeContract.h"

class PeripheralManager;
class QComboBox;
class QLabel;
class QTableWidget;
class QGraphicsView;
class QGraphicsScene;
class ProjectDocument;
class QPushButton;
class QTemporaryDir;
class QListWidget;
class QLineEdit;
class QFormLayout;

class BoardWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit BoardWorkspace(QWidget *parent = nullptr);
    ~BoardWorkspace() override;
    void setManager(PeripheralManager *manager);
    void setBridgeAvailable(bool available);
    void setRuntimeStatus(const RuntimeStatus &status);
    QJsonObject circuitDocument() const;
    ProjectDocument *projectModel() const { return project; }
    bool openCircuit(const QString &path, QString *error = nullptr);
    bool saveCircuit(const QString &path);
    bool applyCircuit(QString *error = nullptr);
    QString applyUnavailableReason() const;
    void newCircuit();
    // Availability is acknowledged native runtime support, never legacy transport.
    void setNativeCircuitAvailable(bool available, const QString &reason = {});
    // acceptedDocument is the last acknowledged backend graph, not current draft.
    void setNativeCircuitSnapshot(const QJsonObject &snapshot, const QJsonObject &acceptedDocument);
    void setNativeApplyInFlight(bool inFlight, const QString &result = {});
    QString nativeApplyUnavailableReason() const;
    bool requestNativeApply(QString *error = nullptr);
signals:
    void nativeApplyRequested(const QJsonObject &document);
private:
    void loadCurrentConfig();
    void rebuild();
    void rebuildCanvas();
    void updateCanvasTheme();
    void syncCanvasSelection();
    void rebuildPins();
    void rebuildNetEditor();
    void updateNetDetails();
    void rebuildQuantities(const QString &componentId);
    void showEditError(const QString &error);
    void sizePinRows();
    void changePin(const QString &componentId, const QString &terminalId, int gpio);
    void saveProject(bool saveAs = false);
    bool confirmDiscard();
    void scheduleRefresh();
    void updateDocumentStatus();
    void cacheCatalogue(const QString &path);
    void addDevice();
    void removeDevice();
    void selectTerminal(const QString &id);
    void activateTerminal(const QString &id, Qt::KeyboardModifiers modifiers);
    void updateNetHighlight();
    PeripheralManager *manager = nullptr;
    ProjectDocument *project;
    QHash<QString, QJsonObject> templates;
    QTemporaryDir *runtimeConfigDirectory = nullptr;
    RuntimeStatus runtimeStatus;
    bool runtimeStatusKnown = false;
    bool importedV2 = false;
    bool refreshing = false;
    bool refreshQueued = false;
    bool quantityRefreshing = false;
    QJsonObject appliedPreview;
    QString appliedConfigPath;
    bool hasAppliedPreview = false;
    bool nativeCircuitAvailable = false;
    QString nativeCircuitReason;
    bool nativeApplyInFlight = false;
    QString nativeApplyResult;
    QJsonObject nativeCircuitSnapshot;
    QJsonObject nativeSnapshotIdentity;
    QString pendingTerminal;
    QPushButton *nativeApplyButton;
    QPushButton *wireButton;
    QLabel *electricalAvailability;
    QComboBox *selection;
    QComboBox *catalogue;
    QTableWidget *pins;
    QGraphicsView *canvas;
    QGraphicsScene *scene;
    QLabel *message;
    QLabel *bridge;
    QLabel *diagnosticView;
    QLabel *applyStatus;
    QPushButton *applyButton;
    QComboBox *netSelection;
    QComboBox *terminalSelection;
    QListWidget *netMembers;
    QLineEdit *netName;
    QLineEdit *terminalName;
    QFormLayout *quantities;
    QLabel *decoderView;
    QLabel *netInfo;
};
