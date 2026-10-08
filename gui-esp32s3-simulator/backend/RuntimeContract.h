#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

enum class RuntimePhase {
    Idle, Validating, Launching, Connecting, Initializing, Running, Paused,
    WaitingForDebugger, WaitingForDevice, Stopping, Stopped, Error
};

struct RuntimeStatus {
    RuntimePhase phase = RuntimePhase::Idle;
    QString message;
    QString sessionId;
    quint64 resetEpoch = 0;
};

enum class CapabilityMaturity {
    Catalogued, Implemented, NativeTested, HardwareCompared, Qualified
};

struct RuntimeCapability {
    QString id;
    QString title;
    CapabilityMaturity maturity = CapabilityMaturity::Catalogued;
    bool available = false;
    QString reason;
    QStringList evidence;
};

QString runtimePhaseLabel(RuntimePhase phase);
QString capabilityMaturityLabel(CapabilityMaturity maturity);
QString runtimePhaseKey(RuntimePhase phase);
QString capabilityMaturityKey(CapabilityMaturity maturity);
QJsonObject runtimeStatusJson(const RuntimeStatus &status);
QJsonObject runtimeCapabilityJson(const RuntimeCapability &capability);

Q_DECLARE_METATYPE(RuntimePhase)
Q_DECLARE_METATYPE(RuntimeStatus)
Q_DECLARE_METATYPE(CapabilityMaturity)
Q_DECLARE_METATYPE(RuntimeCapability)
Q_DECLARE_METATYPE(QList<RuntimeCapability>)
