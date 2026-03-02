#include <QtTest/QtTest>
#include <QSignalSpy>

#include "QemuController.h"

class BridgeContractTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesI2cEventLine();
    void parsesSpiEventLine();
    void parsesUartEventLine();
    void ignoresInvalidEventLine();
    void emitsResponseLine();
};

void BridgeContractTest::parsesI2cEventLine()
{
    QemuController controller;
    QSignalSpy spy(&controller, &QemuController::i2cTransferRequested);

    const bool consumed = controller.ingestBridgeEventLine(
        "[PERIPH][I2C] {\"controller\":\"i2c0\",\"address\":\"0x40\",\"ops\":[]}");

    QVERIFY(consumed);
    QCOMPARE(spy.count(), 1);

    const QList<QVariant> args = spy.takeFirst();
    const QJsonObject obj = args.at(0).toJsonObject();
    QCOMPARE(obj.value("controller").toString(), QString("i2c0"));
    QCOMPARE(obj.value("address").toString(), QString("0x40"));
}

void BridgeContractTest::parsesSpiEventLine()
{
    QemuController controller;
    QSignalSpy spy(&controller, &QemuController::spiTransferRequested);

    const bool consumed = controller.ingestBridgeEventLine(
        "[PERIPH][SPI] {\"controller\":\"spi2\",\"chip_select\":0,\"mode\":\"quad\",\"tx\":[159],\"rx_len\":3}");

    QVERIFY(consumed);
    QCOMPARE(spy.count(), 1);

    const QList<QVariant> args = spy.takeFirst();
    const QJsonObject obj = args.at(0).toJsonObject();
    QCOMPARE(obj.value("controller").toString(), QString("spi2"));
    QCOMPARE(obj.value("mode").toString(), QString("quad"));
}

void BridgeContractTest::parsesUartEventLine()
{
    QemuController controller;
    QSignalSpy spy(&controller, &QemuController::uartTxRequested);

    const bool consumed = controller.ingestBridgeEventLine(
        "[PERIPH][UART] {\"controller\":\"uart1\",\"unit\":1,\"baud\":115200,\"data\":[72,105]}");

    QVERIFY(consumed);
    QCOMPARE(spy.count(), 1);

    const QList<QVariant> args = spy.takeFirst();
    const QJsonObject obj = args.at(0).toJsonObject();
    QCOMPARE(obj.value("controller").toString(), QString("uart1"));
    QCOMPARE(obj.value("unit").toInt(), 1);
}

void BridgeContractTest::ignoresInvalidEventLine()
{
    QemuController controller;
    QSignalSpy i2cSpy(&controller, &QemuController::i2cTransferRequested);

    QVERIFY(!controller.ingestBridgeEventLine("[PERIPH][I2C] not-json"));
    QCOMPARE(i2cSpy.count(), 0);

    QVERIFY(!controller.ingestBridgeEventLine("plain firmware line"));
    QCOMPARE(i2cSpy.count(), 0);
}

void BridgeContractTest::emitsResponseLine()
{
    QemuController controller;
    QSignalSpy debugSpy(&controller, &QemuController::debugMessageReceived);
    QSignalSpy serialSpy(&controller, &QemuController::serialLineReceived);

    QJsonObject payload;
    payload["ok"] = true;
    payload["bus"] = "i2c";
    payload["method"] = "i2c_transfer";
    payload["device_id"] = "temp0";
    payload["result"] = QJsonObject{{"ack", true}};

    controller.handleBridgeResponse("i2c", payload);

    QCOMPARE(serialSpy.count(), 0);
    QVERIFY(debugSpy.count() >= 1);
    const QList<QVariant> args = debugSpy.takeLast();
    const QString line = args.at(0).toString();
    QVERIFY(line.startsWith("[PERIPH][I2C][RSP] "));
    QVERIFY(line.contains("\"ok\":true"));
    QVERIFY(line.contains("\"device_id\":\"temp0\""));
}

QTEST_GUILESS_MAIN(BridgeContractTest)
#include "BridgeContractTest.moc"
