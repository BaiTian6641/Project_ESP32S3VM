#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run from an already activated, locked ESP-IDF 6.1 environment in WSL2.
# All sdkconfig/build/merge outputs stay in the explicitly selected directory.
set -euo pipefail
helper_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$helper_dir/../../../.." && pwd)"
fixture_source="$helper_dir/dualcore-firmware"
fixture_output="${1:-/tmp/esp32s3vm-hostbus-dualcore}"
mkdir -p -- "$fixture_output"
fixture_output="$(cd -- "$fixture_output" && pwd)"
[[ -f "${IDF_PATH:-}/tools/idf.py" ]] || {
    printf '%s\n' 'Activate the locked ESP-IDF 6.1 environment before this helper.' >&2
    exit 2
}
expected_idf=$(python3 -c 'import json,sys; print(next(x for x in json.load(open(sys.argv[1]))["firmware_profiles"] if x["id"]=="idf-6.1")["commit"])' "$repo_dir/runtime-lock.json")
[[ "$(git -C "$IDF_PATH" rev-parse HEAD)" == "$expected_idf" ]] || {
    printf '%s\n' 'Activated ESP-IDF does not match the locked 6.1 commit.' >&2
    exit 2
}
python "$IDF_PATH/tools/idf.py" -C "$fixture_source" -B "$fixture_output/build" \
    -DIDF_TARGET=esp32s3 -DSDKCONFIG="$fixture_output/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$fixture_source/sdkconfig.defaults" build
(cd -- "$fixture_output/build" && python -m esptool --chip esp32s3 merge-bin \
    --fill-flash-size 4MB -o "$fixture_output/dualcore.flash-4MB.bin" @flash_args)
python3 - "$fixture_output/dualcore.flash-4MB.bin" <<'PY'
from pathlib import Path
import hashlib,sys
path=Path(sys.argv[1])
if path.stat().st_size != 4*1024*1024:
    raise SystemExit("Expected a complete 4 MiB flash image")
print(path)
print("sha256="+hashlib.sha256(path.read_bytes()).hexdigest())
PY
