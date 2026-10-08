#!/usr/bin/env bash
# Build/freeze only. The owner delegates this after the real UART model lands.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
mode=${1:?Usage: bash build-idf-fixture.sh MODE QEMU RUNTIME_SOURCE FROZEN_DIRECTORY}
qemu=${2:?Supply final native qemu-system-xtensa executable}
runtime_source=${3:?Supply final combined runtime source manifest including UART}
frozen=${4:?Supply new output directory for frozen boot artifacts}
case "$mode" in connected|absent|disconnected|wrong|uhci|arduino) ;; *) echo "Unknown UART graph mode: $mode" >&2; exit 2 ;; esac
# Consume only the canonical already-ready activation contract. This builder
# never prepares/downloads an SDK or falls back to the historical NTFS trees.
: "${IDF_PATH:?Source the canonical prepared SDK activate.sh first}"
: "${ESP32S3_FIXTURE_PROFILE:?Source canonical SDK activation}"
: "${ESP32S3_IDF_BUILD_ROOT:?Canonical native build root is required}"
: "${ESP32S3_IDF_METADATA:?Canonical SDK identity manifest is required}"
: "${IDF_PYTHON_ENV_PATH:?Canonical activated Python environment is required}"
python="$IDF_PYTHON_ENV_PATH/bin/python"
freeze_args=()
snapshot_args=()
if [[ "$mode" == arduino ]]; then
    export IDF_COMPONENT_MANAGER=0
    export ESP32S3_ARDUINO_SOURCE="${ESP32S3_ARDUINO_SOURCE:-$root/build-compatibility/arduino-3.3.12/vendor/arduino}"
    profile=idf-5.5.5
    expected=b774170ff46c393eeb5e495ea37936038d3f4f4f
    snapshot_args=(--arduino-source "$ESP32S3_ARDUINO_SOURCE")
    [[ $(git -C "$ESP32S3_ARDUINO_SOURCE" rev-parse HEAD) == 94afccf35fb1e401facddbcf9e13bcf7c76a31d8 ]] || {
        echo 'Existing pinned Arduino3.3.12 source revision mismatch' >&2; exit 1;
    }
    elf=uart_native_arduino.elf
else
    profile=idf-6.1
    expected=fff9895c82d744c7237be8847347bdd1b07c6643
    elf=uart_native.elf
fi
[[ "$ESP32S3_FIXTURE_PROFILE" == "$profile" ]] || { echo "Mode $mode requires canonical $profile activation" >&2; exit 1; }
[[ -x "$python" ]] || { echo 'Activated canonical Python executable is unavailable' >&2; exit 1; }
[[ -f "$ESP32S3_IDF_METADATA" ]] || { echo 'Canonical SDK identity manifest is unavailable' >&2; exit 1; }
mkdir -p "$ESP32S3_IDF_BUILD_ROOT"
case "$(stat -f -c %T "$ESP32S3_IDF_BUILD_ROOT")" in ext2/ext3|ext4|btrfs|xfs|overlayfs|tmpfs) ;;
    *) echo 'UART source/build snapshots require a native Linux filesystem' >&2; exit 1 ;;
esac
[[ $(git -C "$IDF_PATH" rev-parse HEAD) == "$expected" ]] || { echo 'Pinned SDK source revision mismatch' >&2; exit 1; }
mapfile -t snapshot < <("$python" "$root/qemu-extensions/prototypes/uart/firmware/prepare-fixture-source.py" \
    --mode "$mode" --sdk-manifest "$ESP32S3_IDF_METADATA" --idf-source "$IDF_PATH" \
    --build-root "$ESP32S3_IDF_BUILD_ROOT" "${snapshot_args[@]}")
[[ "${#snapshot[@]}" == 4 ]] || { echo 'Native fixture source snapshot preparation failed' >&2; exit 1; }
fixture=${snapshot[0]}
build=${snapshot[1]}
defaults="$fixture/sdkconfig.defaults"
if [[ "$mode" == arduino ]]; then
    export ESP32S3_ARDUINO_SOURCE=${snapshot[2]}
    freeze_args=(--arduino-source "$ESP32S3_ARDUINO_SOURCE")
elif [[ "$mode" != connected ]]; then
    defaults="$defaults;$fixture/sdkconfig.$mode.defaults"
fi
"$python" "$IDF_PATH/tools/idf.py" -C "$fixture" -B "$build" -DIDF_TARGET=esp32s3 \
    "-DSDKCONFIG=$build/sdkconfig" "-DSDKCONFIG_DEFAULTS=$defaults" build
(cd "$build" && "$python" -m esptool --chip esp32s3 merge_bin --fill-flash-size 4MB \
    -o uart-native.merged.bin @flash_args)
"$python" "$root/qemu-extensions/prototypes/uart/run-native-fixture.py" freeze \
    --mode "$mode" --qemu "$qemu" --idf "$IDF_PATH" --runtime-source "$runtime_source" \
    --sdk-manifest "$ESP32S3_IDF_METADATA" --source-manifest "${snapshot[3]}" \
    --flash "$build/uart-native.merged.bin" --elf "$build/$elf" \
    --sdkconfig "$build/sdkconfig" --output "$frozen" "${freeze_args[@]}"
