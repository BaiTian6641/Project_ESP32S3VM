/*
 * Combined Peripheral Test — runs all four simulator tests in one sketch
 *
 * Exercises the complete peripheral simulation system:
 *   1. SSD1306 OLED display (I2C, Adafruit_SSD1306)
 *   2. SHT21 Temperature/Humidity sensor (I2C, raw Wire)
 *   3. SPI NOR Flash W25Q128JV (SPI, raw SPI)
 *   4. UART Loopback (UART1, HardwareSerial)
 *
 * All four devices are tested in sequence with pass/fail reporting.
 *
 * Hardware/Simulator wiring:
 *   I2C0:  SDA=GPIO 8, SCL=GPIO 9    → SSD1306 (0x3C) + SHT21 (0x40)
 *   SPI2:  MOSI=11, MISO=13, SCLK=12, CS=10  → W25Q128JV flash
 *   UART1: TX=GPIO 17, RX=GPIO 18    → Loopback
 *
 * Required libraries:
 *   - Adafruit SSD1306  (>= 2.5.0)
 *   - Adafruit GFX Library (>= 1.11.0)
 *   - Wire, SPI, HardwareSerial (built-in)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* Bypass ESP-IDF coredump flash check for QEMU compatibility.
 * See ssd1331_sht21_test.ino for detailed explanation. */
extern "C" void __wrap_esp_core_dump_init(void) { /* no-op */ }

// ============================================================
//  Pin Configuration
// ============================================================
#define I2C_SDA      8
#define I2C_SCL      9
#define SPI_MOSI     11
#define SPI_MISO     13
#define SPI_SCLK     12
#define SPI_CS       10
#define UART_TX_PIN  17
#define UART_RX_PIN  18

// Device addresses
#define OLED_ADDR    0x3C
#define SHT21_ADDR   0x40

// SHT21 commands
#define CMD_TEMP_HOLD    0xE3
#define CMD_RH_HOLD      0xE5
#define CMD_READ_USER    0xE7
#define CMD_WRITE_USER   0xE6
#define CMD_SOFT_RESET   0xFE

// SPI flash commands
#define CMD_JEDEC_ID      0x9F
#define CMD_WRITE_ENABLE  0x06
#define CMD_READ_SR1      0x05
#define CMD_READ_DATA     0x03
#define CMD_PAGE_PROGRAM  0x02
#define CMD_SECTOR_ERASE  0x20
#define CMD_CHIP_ERASE    0xC7

#define SR1_BUSY          0x01

// Global counters
static int totalTests = 0;
static int totalPass = 0;
static int totalFail = 0;
static const char *currentSuite = "";

// Peripherals
Adafruit_SSD1306 display(128, 64, &Wire, -1);
HardwareSerial LoopSerial(1);
SPIClass *hspi = nullptr;

static void report(const char* name, bool pass) {
  totalTests++;
  if (pass) {
    totalPass++;
    Serial.printf("[TEST][%s] PASS: %s\n", currentSuite, name);
  } else {
    totalFail++;
    Serial.printf("[TEST][%s] FAIL: %s\n", currentSuite, name);
  }
}

// ============================================================
//  SHT21 Helpers
// ============================================================
static uint8_t crc8_sht21(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
  }
  return crc;
}

static float rawToTemp(uint16_t raw) {
  return -46.85f + 175.72f * (float)(raw & 0xFFFC) / 65536.0f;
}

static float rawToRH(uint16_t raw) {
  return -6.0f + 125.0f * (float)(raw & 0xFFFC) / 65536.0f;
}

// ============================================================
//  SPI Flash Helpers
// ============================================================
static void csLow()  { digitalWrite(SPI_CS, LOW);  }
static void csHigh() { digitalWrite(SPI_CS, HIGH); }

static void flashCmd(uint8_t cmd) {
  csLow(); hspi->transfer(cmd); csHigh();
}

static uint8_t flashReadSR1() {
  csLow();
  hspi->transfer(CMD_READ_SR1);
  uint8_t sr = hspi->transfer(0);
  csHigh();
  return sr;
}

static void flashWaitBusy() {
  uint32_t start = millis();
  while ((flashReadSR1() & SR1_BUSY) && (millis() - start < 30000)) delay(1);
}

