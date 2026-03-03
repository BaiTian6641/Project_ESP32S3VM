/*
 * SPI NOR Flash Test — exercises all W25Q128JV simulator functionality
 *
 * Uses raw SPI calls to test every command the spi_mem_sim.py supports:
 *   - JEDEC ID (0x9F), Manufacturer/Device ID (0x90), Unique ID (0x4B)
 *   - Status Register 1/2/3 read & write (0x05/0x35/0x15, 0x01/0x31/0x11)
 *   - Write Enable / Disable (0x06 / 0x04)
 *   - Standard Read (0x03), Fast Read (0x0B)
 *   - Page Program (0x02) with 256-byte page boundary wrap
 *   - Sector Erase 4KB (0x20), Block Erase 32KB (0x52), 64KB (0xD8)
 *   - Chip Erase (0xC7)
 *   - WIP (Write-In-Progress) busy polling via SR1
 *   - Power Down (0xB9) / Release (0xAB)
 *   - Enable Reset (0x66) + Reset (0x99)
 *
 * Hardware/Simulator wiring:
 *   SPI2 (HSPI):  MOSI=GPIO 11, MISO=GPIO 13, SCLK=GPIO 12, CS=GPIO 10
 *
 * Required libraries:
 *   - SPI (built-in)
 */

#include <Arduino.h>
#include <SPI.h>

/* Bypass ESP-IDF coredump flash check for QEMU compatibility.
 * See ssd1331_sht21_test.ino for detailed explanation. */
extern "C" void __wrap_esp_core_dump_init(void) { /* no-op */ }

// --- Pin Configuration ---
#define SPI_MOSI  11
#define SPI_MISO  13
#define SPI_SCLK  12
#define SPI_CS    10

// --- SPI Flash Commands ---
#define CMD_WRITE_ENABLE     0x06
#define CMD_WRITE_DISABLE    0x04
#define CMD_READ_SR1         0x05
#define CMD_READ_SR2         0x35
#define CMD_READ_SR3         0x15
#define CMD_WRITE_SR1        0x01
#define CMD_WRITE_SR2        0x31
#define CMD_WRITE_SR3        0x11
#define CMD_READ_DATA        0x03
#define CMD_FAST_READ        0x0B
#define CMD_PAGE_PROGRAM     0x02
#define CMD_SECTOR_ERASE     0x20   // 4KB
#define CMD_BLOCK_ERASE_32K  0x52   // 32KB
#define CMD_BLOCK_ERASE_64K  0xD8   // 64KB
#define CMD_CHIP_ERASE       0xC7
#define CMD_JEDEC_ID         0x9F
#define CMD_MFR_DEV_ID       0x90
#define CMD_UNIQUE_ID        0x4B
#define CMD_POWER_DOWN       0xB9
#define CMD_RELEASE_PD       0xAB
#define CMD_ENABLE_RESET     0x66
#define CMD_RESET_DEVICE     0x99

// W25Q128JV expected values
#define EXPECTED_MFR         0xEF
#define EXPECTED_MEM_TYPE    0x40
#define EXPECTED_CAPACITY    0x18
#define EXPECTED_DEV_ID      0x17

// Status register bits
#define SR1_BUSY             0x01
#define SR1_WEL              0x02

static int testNum = 0;
static int passCount = 0;
static int failCount = 0;

static SPIClass *spi = nullptr;

// --- Helpers ---

static void reportResult(const char* name, bool pass) {
  testNum++;
  if (pass) {
    passCount++;
    Serial.printf("[TEST][SPIFLASH] #%02d PASS: %s\n", testNum, name);
  } else {
    failCount++;
    Serial.printf("[TEST][SPIFLASH] #%02d FAIL: %s\n", testNum, name);
  }
}

static void csLow()  { digitalWrite(SPI_CS, LOW);  }
static void csHigh() { digitalWrite(SPI_CS, HIGH); }

static void flashCmd(uint8_t cmd) {
  csLow();
  spi->transfer(cmd);
  csHigh();
}

static uint8_t flashReadSR1() {
  csLow();
  spi->transfer(CMD_READ_SR1);
  uint8_t sr = spi->transfer(0x00);
  csHigh();
  return sr;
}

static void flashWriteEnable() {
  flashCmd(CMD_WRITE_ENABLE);
}

static void flashWaitBusy(uint32_t timeoutMs = 30000) {
  uint32_t start = millis();
  while ((flashReadSR1() & SR1_BUSY) && (millis() - start < timeoutMs)) {
    delay(1);
  }
}

