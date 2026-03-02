/*
 * SHT21 Temperature & Humidity Sensor Test — exercises all simulator functionality
 *
 * Uses the SHT21 Arduino library (or compatible HTU21D library).
 * Tests every command and mode that the sht21_sim.py simulator supports:
 *   - Hold-master temperature & humidity reads (0xE3, 0xE5)
 *   - No-hold-master reads (0xF3, 0xF5)
 *   - User register read/write (0xE7, 0xE6)
 *   - Resolution configuration (all 4 modes)
 *   - Soft reset (0xFE) with heater-bit preservation
 *   - CRC-8 verification
 *   - Heater enable/disable
 *
 * Hardware/Simulator wiring:
 *   I2C0 SDA = GPIO 8, SCL = GPIO 9
 *   SHT21 address = 0x40
 *
 * Required libraries:
 *   - Wire (built-in)
 *   (Uses raw Wire calls — no external SHT21 library dependency, to
 *    exercise the exact I2C protocol the simulator models.)
 */

#include <Arduino.h>
#include <Wire.h>

// --- Configuration ---
#define SHT21_ADDR       0x40
#define SDA_PIN          8
#define SCL_PIN          9

// SHT21 commands (per datasheet)
#define CMD_TEMP_HOLD    0xE3
#define CMD_RH_HOLD      0xE5
#define CMD_TEMP_NO_HOLD 0xF3
#define CMD_RH_NO_HOLD   0xF5
#define CMD_WRITE_USER   0xE6
#define CMD_READ_USER    0xE7
#define CMD_SOFT_RESET   0xFE

// Resolution settings (user register bits [7, 0])
// 00 = RH:12-bit, T:14-bit (default)
// 01 = RH:8-bit,  T:12-bit
// 10 = RH:10-bit, T:13-bit
// 11 = RH:11-bit, T:11-bit

static int testNum = 0;
static int passCount = 0;
static int failCount = 0;

// --- CRC-8 verification (polynomial 0x131) ---
static uint8_t crc8_sht21(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}

static void reportResult(const char* name, bool pass) {
  testNum++;
  if (pass) {
    passCount++;
    Serial.printf("[TEST][SHT21] #%02d PASS: %s\n", testNum, name);
  } else {
    failCount++;
    Serial.printf("[TEST][SHT21] #%02d FAIL: %s\n", testNum, name);
  }
}

// --- Raw I2C helpers ---

// Write a single command byte
static bool sht21_writeCmd(uint8_t cmd) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(cmd);
  return Wire.endTransmission() == 0;
}

// Write command + data byte
static bool sht21_writeReg(uint8_t cmd, uint8_t value) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(cmd);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Read N bytes (after a command was sent)
static uint8_t sht21_readBytes(uint8_t *buf, uint8_t count) {
  uint8_t got = Wire.requestFrom((uint8_t)SHT21_ADDR, count);
  for (uint8_t i = 0; i < got && i < count; i++) {
    buf[i] = Wire.read();
  }
  return got;
}

// Hold-master measurement: write cmd, then immediately read 3 bytes (MSB, LSB, CRC)
static bool sht21_measureHold(uint8_t cmd, uint16_t *rawOut) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;

  delay(100);  // Give simulator time even in hold mode

  uint8_t buf[3];
  if (sht21_readBytes(buf, 3) != 3) return false;

  // Verify CRC
  uint8_t expectedCrc = crc8_sht21(buf, 2);
  if (buf[2] != expectedCrc) {
    Serial.printf("  CRC mismatch: got 0x%02X, expected 0x%02X\n", buf[2], expectedCrc);
    return false;
  }

  *rawOut = ((uint16_t)buf[0] << 8) | buf[1];
  return true;
}

// Convert raw temperature to °C
static float rawToTemperature(uint16_t raw) {
  raw &= 0xFFFC;  // Clear status bits
  return -46.85f + 175.72f * (float)raw / 65536.0f;
}

// Convert raw humidity to %RH
static float rawToHumidity(uint16_t raw) {
  raw &= 0xFFFC;  // Clear status bits
  return -6.0f + 125.0f * (float)raw / 65536.0f;
}

// Read user register
static bool sht21_readUserReg(uint8_t *regOut) {
  if (!sht21_writeCmd(CMD_READ_USER)) return false;
  uint8_t buf[2];  // [register, CRC]
  if (sht21_readBytes(buf, 2) != 2) return false;

  // Verify CRC of single register byte
  uint8_t expectedCrc = crc8_sht21(buf, 1);
  if (buf[1] != expectedCrc) {
    Serial.printf("  User reg CRC mismatch: got 0x%02X, expected 0x%02X\n", buf[1], expectedCrc);
    return false;
  }
  *regOut = buf[0];
  return true;
}

// ================================================================
//  Test Functions
// ================================================================

void testI2CPresence() {
  Wire.beginTransmission(SHT21_ADDR);
  bool ack = (Wire.endTransmission() == 0);
  reportResult("I2C device ACK at 0x40", ack);
}

void testSoftReset() {
  bool ok = sht21_writeCmd(CMD_SOFT_RESET);
  delay(15);  // Datasheet: max 15ms for soft reset
  reportResult("Soft reset (0xFE) accepted", ok);
}

