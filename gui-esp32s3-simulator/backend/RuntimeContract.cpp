#include "RuntimeContract.h"

#include <QJsonArray>

QString runtimePhaseKey(RuntimePhase phase)
{
    switch (phase) {
    case RuntimePhase::Idle: return QStringLiteral("idle");
    case RuntimePhase::Validating: return QStringLiteral("validating");
    case RuntimePhase::Launching: return QStringLiteral("launching");
    case RuntimePhase::Connecting: return QStringLiteral("connecting");
    case RuntimePhase::Initializing: return QStringLiteral("initializing");
    case RuntimePhase::Running: return QStringLiteral("running");
    case RuntimePhase::Paused: return QStringLiteral("paused");
    case RuntimePhase::WaitingForDebugger: return QStringLiteral("waiting-for-debugger");
    case RuntimePhase::WaitingForDevice: return QStringLiteral("waiting-for-device");
    case RuntimePhase::Stopping: return QStringLiteral("stopping");
    case RuntimePhase::Stopped: return QStringLiteral("stopped");
    case RuntimePhase::Error: return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

QString runtimePhaseLabel(RuntimePhase phase)
{
    switch (phase) {
    case RuntimePhase::Idle: return QStringLiteral("No simulation");
    case RuntimePhase::Validating: return QStringLiteral("Validating firmware");
    case RuntimePhase::Launching: return QStringLiteral("Launching QEMU");
    case RuntimePhase::Connecting: return QStringLiteral("Connecting to QEMU");
    case RuntimePhase::Initializing: return QStringLiteral("Initializing runtime");
    case RuntimePhase::Running: return QStringLiteral("Running");
    case RuntimePhase::Paused: return QStringLiteral("Paused");
    case RuntimePhase::WaitingForDebugger: return QStringLiteral("Waiting for debugger");
    case RuntimePhase::WaitingForDevice: return QStringLiteral("Waiting for device");
    case RuntimePhase::Stopping: return QStringLiteral("Stopping");
    case RuntimePhase::Stopped: return QStringLiteral("Stopped");
    case RuntimePhase::Error: return QStringLiteral("Runtime error");
    }
    return QStringLiteral("Runtime error");
}

QString capabilityMaturityKey(CapabilityMaturity maturity)
{
    switch (maturity) {
    case CapabilityMaturity::Catalogued: return QStringLiteral("catalogued");
    case CapabilityMaturity::Implemented: return QStringLiteral("implemented");
    case CapabilityMaturity::NativeTested: return QStringLiteral("native-tested");
    case CapabilityMaturity::HardwareCompared: return QStringLiteral("hardware-compared");
    case CapabilityMaturity::Qualified: return QStringLiteral("qualified");
    }
    return QStringLiteral("catalogued");
}

QString capabilityMaturityLabel(CapabilityMaturity maturity)
{
    switch (maturity) {
    case CapabilityMaturity::Catalogued: return QStringLiteral("Catalogued");
    case CapabilityMaturity::Implemented: return QStringLiteral("Implemented");
    case CapabilityMaturity::NativeTested: return QStringLiteral("Native tested");
    case CapabilityMaturity::HardwareCompared: return QStringLiteral("Hardware compared");
    case CapabilityMaturity::Qualified: return QStringLiteral("Qualified");
    }
    return QStringLiteral("Catalogued");
}

QJsonObject runtimeStatusJson(const RuntimeStatus &status)
{
    // Epoch is a decimal string so JSON consumers cannot lose uint64 precision.
    return {{"schema_version", 1}, {"phase", runtimePhaseKey(status.phase)},
            {"message", status.message}, {"session_id", status.sessionId},
            {"reset_epoch", QString::number(status.resetEpoch)}};
}

QJsonObject runtimeCapabilityJson(const RuntimeCapability &capability)
{
    return {{"id", capability.id}, {"title", capability.title},
            {"maturity", capabilityMaturityKey(capability.maturity)},
            {"available", capability.available}, {"reason", capability.reason},
            {"evidence", QJsonArray::fromStringList(capability.evidence)}};
}