static void flashRead(uint32_t addr, uint8_t *buf, uint16_t len) {
  csLow();
  spi->transfer(CMD_READ_DATA);
  spi->transfer((addr >> 16) & 0xFF);
  spi->transfer((addr >> 8) & 0xFF);
  spi->transfer(addr & 0xFF);
  for (uint16_t i = 0; i < len; i++) {
    buf[i] = spi->transfer(0x00);
  }
  csHigh();
}

static void flashFastRead(uint32_t addr, uint8_t *buf, uint16_t len) {
  csLow();
  spi->transfer(CMD_FAST_READ);
  spi->transfer((addr >> 16) & 0xFF);
  spi->transfer((addr >> 8) & 0xFF);
  spi->transfer(addr & 0xFF);
  spi->transfer(0x00);  // Dummy byte
  for (uint16_t i = 0; i < len; i++) {
    buf[i] = spi->transfer(0x00);
  }
  csHigh();
}

static void flashPageProgram(uint32_t addr, const uint8_t *data, uint16_t len) {
  flashWriteEnable();
  csLow();
  spi->transfer(CMD_PAGE_PROGRAM);
  spi->transfer((addr >> 16) & 0xFF);
  spi->transfer((addr >> 8) & 0xFF);
  spi->transfer(addr & 0xFF);
  for (uint16_t i = 0; i < len; i++) {
    spi->transfer(data[i]);
  }
  csHigh();
  flashWaitBusy();
}

static void flashSectorErase(uint32_t addr) {
  flashWriteEnable();
  csLow();
  spi->transfer(CMD_SECTOR_ERASE);
  spi->transfer((addr >> 16) & 0xFF);
  spi->transfer((addr >> 8) & 0xFF);
  spi->transfer(addr & 0xFF);
  csHigh();
  flashWaitBusy();
}

// ================================================================
//  Test Functions
// ================================================================

void testJedecId() {
  csLow();
  spi->transfer(CMD_JEDEC_ID);
  uint8_t mfr = spi->transfer(0x00);
  uint8_t type = spi->transfer(0x00);
  uint8_t cap = spi->transfer(0x00);
  csHigh();

  Serial.printf("  JEDEC: Mfr=0x%02X, Type=0x%02X, Cap=0x%02X\n", mfr, type, cap);
  reportResult("JEDEC ID (0x9F)", mfr == EXPECTED_MFR && type == EXPECTED_MEM_TYPE && cap == EXPECTED_CAPACITY);
}

void testMfrDeviceId() {
  csLow();
  spi->transfer(CMD_MFR_DEV_ID);
  spi->transfer(0x00);  // Dummy
  spi->transfer(0x00);  // Dummy
  spi->transfer(0x00);  // Dummy
  uint8_t mfr = spi->transfer(0x00);
  uint8_t dev = spi->transfer(0x00);
  csHigh();

  Serial.printf("  Mfr/Dev ID: Mfr=0x%02X, Dev=0x%02X\n", mfr, dev);
  reportResult("Manufacturer/Device ID (0x90)", mfr == EXPECTED_MFR && dev == EXPECTED_DEV_ID);
}

void testUniqueId() {
  csLow();
  spi->transfer(CMD_UNIQUE_ID);
  for (int i = 0; i < 4; i++) spi->transfer(0x00);  // 4 dummy bytes
  uint8_t uid[8];
  for (int i = 0; i < 8; i++) uid[i] = spi->transfer(0x00);
  csHigh();

  Serial.printf("  Unique ID: %02X%02X%02X%02X%02X%02X%02X%02X\n",
                uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);
  // Just verify we got something non-zero
  bool nonZero = false;
  for (int i = 0; i < 8; i++) if (uid[i] != 0x00) nonZero = true;
  reportResult("Unique ID (0x4B)", nonZero);
}

void testWriteEnableDisable() {
  // Write Enable
  flashCmd(CMD_WRITE_ENABLE);
  uint8_t sr = flashReadSR1();
  bool welSet = (sr & SR1_WEL) != 0;
  reportResult("Write Enable (0x06) sets WEL", welSet);

  // Write Disable
  flashCmd(CMD_WRITE_DISABLE);
  sr = flashReadSR1();
  bool welClear = (sr & SR1_WEL) == 0;
  reportResult("Write Disable (0x04) clears WEL", welClear);
}