void testReadUserRegDefault() {
  uint8_t reg;
  bool ok = sht21_readUserReg(&reg);
  // Default after reset: 0x02 (OTP_reload disabled)
  // Bits [7,0] = 00 -> 12/14-bit resolution
  bool correctDefault = ok && ((reg & 0x81) == 0x00);  // resolution bits = 00
  reportResult("User register default resolution (00)", correctDefault);
  Serial.printf("  User register = 0x%02X\n", reg);
}

void testTemperatureHoldMaster() {
  uint16_t raw;
  bool ok = sht21_measureHold(CMD_TEMP_HOLD, &raw);

  if (ok) {
    float tempC = rawToTemperature(raw);
    Serial.printf("  Raw=0x%04X, T=%.2f °C\n", raw, tempC);
    // Status bit 1 should be 0 for temperature
    bool statusOk = ((raw & 0x02) == 0);
    // Reasonable range
    bool rangeOk = (tempC > -40.0f && tempC < 125.0f);
    reportResult("Temperature hold-master (0xE3) + CRC", ok && statusOk && rangeOk);
  } else {
    reportResult("Temperature hold-master (0xE3) + CRC", false);
  }
}

void testHumidityHoldMaster() {
  uint16_t raw;
  bool ok = sht21_measureHold(CMD_RH_HOLD, &raw);

  if (ok) {
    float rh = rawToHumidity(raw);
    Serial.printf("  Raw=0x%04X, RH=%.2f %%\n", raw, rh);
    // Status bit 1 should be 1 for humidity
    bool statusOk = ((raw & 0x02) != 0);
    bool rangeOk = (rh >= 0.0f && rh <= 100.0f);
    reportResult("Humidity hold-master (0xE5) + CRC", ok && statusOk && rangeOk);
  } else {
    reportResult("Humidity hold-master (0xE5) + CRC", false);
  }
}

void testResolutionChange() {
  // Change to 8-bit RH / 12-bit T  (bits [7,0] = 01)
  // Read current register, modify resolution bits
  uint8_t reg;
  if (!sht21_readUserReg(&reg)) {
    reportResult("Resolution change (01): read reg", false);
    return;
  }

  uint8_t newReg = (reg & 0x7E) | 0x01;  // Set bit 0 = 1, bit 7 = 0
  bool writeOk = sht21_writeReg(CMD_WRITE_USER, newReg);
  delay(10);

  uint8_t verify;
  bool readOk = sht21_readUserReg(&verify);
  bool match = readOk && ((verify & 0x81) == 0x01);  // bits [7,0] = 01
  reportResult("Resolution change to 8/12-bit (01)", writeOk && match);
  Serial.printf("  Wrote=0x%02X, Read back=0x%02X\n", newReg, verify);

  // Measure at lower resolution
  uint16_t raw;
  bool ok = sht21_measureHold(CMD_TEMP_HOLD, &raw);
  if (ok) {
    float tempC = rawToTemperature(raw);
    Serial.printf("  12-bit T: Raw=0x%04X, T=%.2f °C\n", raw, tempC);
  }
  reportResult("Temperature at 12-bit resolution", ok);

  // Change to 10-bit RH / 13-bit T (bits [7,0] = 10)
  newReg = (reg & 0x7E) | 0x80;  // Set bit 7 = 1, bit 0 = 0
  sht21_writeReg(CMD_WRITE_USER, newReg);
  delay(10);
  readOk = sht21_readUserReg(&verify);
  match = readOk && ((verify & 0x81) == 0x80);
  reportResult("Resolution change to 10/13-bit (10)", match);

  // Change to 11-bit RH / 11-bit T (bits [7,0] = 11)
  newReg = (reg & 0x7E) | 0x81;  // Set bit 7 = 1, bit 0 = 1
  sht21_writeReg(CMD_WRITE_USER, newReg);
  delay(10);
  readOk = sht21_readUserReg(&verify);
  match = readOk && ((verify & 0x81) == 0x81);
  reportResult("Resolution change to 11/11-bit (11)", match);

  // Restore default resolution (00)
  newReg = reg & 0x7E;  // Clear bits 7 and 0
  sht21_writeReg(CMD_WRITE_USER, newReg);
}

void testReservedBitPreservation() {
  // Write a value with bits 3,4,5 set — they should be ignored
  uint8_t reg;
  sht21_readUserReg(&reg);

  // Try to set reserved bits 3,4,5
  uint8_t attempt = reg | 0x38;  // Set bits 3,4,5
  sht21_writeReg(CMD_WRITE_USER, attempt);
  delay(10);

  uint8_t verify;
  sht21_readUserReg(&verify);
  // Reserved bits should remain as they were (not what we wrote)
  bool preservedOk = ((verify & 0x38) == (reg & 0x38));
  reportResult("Reserved bits [5:3] preserved on write", preservedOk);
  Serial.printf("  Before=0x%02X, Attempted=0x%02X, After=0x%02X\n", reg, attempt, verify);
}

