#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Isolated WSL/Linux test-peer build; never writes a shared CMake/QEMU build.
set -euo pipefail
prototype_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$prototype_dir/../../.." && pwd)"
output_dir="${1:-/tmp/esp32s3vm-hostbus-peer}"
mkdir -p -- "$output_dir"
backend_dir="$project_dir/gui-esp32s3-simulator/backend"
moc_path="$(qtpaths6 --query QT_INSTALL_LIBEXECS)/moc"
"$moc_path" "$backend_dir/PeripheralTransport.h" -o "$output_dir/moc_PeripheralTransport.cpp"
g++ -std=c++17 -fPIC -O0 -g -I"$backend_dir" \
    $(pkg-config --cflags Qt6Core Qt6Network) \
    "$backend_dir/PeripheralTransport.cpp" "$output_dir/moc_PeripheralTransport.cpp" \
    "$prototype_dir/peer-runner.cpp" $(pkg-config --libs Qt6Core Qt6Network) \
    -o "$output_dir/hostbus-peer"
printf '%s\n' "$output_dir/hostbus-peer"
