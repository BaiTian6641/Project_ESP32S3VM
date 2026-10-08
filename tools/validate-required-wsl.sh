#!/usr/bin/env bash
# Required integration lane: fails before CTest if either native fixture is absent.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export ESP32S3_QEMU_BIN="${ESP32S3_QEMU_BIN:-$root/build-qemu-source/build-wsl/qemu-system-xtensa}"
export ESP32S3_BOOT_FIRMWARE="${ESP32S3_BOOT_FIRMWARE:-$root/tests/firmware/boot_smoke/build/boot-smoke.merged.bin}"
export ESP32S3_I2C_FIRMWARE="${ESP32S3_I2C_FIRMWARE:-$root/tests/firmware/i2c_sensor/build/i2c-sensor.merged.bin}"
for artifact in "$ESP32S3_QEMU_BIN" "$ESP32S3_BOOT_FIRMWARE" "$ESP32S3_I2C_FIRMWARE"; do
    [[ -s "$artifact" ]] || { printf 'Required artifact missing: %s\n' "$artifact" >&2; exit 2; }
done
cmake -S "$root/gui-esp32s3-simulator" -B "$root/gui-esp32s3-simulator/build-wsl" +    -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/gui-esp32s3-simulator/build-wsl" -j "${BUILD_JOBS:-6}"
ctest --test-dir "$root/gui-esp32s3-simulator/build-wsl" --output-on-failure