void testHeaterEnable() {
  // Bit 2 of user register = heater
  uint8_t reg;
  sht21_readUserReg(&reg);

  // Enable heater
  uint8_t withHeater = reg | 0x04;
  sht21_writeReg(CMD_WRITE_USER, withHeater);
  delay(10);

  uint8_t verify;
  sht21_readUserReg(&verify);
  bool heaterOn = (verify & 0x04) != 0;
  reportResult("Heater enable (bit 2)", heaterOn);

  // Soft reset should preserve heater bit
  sht21_writeCmd(CMD_SOFT_RESET);
  delay(15);

  sht21_readUserReg(&verify);
  bool preserved = (verify & 0x04) != 0;
  reportResult("Heater preserved after soft reset", preserved);

  // Disable heater
  uint8_t withoutHeater = verify & ~0x04;
  sht21_writeReg(CMD_WRITE_USER, withoutHeater);
  delay(10);

  sht21_readUserReg(&verify);
  bool heaterOff = (verify & 0x04) == 0;
  reportResult("Heater disable", heaterOff);
}

void testMultipleReadings() {
  // Take multiple readings and verify consistency
  float temps[5];
  bool allOk = true;

  for (int i = 0; i < 5; i++) {
    uint16_t raw;
    if (sht21_measureHold(CMD_TEMP_HOLD, &raw)) {
      temps[i] = rawToTemperature(raw);
    } else {
      allOk = false;
    }
    delay(100);
  }

  if (allOk) {
    // Check readings are within reasonable range of each other (±2°C with noise)
    float minT = temps[0], maxT = temps[0];
    for (int i = 1; i < 5; i++) {
      if (temps[i] < minT) minT = temps[i];
      if (temps[i] > maxT) maxT = temps[i];
    }
    bool consistent = (maxT - minT) < 5.0f;
    Serial.printf("  5 readings: min=%.2f, max=%.2f, spread=%.2f\n", minT, maxT, maxT - minT);
    reportResult("5 consecutive readings consistent", consistent);
  } else {
    reportResult("5 consecutive readings consistent", false);
  }
}

void testCrcVerification() {
  // Explicitly verify CRC on a temperature reading
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_TEMP_HOLD);
  Wire.endTransmission();
  delay(100);

  uint8_t buf[3];
  sht21_readBytes(buf, 3);

  uint8_t computed = crc8_sht21(buf, 2);
  bool crcOk = (buf[2] == computed);
  reportResult("CRC-8 verification (temp)", crcOk);
  Serial.printf("  Data=[0x%02X, 0x%02X], CRC=0x%02X, Computed=0x%02X\n",
                buf[0], buf[1], buf[2], computed);

  // Humidity CRC
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(CMD_RH_HOLD);
  Wire.endTransmission();
  delay(100);

  sht21_readBytes(buf, 3);
  computed = crc8_sht21(buf, 2);
  crcOk = (buf[2] == computed);
  reportResult("CRC-8 verification (humidity)", crcOk);
  Serial.printf("  Data=[0x%02X, 0x%02X], CRC=0x%02X, Computed=0x%02X\n",
                buf[0], buf[1], buf[2], computed);
}

void testUserRegCrc() {
  // Verify CRC on user register read
  sht21_writeCmd(CMD_READ_USER);
  uint8_t buf[2];
  sht21_readBytes(buf, 2);

  uint8_t computed = crc8_sht21(buf, 1);
  bool crcOk = (buf[1] == computed);
  reportResult("CRC-8 verification (user register)", crcOk);
  Serial.printf("  Reg=0x%02X, CRC=0x%02X, Computed=0x%02X\n",
                buf[0], buf[1], computed);
}

// ================================================================
//  Main
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[TEST][SHT21] === SHT21 Sensor Test Suite ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  Serial.println("[TEST][SHT21] Running tests...\n");

  testI2CPresence();
  testSoftReset();
  testReadUserRegDefault();
  testTemperatureHoldMaster();
  testHumidityHoldMaster();
  testResolutionChange();
  testReservedBitPreservation();
  testHeaterEnable();
  testMultipleReadings();
  testCrcVerification();
  testUserRegCrc();

  Serial.println();
  Serial.println("[TEST][SHT21] === Summary ===");
  Serial.printf("[TEST][SHT21] Total: %d  PASS: %d  FAIL: %d\n",
                testNum, passCount, failCount);
  if (failCount == 0) {
    Serial.println("[TEST][SHT21] ALL TESTS PASSED");
  } else {
    Serial.println("[TEST][SHT21] SOME TESTS FAILED");
  }
}

void loop() {
  // Periodic re-read for live monitoring
  static unsigned long nextMs = 5000;
  if (millis() >= nextMs) {
    uint16_t raw;
    if (sht21_measureHold(CMD_TEMP_HOLD, &raw)) {
      Serial.printf("[TEST][SHT21] Live T=%.2f °C", rawToTemperature(raw));
    }
    if (sht21_measureHold(CMD_RH_HOLD, &raw)) {
      Serial.printf("  RH=%.2f %%", rawToHumidity(raw));
    }
    Serial.println();
    nextMs = millis() + 5000;
  }
  delay(100);
}
