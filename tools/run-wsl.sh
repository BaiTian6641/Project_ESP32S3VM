#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -z "${ESP32S3_QEMU_BIN:-}" ]]; then
    if command -v qemu-system-xtensa >/dev/null; then
        export ESP32S3_QEMU_BIN="$(command -v qemu-system-xtensa)"
    else
        mapfile -t binaries < <(find "$HOME/.espressif/tools/qemu-xtensa" -name qemu-system-xtensa -type f 2>/dev/null | sort -V)
        if ((${#binaries[@]})); then export ESP32S3_QEMU_BIN="${binaries[-1]}"; fi
    fi
fi
cd "$root/gui-esp32s3-simulator"
exec ./build-wsl/gui_esp32s3_simulator "$@"
