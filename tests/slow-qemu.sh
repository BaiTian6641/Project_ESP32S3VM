#!/usr/bin/env bash
set -euo pipefail
# Integration test adapter: QEMU starts listening after the first QMP attempt.
# Machine-help discovery stays immediate so this tests process startup ordering.
if [[ " $* " != *" esp32s3,help "* ]]; then sleep 1; fi
exec "${ESP32S3_REAL_QEMU_BIN:?Set the actual QEMU executable}" "$@"
