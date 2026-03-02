#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QVBoxLayout;
class QGroupBox;
class PinConnectionWidget;

/// Base class for all device-specific panels. Each peripheral device loaded
/// by the simulator gets its own panel subclass, rendered inside the
/// Peripherals tab.
class DevicePanelBase : public QWidget
{
    Q_OBJECT

public:
    explicit DevicePanelBase(const QString &deviceId,
                             const QString &deviceType,
                             const QJsonObject &rawConfig,
                             QWidget *parent = nullptr);
    virtual ~DevicePanelBase() = default;

    QString deviceId() const { return m_deviceId; }
    QString deviceType() const { return m_deviceType; }
    QJsonObject rawConfig() const { return m_rawConfig; }

    /// Called when device state is updated from the simulator process.
    virtual void updateState(const QJsonObject &state);

    /// Called when capabilities JSON arrives from the simulator process.
    virtual void updateCapabilities(const QJsonObject &caps);

    /// Called when the device status changes (running/stopped/error).
    void updateStatus(const QString &status, const QString &lastError);

    /// Append a log line to the device-specific log area.
    void appendLog(const QString &line);

signals:
    /// Emitted when the user changes a device parameter from the panel UI.
    void parameterChangeRequested(const QString &deviceId,
                                  const QString &paramName,
                                  const QVariant &value);

protected:
    /// Subclasses place their device-specific widgets inside this layout.
    QVBoxLayout *contentLayout() const { return m_contentLayout; }

    /// Access the pin connection visualization widget.
    PinConnectionWidget *pinWidget() const { return m_pinWidget; }

private:
    void buildBaseLayout();

    QString m_deviceId;
    QString m_deviceType;
    QJsonObject m_rawConfig;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QGroupBox *m_contentGroup = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
    PinConnectionWidget *m_pinWidget = nullptr;
    QGroupBox *m_logGroup = nullptr;
    class QTextEdit *m_logText = nullptr;
};
