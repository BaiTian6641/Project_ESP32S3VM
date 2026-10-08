#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cmake -S "$root/gui-esp32s3-simulator" -B "$root/gui-esp32s3-simulator/build-wsl" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$root/gui-esp32s3-simulator/build-wsl" -j "${BUILD_JOBS:-6}"
ctest --test-dir "$root/gui-esp32s3-simulator/build-wsl" --output-on-failure
python3 "$root/gui-esp32s3-simulator/device-sims/smoke_bridge_check.py"
