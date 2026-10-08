#include <QtTest/QtTest>
#include "QemuLaunchOptions.h"

class LaunchOptionsTest : public QObject {
    Q_OBJECT
private slots:
    void officialBootAndPsram() {
        QemuLaunchOptions options;
        options.downloadBoot = true;
        options.psramEnabled = true;
        options.psramMode = "opi";
        QStringList args;
        QString error;
        QVERIFY(buildMachineArguments({}, options, args, error));
        QCOMPARE(args[1], QString("esp32s3"));
        QVERIFY(args.contains("driver=esp32s3.gpio,property=strap_mode,value=0x00"));
        QVERIFY(args.contains("driver=ssi_psram,property=is_octal,value=true"));
    }
    void customForkProperties() {
        const auto properties = parseMachineProperties("esp32s3-machine options:\n  boot-mode=<string>\n  mac=<string>\n  chip-revision=<int>\n  psram-mode=<string>\n");
        QemuLaunchOptions options;
        options.psramEnabled = true;
        options.baseMac = "02:00:00:00:00:01";
        options.revisionEnabled = true;
        options.revision = 203;
        QStringList args;
        QString error;
        QVERIFY(buildMachineArguments(properties, options, args, error));
        QCOMPARE(args, QStringList({"-M", "esp32s3,boot-mode=flash,psram-mode=qspi,mac=02:00:00:00:00:01,chip-revision=203"}));
    }
    void rejectsUnsupportedIdentity() {
        QemuLaunchOptions options;
        options.baseMac = "02:00:00:00:00:01";
        QStringList args;
        QString error;
        QVERIFY(!buildMachineArguments({}, options, args, error));
        QVERIFY(error.contains("eFuse"));
        options.baseMac.clear();
        options.revisionEnabled = true;
        QVERIFY(!buildMachineArguments({}, options, args, error));
        QVERIFY(error.contains("revision"));
    }
};
QTEST_GUILESS_MAIN(LaunchOptionsTest)
#include "LaunchOptionsTest.moc"
