#include <QtTest/QtTest>
#include <QSignalSpy>
#include "QemuController.h"

class BootSmokeTest : public QObject {
    Q_OBJECT
private slots:
    void bootsRealFirmware() {
        const auto firmware = qEnvironmentVariable("ESP32S3_BOOT_FIRMWARE");
        if (firmware.isEmpty()) QSKIP("Set ESP32S3_BOOT_FIRMWARE and ESP32S3_QEMU_BIN for the real firmware test.");
        QString serial, debug;
        QemuController controller; // Captured buffers must outlive process teardown.
        connect(&controller, &QemuController::serialLineReceived, this, [&](const QString &line) { serial += line + '\n'; });
        connect(&controller, &QemuController::debugMessageReceived, this, [&](const QString &line) { debug += line + '\n'; });
        QSignalSpy snapshots(&controller, &QemuController::cpuSnapshotUpdated);
        controller.setSpiFlashConfig(true, 4);
        controller.loadFirmware(firmware);
        QElapsedTimer deadline;
        deadline.start();
        while (!serial.contains("ESP32S3VM_BOOT_OK cores=2 flash=4194304") && deadline.elapsed() < 20000) QTest::qWait(50);
        if (!serial.contains("ESP32S3VM_BOOT_OK cores=2 flash=4194304")) qWarning().noquote() << debug << serial;
        QVERIFY2(serial.contains("ESP32S3VM_BOOT_OK cores=2 flash=4194304"), qPrintable(debug + "\n" + serial));
        QTRY_VERIFY_WITH_TIMEOUT(serial.contains("ESP32S3VM_TICK 2"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(debug.contains("[QMP] capabilities enabled"), 5000);
        controller.pauseExecution();
        QTest::qWait(100);
        controller.requestCpuSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(snapshots.count() > 0, 5000);
        QVERIFY(!snapshots.last()[1].toStringList().isEmpty());
        QVERIFY(snapshots.last()[0].toString() != "unavailable");
        QVERIFY(snapshots.last()[1].toStringList()[0] != "unavailable");
        controller.continueExecution();
        QTRY_VERIFY_WITH_TIMEOUT(serial.contains("ESP32S3VM_TICK 3"), 10000);
        serial.clear();
        controller.resetTarget();
        QTRY_VERIFY_WITH_TIMEOUT(serial.contains("ESP32S3VM_BOOT_OK"), 20000);
        QVERIFY2(!debug.contains("command error"), qPrintable(debug));
    }
};
QTEST_GUILESS_MAIN(BootSmokeTest)
#include "BootSmokeTest.moc"