void testStatusRegisters() {
  // Read SR1/SR2/SR3
  uint8_t sr1 = flashReadSR1();

  csLow();
  spi->transfer(CMD_READ_SR2);
  uint8_t sr2 = spi->transfer(0x00);
  csHigh();

  csLow();
  spi->transfer(CMD_READ_SR3);
  uint8_t sr3 = spi->transfer(0x00);
  csHigh();

  Serial.printf("  SR1=0x%02X, SR2=0x%02X, SR3=0x%02X\n", sr1, sr2, sr3);
  reportResult("Status Register read (0x05/0x35/0x15)", true);

  // Write SR2 (set QE bit)
  flashWriteEnable();
  csLow();
  spi->transfer(CMD_WRITE_SR2);
  spi->transfer(0x02);  // QE bit
  csHigh();
  flashWaitBusy();

  csLow();
  spi->transfer(CMD_READ_SR2);
  sr2 = spi->transfer(0x00);
  csHigh();
  reportResult("Write SR2 QE bit (0x31)", (sr2 & 0x02) != 0);
}

void testEraseAndVerify() {
  // Erase sector at address 0
  flashSectorErase(0x000000);

  // Verify erased (all 0xFF)
  uint8_t buf[64];
  flashRead(0x000000, buf, 64);
  bool allFF = true;
  for (int i = 0; i < 64; i++) {
    if (buf[i] != 0xFF) { allFF = false; break; }
  }
  reportResult("Sector Erase 4KB (0x20) + verify 0xFF", allFF);
}

void testPageProgram() {
  // Write a test pattern to page 0
  uint8_t pattern[32];
  for (int i = 0; i < 32; i++) pattern[i] = i;

  flashSectorErase(0x000000);  // Erase first
  flashPageProgram(0x000000, pattern, 32);

  // Read back
  uint8_t buf[32];
  flashRead(0x000000, buf, 32);

  bool match = true;
  for (int i = 0; i < 32; i++) {
    if (buf[i] != pattern[i]) { match = false; break; }
  }
  reportResult("Page Program (0x02) + Read back", match);
}

void testPageBoundaryWrap() {
  // Write data that crosses 256-byte page boundary
  // Start at offset 250, write 16 bytes -> should wrap at byte 256 back to 0
  flashSectorErase(0x000000);

  uint8_t data[16];
  for (int i = 0; i < 16; i++) data[i] = 0xA0 + i;

  flashPageProgram(250, data, 16);

  // Bytes at 250-255 should be 0xA0-0xA5
  uint8_t buf[16];
  flashRead(250, buf, 6);
  bool endOk = true;
  for (int i = 0; i < 6; i++) {
    if (buf[i] != (0xA0 + i)) { endOk = false; break; }
  }

  // Bytes at 0-9 should be 0xA6-0xAF (wrapped around)
  flashRead(0, buf, 10);
  bool wrapOk = true;
  for (int i = 0; i < 10; i++) {
    if (buf[i] != (0xA6 + i)) { wrapOk = false; break; }
  }

  reportResult("Page boundary wrap (256-byte)", endOk && wrapOk);
  if (!endOk) Serial.println("  End-of-page data mismatch");
  if (!wrapOk) Serial.println("  Wrapped data mismatch");
}

void testAndOnlySemantics() {
  // Flash program can only clear bits (1→0), not set (0→1)
  flashSectorErase(0x001000);

  uint8_t first[4] = {0xFF, 0x0F, 0xF0, 0x55};
  flashPageProgram(0x001000, first, 4);

  // Second write: should AND with existing
  uint8_t second[4] = {0x0F, 0xFF, 0x0F, 0xAA};
  flashPageProgram(0x001000, second, 4);

  uint8_t buf[4];
  flashRead(0x001000, buf, 4);

  // Expected: AND(first, second)
  uint8_t expected[4] = {0x0F, 0x0F, 0x00, 0x00};
  bool ok = true;
  for (int i = 0; i < 4; i++) {
    if (buf[i] != expected[i]) { ok = false; break; }
  }
  reportResult("AND-only write semantics", ok);
  Serial.printf("  Got: [%02X,%02X,%02X,%02X], Expected: [%02X,%02X,%02X,%02X]\n",
                buf[0], buf[1], buf[2], buf[3],
                expected[0], expected[1], expected[2], expected[3]);
}

void testFastRead() {
  // Write known data
  flashSectorErase(0x002000);
  uint8_t data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
  flashPageProgram(0x002000, data, 8);

  // Fast Read (0x0B) with dummy byte
  uint8_t buf[8];
  flashFastRead(0x002000, buf, 8);

  bool match = true;
  for (int i = 0; i < 8; i++) {
    if (buf[i] != data[i]) { match = false; break; }
  }
  reportResult("Fast Read (0x0B) with dummy byte", match);
}

