#include "QemuLaunchOptions.h"
#include <QRegularExpression>

QSet<QString> parseMachineProperties(const QString &help)
{
    QSet<QString> result;
    const QRegularExpression pattern("^\\s*([a-zA-Z0-9_-]+)=<", QRegularExpression::MultilineOption);
    auto matches = pattern.globalMatch(help);
    while (matches.hasNext()) result.insert(matches.next().captured(1));
    return result;
}

bool buildMachineArguments(const QSet<QString> &properties,
                           const QemuLaunchOptions &options,
                           QStringList &arguments, QString &error)
{
    arguments.clear();
    error.clear();
    QString machine = "esp32s3";
    QStringList globals;
    if (properties.contains("boot-mode")) {
        machine += options.downloadBoot ? ",boot-mode=download" : ",boot-mode=flash";
    } else {
        globals << "-global" << QString("driver=esp32s3.gpio,property=strap_mode,value=%1")
                                      .arg(options.downloadBoot ? "0x00" : "0x04");
    }
    if (options.psramEnabled) {
        if (properties.contains("psram-mode")) {
            machine += ",psram-mode=" + options.psramMode;
        } else {
            globals << "-global" << QString("driver=ssi_psram,property=is_octal,value=%1")
                                          .arg(options.psramMode == "opi" ? "true" : "false");
        }
    }
    if (!options.baseMac.isEmpty()) {
        if (!properties.contains("mac")) {
            error = "This QEMU has no machine MAC override. Use an eFuse image or the custom QEMU fork.";
            return false;
        }
        machine += ",mac=" + options.baseMac;
    }
    if (options.revisionEnabled) {
        if (!properties.contains("chip-revision")) {
            error = "This QEMU has no machine chip revision override. Use the custom QEMU fork.";
            return false;
        }
        machine += QString(",chip-revision=%1").arg(options.revision);
    }
    arguments << "-M" << machine;
    arguments += globals;
    return true;
}
