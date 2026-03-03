/*
 * SSD1306 OLED Display Test — exercises all simulator functionality
 *
 * Uses Adafruit_SSD1306 library (which uses Adafruit_GFX).
 * Tests every addressing mode, command, and rendering path
 * that the ssd1306_sim.py simulator supports.
 *
 * Hardware/Simulator wiring:
 *   I2C0 SDA = GPIO 8, SCL = GPIO 9  (ESP32-S3 defaults)
 *   SSD1306 address = 0x3C
 *
 * Required libraries:
 *   - Adafruit SSD1306  (>= 2.5.0)
 *   - Adafruit GFX Library (>= 1.11.0)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* Bypass ESP-IDF coredump flash check for QEMU compatibility.
 * See ssd1331_sht21_test.ino for detailed explanation. */
extern "C" void __wrap_esp_core_dump_init(void) { /* no-op */ }

// --- Configuration ---
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define SDA_PIN       8
#define SCL_PIN       9

// OLED reset pin: -1 = share Arduino reset pin
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Test state
static int testPhase = 0;
static unsigned long phaseStartMs = 0;
static const unsigned long PHASE_DURATION_MS = 3000;

// Forward declarations
void testInit();
void testClearScreen();
void testPixelPattern();
void testTextRendering();
void testLines();
void testRectangles();
void testCircles();
void testTriangles();
void testBitmapXBM();
void testScrolling();
void testInvertDisplay();
void testContrast();
void testDimDisplay();
void testFullScreenFill();
void testMixedContent();
void printTestHeader(const char* name);

// --- Simple XBM bitmap (16x16 smiley) ---
static const uint8_t smiley_bmp[] PROGMEM = {
  0x00, 0x00,  // ................
  0xE0, 0x07,  // .....OOOOO......
  0x18, 0x18,  // ...OO.....OO....
  0x04, 0x20,  // ..O.........O...
  0x62, 0x46,  // .O..OO...OO..O..
  0x62, 0x46,  // .O..OO...OO..O..
  0x02, 0x40,  // .O...........O..
  0x02, 0x40,  // .O...........O..
  0x22, 0x44,  // .O...O...O...O..
  0x42, 0x42,  // .O....O.O....O..
  0x84, 0x21,  // ..O....O....O...
  0x04, 0x20,  // ..O.........O...
  0x18, 0x18,  // ...OO.....OO....
  0xE0, 0x07,  // .....OOOOO......
  0x00, 0x00,  // ................
  0x00, 0x00,  // ................
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[TEST][SSD1306] === SSD1306 Display Test Suite ===");
  Serial.println("[TEST][SSD1306] Initializing I2C...");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400kHz Fast-mode I2C

  Serial.println("[TEST][SSD1306] Initializing display...");

  // Adafruit_SSD1306::begin() sends the full init sequence:
  //  - Display OFF (0xAE)
  //  - Set MUX Ratio (0xA8, 0x3F)
  //  - Set Display Offset (0xD3, 0x00)
  //  - Set Display Start Line (0x40)
  //  - Set Segment Re-map (0xA1)
  //  - Set COM Scan Direction (0xC8)
  //  - Set COM Pins (0xDA, 0x12)
  //  - Set Contrast (0x81, 0xCF)
  //  - Entire Display ON resume (0xA4)
  //  - Set Normal Display (0xA6)
  //  - Set Clock Divide (0xD5, 0x80)
  //  - Set Pre-charge (0xD9, 0xF1)
  //  - Set VCOMH (0xDB, 0x40)
  //  - Charge Pump (0x8D, 0x14)
  //  - Set Memory Addressing Mode to HORIZONTAL (0x20, 0x00) <-- important!
  //  - Display ON (0xAF)
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[TEST][SSD1306] FAIL: display.begin() returned false");
    Serial.println("[TEST][SSD1306] Check I2C address and wiring");
    while (1) { delay(1000); }
  }

  Serial.println("[TEST][SSD1306] PASS: display initialized successfully");
  Serial.printf("[TEST][SSD1306] Display: %dx%d at 0x%02X\n",
                SCREEN_WIDTH, SCREEN_HEIGHT, OLED_ADDR);

  display.clearDisplay();
  display.display();
  delay(500);

  phaseStartMs = millis();
  testPhase = 0;
}

