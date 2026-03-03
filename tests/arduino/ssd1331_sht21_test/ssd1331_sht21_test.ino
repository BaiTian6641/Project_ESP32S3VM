/*
 * SSD1331 + SHT21 Integration Test (ESP32-S3)
 *
 * Uses standard Arduino libraries for real hardware behavior:
 *   - Wire (SHT21)
 *   - SPI + Adafruit SSD1331 + Adafruit GFX (display)
 *
 * Wiring (matches peripherals.ssd1331_sht21.example.json):
 *   I2C0: SDA=GPIO8, SCL=GPIO9  -> SHT21 @ 0x40
 *   SPI2: MOSI=GPIO11, MISO=GPIO13, SCLK=GPIO12, CS=GPIO10 -> SSD1331
 *   SSD1331 control pins: DC=GPIO4, RST=GPIO5
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1331.h>

/*
 * Bypass ESP-IDF core-dump flash check.  The pre-compiled Arduino ESP32
 * libraries have CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y baked in, which
 * aborts at boot when the coredump partition content doesn't pass CRC
 * validation.  In QEMU the flash image is synthetic so this always
 * fails.  We use the linker --wrap mechanism (see build script) to
 * replace esp_core_dump_init() with this no-op.
 */
extern "C" void __wrap_esp_core_dump_init(void) {
    /* intentionally empty – coredump not supported under QEMU */
}

#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
#define SHT21_ADDR 0x40

#define SPI_MOSI_PIN 11
#define SPI_MISO_PIN 13
#define SPI_SCLK_PIN 12
#define SSD1331_CS_PIN 10
#define SSD1331_DC_PIN 4
#define SSD1331_RST_PIN 5

#define SSD1331_WIDTH 96
#define SSD1331_HEIGHT 64

#define CMD_TEMP_HOLD 0xE3
#define CMD_RH_HOLD 0xE5
#define CMD_READ_USER 0xE7
#define CMD_SOFT_RESET 0xFE

static int g_total = 0;
static int g_pass = 0;
static int g_fail = 0;

SPIClass hspi(HSPI);
Adafruit_SSD1331 display(&hspi, SSD1331_CS_PIN, SSD1331_DC_PIN, SSD1331_RST_PIN);

static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static void report(const char *suite, const char *name, bool ok) {
  g_total++;
  if (ok) {
    g_pass++;
    Serial.printf("[TEST][%s] PASS: %s\n", suite, name);
  } else {
    g_fail++;
    Serial.printf("[TEST][%s] FAIL: %s\n", suite, name);
  }
}

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

static float rawToTempC(uint16_t raw) {
  raw &= 0xFFFC;
  return -46.85f + 175.72f * (float)raw / 65536.0f;
}

static float rawToRh(uint16_t raw) {
  raw &= 0xFFFC;
  return -6.0f + 125.0f * (float)raw / 65536.0f;
}

static bool sht21ReadUserReg(uint8_t *regOut) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_READ_USER);
  if (Wire.endTransmission() != 0) return false;

  uint8_t buf[2] = {0, 0};
  const uint8_t got = Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)2);
  if (got != 2) return false;
  buf[0] = Wire.read();
  buf[1] = Wire.read();
  if (crc8_sht21(buf, 1) != buf[1]) return false;
  *regOut = buf[0];
  return true;
}

static bool sht21Measure(uint8_t cmd, uint16_t *rawOut) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;

  delay(90);

  uint8_t buf[3] = {0, 0, 0};
  const uint8_t got = Wire.requestFrom((uint8_t)SHT21_ADDR, (uint8_t)3);
  if (got != 3) return false;
  buf[0] = Wire.read();
  buf[1] = Wire.read();
  buf[2] = Wire.read();

  if (crc8_sht21(buf, 2) != buf[2]) return false;
  *rawOut = ((uint16_t)buf[0] << 8) | buf[1];
  return true;
}

