#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="$ROOT_DIR/tests/arduino/esp32s3_bridge_tester"
SKETCH_FILE="$SKETCH_DIR/esp32s3_bridge_tester.ino"
BUILD_DIR="$ROOT_DIR/tests/arduino/build"

FQBN="${FQBN:-esp32:esp32:esp32s3}"
CORE_URL="${CORE_URL:-https://espressif.github.io/arduino-esp32/package_esp32_index.json}"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "[arduino] error: arduino-cli not found in PATH"
  echo "[arduino] install: https://arduino.github.io/arduino-cli/latest/installation/"
  exit 1
fi

if [[ ! -f "$SKETCH_FILE" ]]; then
  echo "[arduino] error: sketch not found: $SKETCH_FILE"
  exit 1
fi

mkdir -p "$BUILD_DIR"

echo "[arduino] configuring core index: $CORE_URL"
arduino-cli config init --overwrite >/dev/null || true
arduino-cli config set board_manager.additional_urls "$CORE_URL"

echo "[arduino] updating index"
arduino-cli core update-index

echo "[arduino] ensuring esp32 core is installed"
arduino-cli core install esp32:esp32

echo "[arduino] compiling sketch"
arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_DIR" \
  "$SKETCH_DIR"

echo "[arduino] compile OK for $FQBN"
