#include <Arduino.h>

/* Bypass ESP-IDF coredump flash check for QEMU compatibility.
 * See ssd1331_sht21_test.ino for detailed explanation. */
extern "C" void __wrap_esp_core_dump_init(void) { /* no-op */ }

// ESP32-S3 Arduino bridge tester for GUI simulator peripheral bridge.
// Emits bridge request lines:
//   [PERIPH][I2C] {...}
//   [PERIPH][SPI] {...}
//   [PERIPH][UART] {...}
// and parses response lines:
//   [PERIPH][I2C][RSP] {...}
//   [PERIPH][SPI][RSP] {...}
//   [PERIPH][UART][RSP] {...}
//
// Use with gui-esp32s3-simulator Serial tab or PTY monitor.

static String rxLine;
static uint32_t seqId = 0;

static uint32_t sentI2C = 0;
static uint32_t sentSPI = 0;
static uint32_t sentUART = 0;

static uint32_t rspI2COk = 0;
static uint32_t rspSPIOk = 0;
static uint32_t rspUARTOk = 0;

static uint32_t rspI2CErr = 0;
static uint32_t rspSPIErr = 0;
static uint32_t rspUARTErr = 0;

static uint32_t nextI2CAt = 0;
static uint32_t nextSPIAt = 0;
static uint32_t nextUARTAt = 0;
static uint32_t nextStatsAt = 0;

static const uint32_t I2C_PERIOD_MS = 1200;
static const uint32_t SPI_PERIOD_MS = 1600;
static const uint32_t UART_PERIOD_MS = 900;
static const uint32_t STATS_PERIOD_MS = 5000;

static void sendBridgeLine(const String &kind, const String &jsonPayload) {
  Serial.print("[PERIPH][");
  Serial.print(kind);
  Serial.print("] ");
  Serial.println(jsonPayload);
}

static void sendI2CTest() {
  // Request: SHT21 temperature measurement via i2c_transfer (write cmd, then read 3 bytes)
  // controller/address should match peripherals.example.json defaults.
  String payload = "{";
  payload += "\"id\":" + String(seqId++) + ",";
  payload += "\"controller\":\"i2c0\",";
  payload += "\"address\":\"0x40\",";
  payload += "\"clock_hz\":100000,";
  payload += "\"repeated_start\":true,";
  payload += "\"ops\":[";
  payload += "{\"dir\":\"write\",\"data\":[227]},";
  payload += "{\"dir\":\"read\",\"len\":3,\"nack_last\":true}";
  payload += "]}";

  sendBridgeLine("I2C", payload);
  sentI2C++;
}

static void sendSPITest() {
  // Request: JEDEC ID read on SPI flash simulator.
  String payload = "{";
  payload += "\"id\":" + String(seqId++) + ",";
  payload += "\"controller\":\"spi2\",";
  payload += "\"chip_select\":0,";
  payload += "\"mode\":\"quad\",";
  payload += "\"frequency_hz\":20000000,";
  payload += "\"tx\":[159],";
  payload += "\"rx_len\":3}";

  sendBridgeLine("SPI", payload);
  sentSPI++;
}

static void sendUARTTest() {
  // Request: UART loopback simulator test frame "Hi from ESP32-S3".
  String payload = "{";
  payload += "\"id\":" + String(seqId++) + ",";
  payload += "\"controller\":\"uart1\",";
  payload += "\"unit\":1,";
  payload += "\"baud\":115200,";
  payload += "\"data_bits\":8,";
  payload += "\"parity\":\"none\",";
  payload += "\"stop_bits\":1,";
  payload += "\"data\":[72,105,32,102,114,111,109,32,69,83,80,51,50,45,83,51]}";

  sendBridgeLine("UART", payload);
  sentUART++;
}

static bool jsonHasOkTrue(const String &line) {
  return line.indexOf("\"ok\":true") >= 0;
}

static void handleResponseLine(const String &line) {
  if (line.startsWith("[PERIPH][I2C][RSP]")) {
    if (jsonHasOkTrue(line)) rspI2COk++; else rspI2CErr++;
  } else if (line.startsWith("[PERIPH][SPI][RSP]")) {
    if (jsonHasOkTrue(line)) rspSPIOk++; else rspSPIErr++;
  } else if (line.startsWith("[PERIPH][UART][RSP]")) {
    if (jsonHasOkTrue(line)) rspUARTOk++; else rspUARTErr++;
  }
}

static void printStats() {
  Serial.println("[ARDUINO][BRIDGE][STATS] ------------------------------");
  Serial.printf("sent:  i2c=%lu spi=%lu uart=%lu\n", (unsigned long)sentI2C, (unsigned long)sentSPI, (unsigned long)sentUART);
  Serial.printf("rsp ok:  i2c=%lu spi=%lu uart=%lu\n", (unsigned long)rspI2COk, (unsigned long)rspSPIOk, (unsigned long)rspUARTOk);
  Serial.printf("rsp err: i2c=%lu spi=%lu uart=%lu\n", (unsigned long)rspI2CErr, (unsigned long)rspSPIErr, (unsigned long)rspUARTErr);
}

static void pollSerialResponses() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (rxLine.length() > 0) {
        handleResponseLine(rxLine);
        rxLine = "";
      }
      continue;
    }
    if (rxLine.length() < 2048) {
      rxLine += c;
    } else {
      rxLine = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  const uint32_t now = millis();
  nextI2CAt = now + 500;
  nextSPIAt = now + 900;
  nextUARTAt = now + 700;
  nextStatsAt = now + STATS_PERIOD_MS;

  Serial.println("[ARDUINO][BRIDGE] ESP32-S3 bridge tester started");
  Serial.println("[ARDUINO][BRIDGE] emitting [PERIPH][I2C|SPI|UART] requests");
}

void loop() {
  pollSerialResponses();

  const uint32_t now = millis();

  if ((int32_t)(now - nextI2CAt) >= 0) {
    sendI2CTest();
    nextI2CAt += I2C_PERIOD_MS;
  }

  if ((int32_t)(now - nextSPIAt) >= 0) {
    sendSPITest();
    nextSPIAt += SPI_PERIOD_MS;
  }

  if ((int32_t)(now - nextUARTAt) >= 0) {
    sendUARTTest();
    nextUARTAt += UART_PERIOD_MS;
  }

  if ((int32_t)(now - nextStatsAt) >= 0) {
    printStats();
    nextStatsAt += STATS_PERIOD_MS;
  }

  delay(2);
}
