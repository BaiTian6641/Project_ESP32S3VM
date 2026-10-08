#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QStringList>
#include <QUndoStack>
#include <optional>

class QIODevice;
class ProjectEditCommand;

struct ProjectDiagnostic {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    QString code;
    QString path;
    QString message;
    bool historical = false;
    bool resolved = false;
};

struct ProjectParameter {
    QJsonValue value;
    QString unit;
    // Finite SI value for a recognized numeric unit. Diagnostics still gate
    // component constraints (for example, positive resistance).
    std::optional<double> siValue;
};

struct ProjectTerminal {
    QString id;
    QString name;
    QString role; // Electrical role is stable even when a display name changes.
    QString domain;
    QString direction;
    std::optional<int> gpio;
    QJsonObject attributes;
};

struct ProjectComponent {
    QString id;
    QString name;
    QString kind;
    QString type;
    QList<ProjectTerminal> terminals;
    QMap<QString, ProjectParameter> parameters;
    QJsonObject simulator;
    QJsonObject attributes;
};

struct ProjectNet {
    QString id;
    QString name;
    QStringList endpoints; // Global, stable terminal IDs; geometry is not connectivity.
    QJsonObject attributes;
};

// Version-3 document and editing boundary, not an electrical solver or runtime.
// Raw JSON is retained alongside typed read views so invalid drafts and vendor
// extensions can be repaired/saved without lossy conversion. Known file paths
// are absolute internally, relative to the destination on disk; other-host
// paths/URIs are preserved with diagnostics, never guessed into local files.
// Backslash UNC and leading // paths stay verbatim. // has no inferred host
// meaning; native Windows UNC access remains unverified. Undo after Save As
// consequently keeps the same resource identities.
class ProjectDocument : public QObject {
    Q_OBJECT
public:
    explicit ProjectDocument(QObject *parent = nullptr);
    static constexpr int SchemaVersion = 3;
    static constexpr int MaximumIdentifierLength = 64;
    // Opaque transport IDs: absolute ASCII full match, never inferred from
    // display names or prefixes. Display names remain arbitrary Unicode.
    static bool isValidIdentifier(const QString &id);
    static QStringList parameterUnits();
    // Keep the physical quantity when recognized numeric dimensions match.
    // Otherwise preserve the value and record the requested unit as a draft.
    // Quantity extension fields are retained in either case.
    static QJsonObject quantityForUnit(QJsonObject quantity, const QString &unit);

    void setLegacyResourceRoot(const QString &path);
    bool loadFile(const QString &path, QString *error = nullptr);
    // Structurally invalid v3 data loads as an editable draft with diagnostics.
    // Parse errors / unsupported versions fail without changing this document.
    bool loadJson(const QJsonObject &json, const QString &sourcePath = {}, QString *error = nullptr);
    QJsonObject toJson() const;
    QString filePath() const { return m_path; }
    QList<ProjectComponent> components() const;
    QList<ProjectNet> nets() const;
    QJsonObject profile() const;
    QJsonObject firmware() const;
    QJsonObject runtime() const;
    QJsonObject geometry() const;
    QList<ProjectDiagnostic> diagnostics() const;
    bool isStructurallyValid() const;
    bool isDirty() const;
    QUndoStack *undoStack() { return &m_undo; }

    bool save(QString *error = nullptr);
    bool saveAs(const QString &path, QString *error = nullptr);
    // Explicit compatibility export only. Refuses topology/typed parameter
    // changes that the legacy decoder/address runtime cannot represent. Its
    // output is configuration metadata, never evidence of electrical routing.
    QJsonObject legacyRuntimeJson(QString *error = nullptr) const;
    // Same strict representability checks, with declared local paths rebased
    // to the explicitly chosen output file's directory. No runtime is applied.
    QJsonObject legacyRuntimeJsonForDestination(const QString &destinationPath, QString *error = nullptr) const;
    QJsonObject legacyRuntimeJsonWithAbsoluteResources(QString *error = nullptr) const;

    QString addComponent(QJsonObject component, QString *error = nullptr);
    bool removeComponent(const QString &id, QString *error = nullptr);
    bool renameComponent(const QString &id, const QString &name, QString *error = nullptr);
    bool renameTerminal(const QString &id, const QString &name, QString *error = nullptr);
    bool renameNet(const QString &id, const QString &name, QString *error = nullptr);
    // A drag uses transient canvas coordinates, then ONE call on release.
    // Do not call for every mouse move: distinct drags remain distinct commands.
    bool moveComponent(const QString &id, const QPointF &position, QString *error = nullptr);
    QString addNet(QJsonObject net = {}, QString *error = nullptr);
    bool removeNet(const QString &id, QString *error = nullptr);
    bool connectTerminal(const QString &netId, const QString &terminalId, QString *error = nullptr);
    bool disconnectTerminal(const QString &netId, const QString &terminalId, QString *error = nullptr);
    // Invalid values remain editable/undoable; diagnostics gate runtime apply.
    bool setParameter(const QString &componentId, const QString &name,
                      const QJsonObject &quantity, QString *error = nullptr);
    bool setSimulator(const QString &componentId, const QJsonObject &simulator, QString *error = nullptr);
    bool setComponentAttributes(const QString &componentId, const QJsonObject &attributes, QString *error = nullptr);
    void setProfile(const QJsonObject &profile);
    void setFirmware(const QJsonObject &firmware);
    void setRuntime(const QJsonObject &runtime);

signals:
    void documentChanged();
    void diagnosticsChanged();
    void dirtyChanged(bool dirty);

protected:
    // A narrow I/O seam allows a partial-write failure test. Production saves
    // always use QSaveFile with direct-write fallback disabled.
    virtual bool writePayload(QIODevice &device, const QByteArray &bytes, QString *error);

private:
    friend class ProjectEditCommand;
    void replaceState(const QJsonObject &json);
    void pushEdit(const QJsonObject &next, const QString &label);
    QJsonObject normalizePaths(QJsonObject json, const QString &base, bool legacySearch) const;
    QJsonObject serializedFor(const QString &path) const;
    QJsonObject legacyRuntimeDocument(const QString &destinationPath, bool absoluteResources, QString *error) const;
    QJsonObject m_json;
    QString m_path;
    QString m_legacyRoot;
    bool m_migratedUnsaved = false;
    QUndoStack m_undo;
};