static void runSht21Suite(float *outTempC, float *outRh) {
  const char *suite = "SHT21";

  Wire.beginTransmission(SHT21_ADDR);
  report(suite, "I2C ACK at 0x40", Wire.endTransmission() == 0);

  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_SOFT_RESET);
  report(suite, "Soft reset (0xFE)", Wire.endTransmission() == 0);
  delay(15);

  uint8_t reg = 0;
  report(suite, "Read user register + CRC", sht21ReadUserReg(&reg));

  uint16_t rawT = 0;
  bool tOk = sht21Measure(CMD_TEMP_HOLD, &rawT);
  const float tC = tOk ? rawToTempC(rawT) : NAN;
  report(suite, "Temperature hold-master + CRC", tOk && tC > -40.0f && tC < 125.0f);

  uint16_t rawH = 0;
  bool hOk = sht21Measure(CMD_RH_HOLD, &rawH);
  const float rh = hOk ? rawToRh(rawH) : NAN;
  report(suite, "Humidity hold-master + CRC", hOk && rh >= 0.0f && rh <= 100.0f);

  if (outTempC) *outTempC = tC;
  if (outRh) *outRh = rh;

  if (tOk && hOk) {
    Serial.printf("[TEST][SHT21] T=%.2f C, RH=%.2f %%\n", tC, rh);
  }
}

static void drawColorBars() {
  const uint16_t colors[6] = {
    color565(255, 0, 0),
    color565(255, 255, 0),
    color565(0, 255, 0),
    color565(0, 255, 255),
    color565(0, 0, 255),
    color565(255, 0, 255),
  };

  for (int band = 0; band < 6; ++band) {
    const int y0 = (band * SSD1331_HEIGHT) / 6;
    const int y1 = ((band + 1) * SSD1331_HEIGHT) / 6;
    display.fillRect(0, y0, SSD1331_WIDTH, y1 - y0, colors[band]);
  }
}

static void drawTempHum(float tempC, float rh) {
  const uint16_t bg = color565(0, 0, 0);
  const uint16_t fg = color565(255, 255, 255);
  const uint16_t tempColor = color565(255, 80, 0);
  const uint16_t humColor = color565(0, 140, 255);

  const int tempW = (int)constrain((tempC + 20.0f) * 2.0f, 0.0f, (float)SSD1331_WIDTH);
  const int rhW = (int)constrain(rh * 0.96f, 0.0f, (float)SSD1331_WIDTH);

  display.fillScreen(bg);
  display.setTextSize(1);
  display.setTextColor(fg);
  display.setCursor(0, 0);
  display.printf("T:%5.2fC", tempC);
  display.setCursor(0, 32);
  display.printf("RH:%5.2f%%", rh);

  display.drawRect(0, 12, SSD1331_WIDTH, 10, color565(50, 50, 50));
  display.fillRect(1, 13, max(0, tempW - 2), 8, tempColor);

  display.drawRect(0, 44, SSD1331_WIDTH, 10, color565(50, 50, 50));
  display.fillRect(1, 45, max(0, rhW - 2), 8, humColor);
}

static void runSsd1331Suite(float tC, float rh) {
  const char *suite = "SSD1331";

  display.begin(8000000);
  report(suite, "Adafruit SSD1331 begin()", true);

  display.fillScreen(color565(0, 0, 0));
  report(suite, "Clear screen", true);

  drawColorBars();
  report(suite, "Draw RGB color bars (96x64)", true);
  delay(500);

  drawTempHum(tC, rh);
  report(suite, "Draw temperature/humidity dashboard", true);
  delay(500);

  display.invertDisplay(true);
  delay(120);
  display.invertDisplay(false);
  report(suite, "Invert toggle", true);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[TEST] === SSD1331 + SHT21 Integration Test ===");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  hspi.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SSD1331_CS_PIN);

  float tempC = 25.0f;
  float rh = 50.0f;
  runSht21Suite(&tempC, &rh);
  runSsd1331Suite(tempC, rh);

  Serial.printf("\n[TEST] Summary: total=%d pass=%d fail=%d\n", g_total, g_pass, g_fail);
  Serial.println(g_fail == 0 ? "[TEST] RESULT: PASS" : "[TEST] RESULT: FAIL");
}

void loop() {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs < 1500) {
    delay(10);
    return;
  }
  lastMs = now;

  uint16_t rawT = 0;
  uint16_t rawH = 0;
  if (sht21Measure(CMD_TEMP_HOLD, &rawT) && sht21Measure(CMD_RH_HOLD, &rawH)) {
    const float tC = rawToTempC(rawT);
    const float rh = rawToRh(rawH);
    Serial.printf("[RUN] T=%.2f C RH=%.2f %%\n", tC, rh);
    drawTempHum(tC, rh);
  }
}
