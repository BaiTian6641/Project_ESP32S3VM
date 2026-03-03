#!/usr/bin/env bash
#
# Build all Arduino test sketches for ESP32-S3
# Usage:  bash tests/arduino/build_all_sketches.sh
# Override board: FQBN=esp32:esp32:esp32s3 bash tests/arduino/build_all_sketches.sh
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARDUINO_DIR="$ROOT_DIR/tests/arduino"
BUILD_DIR="$ARDUINO_DIR/build"

FQBN="${FQBN:-esp32:esp32:esp32s3}"
CORE_URL="${CORE_URL:-https://espressif.github.io/arduino-esp32/package_esp32_index.json}"

# Sketches that need no external libraries
BASIC_SKETCHES=(
  "esp32s3_bridge_tester"
  "sht21_sensor_test"
  "spi_flash_test"
  "uart_loopback_test"
)

# Sketches needing Adafruit display libraries
OLED_SKETCHES=(
  "ssd1306_display_test"
  "ssd1331_sht21_test"
  "combined_peripheral_test"
)

# ----------------------------------------------------------------
# Pre-flight
# ----------------------------------------------------------------
if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "[build] ERROR: arduino-cli not found in PATH"
  echo "[build] Install: https://arduino.github.io/arduino-cli/latest/installation/"
  exit 1
fi

mkdir -p "$BUILD_DIR"

# ----------------------------------------------------------------
# Core setup
# ----------------------------------------------------------------
echo "[build] configuring core index: $CORE_URL"
arduino-cli config init --overwrite >/dev/null 2>&1 || true
arduino-cli config set board_manager.additional_urls "$CORE_URL"

echo "[build] updating index"
arduino-cli core update-index

echo "[build] ensuring esp32 core is installed"
arduino-cli core install esp32:esp32

# ----------------------------------------------------------------
# Library install (for display sketches)
# ----------------------------------------------------------------
echo "[build] installing Adafruit SSD1306 + SSD1331 + GFX libraries"
arduino-cli lib install "Adafruit SSD1306" >/dev/null 2>&1 || true
arduino-cli lib install "Adafruit GFX Library" >/dev/null 2>&1 || true
arduino-cli lib install "Adafruit SSD1331 OLED Driver Library for Arduino" >/dev/null 2>&1 || true

# ----------------------------------------------------------------
# Compile
# ----------------------------------------------------------------
TOTAL=0
PASS=0
FAIL=0

compile_sketch() {
  local name="$1"
  local sketch_dir="$ARDUINO_DIR/$name"
  local out_dir="$BUILD_DIR/$name"

  if [[ ! -d "$sketch_dir" ]]; then
    echo "[build] SKIP: $name (directory not found)"
    return
  fi

  mkdir -p "$out_dir"
  ((TOTAL++)) || true

  echo ""
  echo "[build] ── compiling: $name ──"
  if arduino-cli compile \
    --fqbn "$FQBN" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=esp_core_dump_init" \
    --build-path "$out_dir" \
    "$sketch_dir"; then
    echo "[build] OK: $name"
    ((PASS++)) || true
  else
    echo "[build] FAIL: $name"
    ((FAIL++)) || true
  fi
}

for s in "${BASIC_SKETCHES[@]}"; do
  compile_sketch "$s"
done

for s in "${OLED_SKETCHES[@]}"; do
  compile_sketch "$s"
done

# ----------------------------------------------------------------
# Summary
# ----------------------------------------------------------------
echo ""
echo "════════════════════════════════════════"
echo "  Build Summary ($FQBN)"
echo "  Total: $TOTAL   Pass: $PASS   Fail: $FAIL"
if [[ $FAIL -eq 0 ]]; then
  echo "  ✓ All sketches compiled successfully"
else
  echo "  ✗ Some sketches failed"
fi
echo "════════════════════════════════════════"

exit "$FAIL"
