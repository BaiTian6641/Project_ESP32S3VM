#include <QtTest/QtTest>
#include <QRegularExpression>
#include "QemuController.h"
#include "PeripheralManager.h"

class NativeI2cTest : public QObject {
    Q_OBJECT
private slots:
    void normalDriverReadsHostSensor() {
        const QString firmware = qEnvironmentVariable("ESP32S3_I2C_FIRMWARE");
        if (firmware.isEmpty()) QSKIP("Set ESP32S3_I2C_FIRMWARE and a QEMU with the native I2C bridge.");
        QString serial, debug;
        QList<int> temperatures;
        PeripheralManager manager;
        QemuController controller;
        connect(&controller, &QemuController::serialLineReceived, this, [&](const QString &line) {
            serial += line + '\n';
            const auto match = QRegularExpression("I2C_TEMP (-?[0-9]+) crc=ok").match(line);
            if (match.hasMatch()) temperatures.append(match.captured(1).toInt());
        });
        connect(&controller, &QemuController::debugMessageReceived, this, [&](const QString &line) { debug += line + '\n'; });
        connect(&controller, &QemuController::i2cTransferRequested, &manager, &PeripheralManager::dispatchI2cTransfer);
        connect(&manager, &PeripheralManager::bridgeResponseReady, &controller, &QemuController::handleBridgeResponse);
        connect(&manager, &PeripheralManager::i2cResponseMapReady, &controller, &QemuController::setI2cBridgeResponseMap);
        manager.setWorkspaceRoot(QStringLiteral(SIMULATOR_SOURCE_DIR));
        QVERIFY(manager.loadConfig(QStringLiteral(SIMULATOR_SOURCE_DIR) + "/peripherals/peripherals.example.json"));
        controller.registerI2cBridgeAddress(0, "40");
        QTest::qWait(400);
        manager.setDeviceParameter("temp0", "noise", false);
        manager.setDeviceParameter("temp0", "temperature", 25.0);
        QTest::qWait(1200);
        controller.setSpiFlashConfig(true, 4);
        controller.loadFirmware(firmware);
        auto received = [&](int expected) { for (int value : temperatures) if (qAbs(value - expected) < 20) return true; return false; };
        QElapsedTimer deadline;
        deadline.start();
        while (!received(2500) && deadline.elapsed() < 15000) QTest::qWait(50);
        QVERIFY2(received(2500), qPrintable(debug + serial));
        QVERIFY(serial.contains("I2C_NACK_OK"));
        temperatures.clear();
        manager.setDeviceParameter("temp0", "temperature", 30.0);
        QTRY_VERIFY_WITH_TIMEOUT(received(3000), 5000);
        QVERIFY(!serial.contains("crc=bad"));
    }
};
QTEST_GUILESS_MAIN(NativeI2cTest)
#include "NativeI2cTest.moc"