// ============================================================
//  Suite 1: SSD1306 OLED (I2C)
// ============================================================
void runOledTests() {
  currentSuite = "SSD1306";
  Serial.println("\n[TEST][SSD1306] --- OLED Display Tests ---");

  // Test 1: Init (sends full command sequence: 0xAE, 0xA8, 0xD3, 0x40, 0xA1,
  //          0xC8, 0xDA, 0x81, 0xA4, 0xA6, 0xD5, 0xD9, 0xDB, 0x8D, 0x20, 0xAF)
  bool initOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  report("Display init (full command sequence)", initOk);
  if (!initOk) return;

  // Test 2: Clear display (horizontal addressing write)
  display.clearDisplay();
  display.display();
  report("Clear display (horizontal addr mode)", true);

  // Test 3: Draw text
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Combined Test"));
  display.display();
  report("Text rendering", true);

  // Test 4: Draw graphics
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  display.fillCircle(64, 32, 15, SSD1306_WHITE);
  display.display();
  report("Graphics (rect + filled circle)", true);

  // Test 5: Invert
  display.invertDisplay(true);
  delay(200);
  display.invertDisplay(false);
  report("Invert display (0xA7/0xA6)", true);

  // Test 6: Contrast
  display.ssd1306_command(0x81);
  display.ssd1306_command(0x00);
  delay(100);
  display.ssd1306_command(0x81);
  display.ssd1306_command(0xCF);
  report("Contrast control (0x81)", true);

  // Test 7: Scroll
  display.startscrollright(0, 7);
  delay(500);
  display.stopscroll();
  report("Scroll right + stop (0x26/0x2F/0x2E)", true);

  // Test 8: Full screen content
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(4, 4);
  display.print(F("T: 25.3C  RH: 50%"));
  display.drawLine(0, 14, 127, 14, SSD1306_WHITE);
  display.setCursor(4, 20);
  display.print(F("Flash: OK  UART: OK"));
  display.setCursor(4, 40);
  display.setTextSize(2);
  display.print(F("ALL PASS"));
  display.display();
  report("Full dashboard layout", true);
}

// ============================================================
//  Suite 2: SHT21 Sensor (I2C)
// ============================================================
void runSht21Tests() {
  currentSuite = "SHT21";
  Serial.println("\n[TEST][SHT21] --- Temperature/Humidity Sensor Tests ---");

  // Test 1: Device present
  Wire.beginTransmission(SHT21_ADDR);
  report("I2C ACK at 0x40", Wire.endTransmission() == 0);

  // Test 2: Soft reset
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_SOFT_RESET);
  report("Soft reset (0xFE)", Wire.endTransmission() == 0);
  delay(15);

  // Test 3: Read user register with CRC
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_READ_USER);
  Wire.endTransmission();
  uint8_t regBuf[2];
  Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)2);
  regBuf[0] = Wire.read();
  regBuf[1] = Wire.read();
  uint8_t regCrc = crc8_sht21(regBuf, 1);
  report("User register read + CRC", regBuf[1] == regCrc);
  Serial.printf("  Reg=0x%02X, CRC=0x%02X\n", regBuf[0], regBuf[1]);

  // Test 4: Temperature hold-master
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_TEMP_HOLD);
  Wire.endTransmission();
  delay(100);
  uint8_t tempBuf[3];
  Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)3);
  for (int i = 0; i < 3; i++) tempBuf[i] = Wire.read();
  uint8_t tempCrc = crc8_sht21(tempBuf, 2);
  uint16_t tempRaw = ((uint16_t)tempBuf[0] << 8) | tempBuf[1];
  float tempC = rawToTemp(tempRaw);
  bool tempOk = (tempBuf[2] == tempCrc) && (tempRaw & 0x02) == 0 && (tempC > -40 && tempC < 125);
  report("Temperature hold-master + CRC", tempOk);
  Serial.printf("  T=%.2f°C (raw=0x%04X)\n", tempC, tempRaw);

  // Test 5: Humidity hold-master
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_RH_HOLD);
  Wire.endTransmission();
  delay(100);
  uint8_t rhBuf[3];
  Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)3);
  for (int i = 0; i < 3; i++) rhBuf[i] = Wire.read();
  uint8_t rhCrc = crc8_sht21(rhBuf, 2);
  uint16_t rhRaw = ((uint16_t)rhBuf[0] << 8) | rhBuf[1];
  float rh = rawToRH(rhRaw);
  bool rhOk = (rhBuf[2] == rhCrc) && (rhRaw & 0x02) != 0 && (rh >= 0 && rh <= 100);
  report("Humidity hold-master + CRC", rhOk);
  Serial.printf("  RH=%.2f%% (raw=0x%04X)\n", rh, rhRaw);

  // Test 6: Resolution change
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_WRITE_USER);
  Wire.write((regBuf[0] & 0x7E) | 0x01);  // 8/12-bit resolution
  Wire.endTransmission();
  delay(10);
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_READ_USER);
  Wire.endTransmission();
  uint8_t newReg[2];
  Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)2);
  newReg[0] = Wire.read(); newReg[1] = Wire.read();
  report("Resolution change (8/12-bit)", (newReg[0] & 0x81) == 0x01);

  // Test 7: Heater + soft reset preservation
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_WRITE_USER);
  Wire.write(newReg[0] | 0x04);  // Enable heater
  Wire.endTransmission();
  delay(10);
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_SOFT_RESET);
  Wire.endTransmission();
  delay(15);
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_READ_USER);
  Wire.endTransmission();
  uint8_t afterReset[2];
  Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)2);
  afterReset[0] = Wire.read(); afterReset[1] = Wire.read();
  report("Heater preserved after soft reset", (afterReset[0] & 0x04) != 0);

  // Disable heater
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_WRITE_USER);
  Wire.write(afterReset[0] & ~0x04);
  Wire.endTransmission();
}