void loop() {
  unsigned long now = millis();

  if ((now - phaseStartMs) >= PHASE_DURATION_MS) {
    testPhase++;
    phaseStartMs = now;
  }

  switch (testPhase) {
    case 0:  testClearScreen();    break;
    case 1:  testPixelPattern();   break;
    case 2:  testTextRendering();  break;
    case 3:  testLines();          break;
    case 4:  testRectangles();     break;
    case 5:  testCircles();        break;
    case 6:  testTriangles();      break;
    case 7:  testBitmapXBM();      break;
    case 8:  testScrolling();      break;
    case 9:  testInvertDisplay();  break;
    case 10: testContrast();       break;
    case 11: testDimDisplay();     break;
    case 12: testFullScreenFill(); break;
    case 13: testMixedContent();   break;
    default:
      // All tests complete — restart cycle
      Serial.println("[TEST][SSD1306] === All tests complete, restarting ===");
      testPhase = 0;
      phaseStartMs = millis();
      break;
  }

  delay(50);
}

// ================================================================
//  Individual Test Functions
// ================================================================

void printTestHeader(const char* name) {
  static int lastPhase = -1;
  if (testPhase != lastPhase) {
    Serial.printf("[TEST][SSD1306] Phase %d: %s\n", testPhase, name);
    lastPhase = testPhase;
  }
}

void testClearScreen() {
  printTestHeader("Clear Screen");
  // Tests: horizontal addressing mode (0x20,0x00), column range (0x21),
  //        page range (0x22), data write of all-zero bytes
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 28);
  display.print(F("SSD1306 Test Suite"));
  display.display();
}

void testPixelPattern() {
  printTestHeader("Pixel Pattern");
  // Tests: individual pixel writes, GDDRAM byte packing
  display.clearDisplay();
  // Checkerboard pattern
  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
      if ((x + y) % 2 == 0) {
        display.drawPixel(x, y, SSD1306_WHITE);
      }
    }
  }
  display.display();  // Sends full 128x8 pages = 1024 bytes via horizontal mode
}

void testTextRendering() {
  printTestHeader("Text Rendering");
  // Tests: multi-size text, cursor positioning, GDDRAM overwrites
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Size 1: Hello!"));

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.println(F("Size 2"));

  display.setTextSize(3);
  display.setCursor(0, 40);
  display.print(F("BIG"));

  display.display();
}

void testLines() {
  printTestHeader("Lines");
  // Tests: Bresenham line drawing across page boundaries
  display.clearDisplay();

  // Diagonal lines from corners
  display.drawLine(0, 0, 127, 63, SSD1306_WHITE);
  display.drawLine(127, 0, 0, 63, SSD1306_WHITE);

  // Horizontal and vertical
  display.drawLine(0, 32, 127, 32, SSD1306_WHITE);
  display.drawLine(64, 0, 64, 63, SSD1306_WHITE);

  display.display();
}

void testRectangles() {
  printTestHeader("Rectangles");
  // Tests: rectangular fills spanning page boundaries
  display.clearDisplay();

  // Concentric rectangles
  for (int i = 0; i < 5; i++) {
    display.drawRect(i * 8, i * 4, SCREEN_WIDTH - i * 16, SCREEN_HEIGHT - i * 8, SSD1306_WHITE);
  }
  // Filled rectangle
  display.fillRect(48, 20, 32, 24, SSD1306_WHITE);

  display.display();
}

void testCircles() {
  printTestHeader("Circles");
  display.clearDisplay();

  display.drawCircle(64, 32, 30, SSD1306_WHITE);
  display.drawCircle(64, 32, 20, SSD1306_WHITE);
  display.fillCircle(64, 32, 8, SSD1306_WHITE);

  // Small circles in corners
  display.fillCircle(10, 10, 5, SSD1306_WHITE);
  display.fillCircle(117, 10, 5, SSD1306_WHITE);
  display.fillCircle(10, 53, 5, SSD1306_WHITE);
  display.fillCircle(117, 53, 5, SSD1306_WHITE);

  display.display();
}