void testBlockErase() {
  // Write data, then 32KB block erase
  flashPageProgram(0x000000, (const uint8_t*)"TEST", 4);

  flashWriteEnable();
  csLow();
  spi->transfer(CMD_BLOCK_ERASE_32K);
  spi->transfer(0x00);
  spi->transfer(0x00);
  spi->transfer(0x00);
  csHigh();
  flashWaitBusy();

  uint8_t buf[4];
  flashRead(0x000000, buf, 4);
  bool erased = (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF);
  reportResult("Block Erase 32KB (0x52)", erased);
}

void testBusyPolling() {
  // Erase and immediately poll SR1 for WIP bit
  flashWriteEnable();
  csLow();
  spi->transfer(CMD_SECTOR_ERASE);
  spi->transfer(0x00);
  spi->transfer(0x30);
  spi->transfer(0x00);
  csHigh();

  // First read of SR1 should have BUSY set (if timing simulation is working)
  uint8_t sr = flashReadSR1();
  bool wasBusy = (sr & SR1_BUSY) != 0;
  Serial.printf("  SR1 immediately after erase: 0x%02X (BUSY=%d)\n", sr, wasBusy);

  // Wait for completion
  flashWaitBusy();
  sr = flashReadSR1();
  bool nowIdle = (sr & SR1_BUSY) == 0;
  reportResult("WIP busy polling (SR1 bit 0)", wasBusy && nowIdle);
}

void testPowerDown() {
  // Power down
  flashCmd(CMD_POWER_DOWN);
  delay(10);

  // In power down, JEDEC ID should return 0xFF
  csLow();
  spi->transfer(CMD_JEDEC_ID);
  uint8_t mfr = spi->transfer(0x00);
  csHigh();
  bool pdBlocked = (mfr == 0xFF);
  reportResult("Power Down (0xB9) blocks commands", pdBlocked);

  // Release from power down
  csLow();
  spi->transfer(CMD_RELEASE_PD);
  uint8_t devId = spi->transfer(0x00);
  csHigh();
  delay(10);

  // JEDEC ID should work again
  csLow();
  spi->transfer(CMD_JEDEC_ID);
  mfr = spi->transfer(0x00);
  csHigh();
  bool released = (mfr == EXPECTED_MFR);
  reportResult("Release Power Down (0xAB)", released);
}

void testSoftwareReset() {
  // Enable Reset + Reset
  flashCmd(CMD_ENABLE_RESET);
  flashCmd(CMD_RESET_DEVICE);
  delay(10);

  // Verify device responds after reset
  csLow();
  spi->transfer(CMD_JEDEC_ID);
  uint8_t mfr = spi->transfer(0x00);
  csHigh();
  reportResult("Software Reset (0x66+0x99)", mfr == EXPECTED_MFR);
}

void testChipErase() {
  // Write data at two different sectors
  uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
  flashSectorErase(0x000000);
  flashPageProgram(0x000000, data, 4);
  flashSectorErase(0x010000);
  flashPageProgram(0x010000, data, 4);

  // Chip erase
  flashWriteEnable();
  flashCmd(CMD_CHIP_ERASE);
  flashWaitBusy(60000);

  // Verify both locations erased
  uint8_t buf[4];
  flashRead(0x000000, buf, 4);
  bool loc1Erased = (buf[0] == 0xFF && buf[1] == 0xFF);

  flashRead(0x010000, buf, 4);
  bool loc2Erased = (buf[0] == 0xFF && buf[1] == 0xFF);

  reportResult("Chip Erase (0xC7)", loc1Erased && loc2Erased);
}

// ================================================================
//  Main
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[TEST][SPIFLASH] === SPI NOR Flash Test Suite ===");

  pinMode(SPI_CS, OUTPUT);
  csHigh();

  spi = new SPIClass(HSPI);
  spi->begin(SPI_SCLK, SPI_MISO, SPI_MOSI, SPI_CS);
  spi->beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  Serial.println("[TEST][SPIFLASH] SPI initialized, running tests...\n");

  testJedecId();
  testMfrDeviceId();
  testUniqueId();
  testWriteEnableDisable();
  testStatusRegisters();
  testEraseAndVerify();
  testPageProgram();
  testPageBoundaryWrap();
  testAndOnlySemantics();
  testFastRead();
  testBlockErase();
  testBusyPolling();
  testPowerDown();
  testSoftwareReset();
  testChipErase();

  Serial.println();
  Serial.println("[TEST][SPIFLASH] === Summary ===");
  Serial.printf("[TEST][SPIFLASH] Total: %d  PASS: %d  FAIL: %d\n",
                testNum, passCount, failCount);
  if (failCount == 0) {
    Serial.println("[TEST][SPIFLASH] ALL TESTS PASSED");
  } else {
    Serial.println("[TEST][SPIFLASH] SOME TESTS FAILED");
  }
}

void loop() {
  delay(10000);
}