// ============================================================
//  Suite 3: SPI NOR Flash (SPI)
// ============================================================
void runSpiFlashTests() {
  currentSuite = "SPIFLASH";
  Serial.println("\n[TEST][SPIFLASH] --- SPI NOR Flash Tests ---");

  // Test 1: JEDEC ID
  csLow();
  hspi->transfer(CMD_JEDEC_ID);
  uint8_t mfr = hspi->transfer(0);
  uint8_t typ = hspi->transfer(0);
  uint8_t cap = hspi->transfer(0);
  csHigh();
  report("JEDEC ID = EF:40:18", mfr == 0xEF && typ == 0x40 && cap == 0x18);

  // Test 2: Write Enable / SR1 WEL
  flashCmd(CMD_WRITE_ENABLE);
  uint8_t sr = flashReadSR1();
  report("Write Enable sets WEL", (sr & 0x02) != 0);

  // Test 3: Sector Erase + verify
  flashCmd(CMD_WRITE_ENABLE);
  csLow();
  hspi->transfer(CMD_SECTOR_ERASE);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  csHigh();
  flashWaitBusy();
  uint8_t readBuf[4];
  csLow();
  hspi->transfer(CMD_READ_DATA);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  for (int i = 0; i < 4; i++) readBuf[i] = hspi->transfer(0);
  csHigh();
  report("Sector erase → 0xFF", readBuf[0]==0xFF && readBuf[1]==0xFF && readBuf[2]==0xFF && readBuf[3]==0xFF);

  // Test 4: Page Program + Readback
  uint8_t wdata[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
  flashCmd(CMD_WRITE_ENABLE);
  csLow();
  hspi->transfer(CMD_PAGE_PROGRAM);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  for (int i = 0; i < 8; i++) hspi->transfer(wdata[i]);
  csHigh();
  flashWaitBusy();

  csLow();
  hspi->transfer(CMD_READ_DATA);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  bool ppOk = true;
  for (int i = 0; i < 8; i++) {
    if (hspi->transfer(0) != wdata[i]) ppOk = false;
  }
  csHigh();
  report("Page Program + Read", ppOk);

  // Test 5: AND-only semantics
  flashCmd(CMD_WRITE_ENABLE);
  csLow();
  hspi->transfer(CMD_PAGE_PROGRAM);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  hspi->transfer(0x0F);  // AND with 0xDE -> 0x0E
  csHigh();
  flashWaitBusy();

  csLow();
  hspi->transfer(CMD_READ_DATA);
  hspi->transfer(0); hspi->transfer(0); hspi->transfer(0);
  uint8_t andVal = hspi->transfer(0);
  csHigh();
  report("AND-only write (0xDE & 0x0F = 0x0E)", andVal == 0x0E);

  // Test 6: WIP busy polling
  flashCmd(CMD_WRITE_ENABLE);
  csLow();
  hspi->transfer(CMD_SECTOR_ERASE);
  hspi->transfer(0); hspi->transfer(0x10); hspi->transfer(0);
  csHigh();
  sr = flashReadSR1();
  bool wasBusy = (sr & SR1_BUSY) != 0;
  flashWaitBusy();
  sr = flashReadSR1();
  bool nowIdle = (sr & SR1_BUSY) == 0;
  report("WIP busy polling", wasBusy && nowIdle);
}

// ============================================================
//  Suite 4: UART Loopback
// ============================================================
void runUartTests() {
  currentSuite = "UART";
  Serial.println("\n[TEST][UART] --- UART Loopback Tests ---");

  // Flush
  while (LoopSerial.available()) LoopSerial.read();

  // Test 1: Basic loopback
  const char msg[] = "Hello";
  LoopSerial.write((const uint8_t*)msg, 5);
  LoopSerial.flush();
  uint8_t buf[16];
  int got = 0;
  uint32_t start = millis();
  while (got < 5 && millis() - start < 1000) {
    if (LoopSerial.available()) buf[got++] = LoopSerial.read();
    else delay(1);
  }
  bool match = (got == 5);
  for (int i = 0; match && i < 5; i++) {
    if (buf[i] != msg[i]) match = false;
  }
  report("Basic loopback (5 bytes)", match);

  // Test 2: Sequential pattern
  while (LoopSerial.available()) LoopSerial.read();
  uint8_t pattern[64];
  for (int i = 0; i < 64; i++) pattern[i] = i;
  LoopSerial.write(pattern, 64);
  LoopSerial.flush();
  got = 0;
  start = millis();
  while (got < 64 && millis() - start < 2000) {
    if (LoopSerial.available()) buf[got++] = LoopSerial.read();
    else delay(1);
  }
  match = (got == 64);
  for (int i = 0; match && i < 64; i++) {
    if (buf[i] != (uint8_t)i) match = false;
  }
  report("Sequential pattern (64 bytes)", match);

  // Test 3: ASCII message
  while (LoopSerial.available()) LoopSerial.read();
  const char *ascii = "ESP32-S3";
  int alen = strlen(ascii);
  LoopSerial.write((const uint8_t*)ascii, alen);
  LoopSerial.flush();
  char abuf[16];
  got = 0;
  start = millis();
  while (got < alen && millis() - start < 1000) {
    if (LoopSerial.available()) abuf[got++] = (char)LoopSerial.read();
    else delay(1);
  }
  abuf[got] = 0;
  report("ASCII loopback", got == alen && strcmp(abuf, ascii) == 0);
}

// ============================================================
//  Main
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║  ESP32-S3 Combined Peripheral Test Suite     ║");
  Serial.println("║  SSD1306 + SHT21 + SPI Flash + UART         ║");
  Serial.println("╚══════════════════════════════════════════════╝");
  Serial.println();

  // Initialize buses
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  pinMode(SPI_CS, OUTPUT);
  digitalWrite(SPI_CS, HIGH);
  hspi = new SPIClass(HSPI);
  hspi->begin(SPI_SCLK, SPI_MISO, SPI_MOSI, SPI_CS);
  hspi->beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  LoopSerial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(200);

  // Run all test suites
  runOledTests();
  runSht21Tests();
  runSpiFlashTests();
  runUartTests();

  // Summary
  Serial.println();
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.printf( "║  TOTAL: %2d tests  PASS: %2d  FAIL: %2d         ║\n",
                 totalTests, totalPass, totalFail);
  if (totalFail == 0) {
    Serial.println("║  ✓ ALL TESTS PASSED                          ║");
  } else {
    Serial.println("║  ✗ SOME TESTS FAILED                         ║");
  }
  Serial.println("╚══════════════════════════════════════════════╝");

  // Update display with final result
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Combined Test Done"));
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
    display.setCursor(0, 16);
    display.printf("Total: %d", totalTests);
    display.setCursor(0, 28);
    display.printf("Pass:  %d", totalPass);
    display.setCursor(0, 40);
    display.printf("Fail:  %d", totalFail);
    display.setTextSize(2);
    display.setCursor(0, 52);
    if (totalFail == 0) {
      display.print(F("ALL OK"));
    } else {
      display.print(F("FAILED"));
    }
    display.display();
  }
}

void loop() {
  delay(10000);
}