void testTriangles() {
  printTestHeader("Triangles");
  display.clearDisplay();

  display.drawTriangle(64, 2, 10, 60, 118, 60, SSD1306_WHITE);
  display.fillTriangle(64, 15, 40, 50, 88, 50, SSD1306_WHITE);

  display.display();
}

void testBitmapXBM() {
  printTestHeader("Bitmap XBM");
  // Tests: drawXBitmap -> byte-aligned bitmap writes
  display.clearDisplay();

  // Draw smiley at multiple positions
  for (int col = 0; col < 8; col++) {
    display.drawXBitmap(col * 16, 0, smiley_bmp, 16, 16, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 40);
  display.print(F("XBM Bitmap Test"));

  display.display();
}

void testScrolling() {
  printTestHeader("Scrolling");
  // Tests: scroll commands 0x26 (right), 0x27 (left), 0x2F (activate), 0x2E (deactivate)
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 24);
  display.print(F("SCROLL"));
  display.display();

  // Right horizontal scroll on pages 0-7
  display.startscrollright(0x00, 0x07);
  delay(1500);
  display.stopscroll();  // 0x2E

  // Left horizontal scroll
  display.startscrollleft(0x00, 0x07);
  delay(1500);
  display.stopscroll();
}

void testInvertDisplay() {
  printTestHeader("Invert Display");
  // Tests: commands A6 (normal) and A7 (inverse)
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(5, 24);
  display.print(F("INVERTED"));
  display.display();

  display.invertDisplay(true);   // 0xA7
  delay(1500);
  display.invertDisplay(false);  // 0xA6
}

void testContrast() {
  printTestHeader("Contrast Sweep");
  // Tests: command 0x81 with parameter 0x00..0xFF
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 28);
  display.print(F("Contrast Sweep"));
  display.display();

  // Sweep contrast from min to max
  for (int c = 0; c <= 255; c += 17) {
    display.ssd1306_command(0x81);  // Set Contrast
    display.ssd1306_command(c);
    delay(100);
  }
  // Restore default
  display.ssd1306_command(0x81);
  display.ssd1306_command(0xCF);
}

void testDimDisplay() {
  printTestHeader("Dim Display");
  // Tests: 0xA5 (entire display on) and 0xA4 (follow RAM)
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 24);
  display.print(F("DIM"));
  display.display();

  display.dim(true);   // Low contrast
  delay(1500);
  display.dim(false);  // Normal contrast
}

void testFullScreenFill() {
  printTestHeader("Full Screen Fill");
  // Tests: writing all 1024 bytes to fill GDDRAM entirely
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
  delay(1000);

  // Clear half
  display.fillRect(0, 0, 64, 64, SSD1306_BLACK);
  display.display();
}

void testMixedContent() {
  printTestHeader("Mixed Content");
  // Tests: combination of text + graphics + bitmap in one frame
  display.clearDisplay();

  // Border
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);

  // Title
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 4);
  display.print(F("ESP32-S3 Simulator"));

  // Horizontal divider
  display.drawLine(0, 14, 127, 14, SSD1306_WHITE);

  // Temperature-like display
  display.setTextSize(2);
  display.setCursor(4, 20);
  display.print(F("25.3"));
  display.setTextSize(1);
  display.setCursor(56, 20);
  display.print(F("C"));

  // Humidity bar
  display.setCursor(4, 42);
  display.print(F("RH:"));
  display.drawRect(30, 42, 90, 10, SSD1306_WHITE);
  display.fillRect(31, 43, 45, 8, SSD1306_WHITE);  // 50% filled

  // Small icon
  display.drawXBitmap(108, 18, smiley_bmp, 16, 16, SSD1306_WHITE);

  display.display();

  Serial.println("[TEST][SSD1306] PASS: Mixed content rendered");
}
