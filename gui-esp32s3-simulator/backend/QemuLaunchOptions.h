#pragma once
#include <QSet>
#include <QStringList>

// QEMU's machine help is the authority for fork-specific properties.
struct QemuLaunchOptions {
    bool downloadBoot = false;
    bool psramEnabled = false;
    QString psramMode = "qspi";
    QString baseMac;
    bool revisionEnabled = false;
    int revision = 0;
};

QSet<QString> parseMachineProperties(const QString &help);
bool buildMachineArguments(const QSet<QString> &properties,
                           const QemuLaunchOptions &options,
                           QStringList &arguments, QString &error);
