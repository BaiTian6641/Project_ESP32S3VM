#!/usr/bin/env bash
# No hardware access. Qualify the combined binary rather than inheriting lane passes.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=$(python3 "$root/tools/prepare-qemu-runtime.py" \
    --profile "${ESP32S3_RUNTIME_PROFILE:-$root/qemu-extensions/runtime-profile.json}")
qemu="$source_dir/build-runtime/qemu-system-xtensa"
output=${1:-"$root/build-runtime-state/combined-runtime-$(date -u +%Y%m%dT%H%M%SZ)"}
[[ ! -e "$output" ]] || { printf 'Evidence output already exists: %s\n' "$output" >&2; exit 2; }
[[ -x "$qemu" ]] || { echo 'Build the combined runtime first.' >&2; exit 2; }
boot_test="$root/gui-esp32s3-simulator/build-wsl/gui_esp32s3_boot_smoke_test"
images=(
    "$root/tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.merged.bin"
    "$root/tests/firmware/boot_smoke/build-idf-5.5.5/boot-smoke.merged.bin"
    "$root/tests/firmware/arduino_boot/build-idf-5.5.5/arduino-boot.merged.bin"
)
profiles=(idf-6.1 idf-5.5.5 arduino-3.3.12)
flash="$root/tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.qemu_flash_4MB.bin"
simd="$root/tests/firmware/simd_reference/build/simd_reference.merged.bin"
manifest="$root/tests/firmware/simd_reference/build/reference-manifest.json"
golden="$root/build-hardware-host/run-2026-10-07-rebaselined/simd/capture/records.json"
for artifact in "$boot_test" "${images[@]}" "$flash" "$simd" "$manifest" "$golden"; do
    [[ -s "$artifact" ]] || { printf 'Required evidence/artifact missing: %s\n' "$artifact" >&2; exit 2; }
done
mkdir -p "$output"
sha256sum "$qemu" "${images[@]}" "$flash" "$simd" "$manifest" "$golden" | tee "$output/input-sha256.txt"
export ESP32S3_QEMU_BIN="$qemu" QT_QPA_PLATFORM=offscreen
for index in "${!images[@]}"; do
    # Preserve the firmware corpus: QemuController creates a writable sibling
    # flash backend next to this input. Keep both inside this evidence run.
    input_dir="$output/boot-inputs/${profiles[$index]}"
    mkdir -p "$input_dir"
    input="$input_dir/$(basename "${images[$index]}")"
    cp -- "${images[$index]}" "$input"
    cmp -- "${images[$index]}" "$input"
    ESP32S3_BOOT_FIRMWARE="$input" "$boot_test" | tee "$output/boot-${profiles[$index]}.log"
    sha256sum "$input" | tee -a "$output/input-sha256.txt"
done
python3 "$root/tools/hardware-reference.py" --qemu --qemu-bin "$qemu" \
    --firmware "$simd" --manifest "$manifest" --seconds 45 \
    --output "$output/simd" | tee "$output/simd-capture.log"
python3 "$root/qemu-extensions/prototypes/simd-pie/compare-captures.py" \
    --hw "$golden" --qemu "$output/simd/capture/records.json" \
    --output "$output/simd-compare.json" --markdown "$output/simd-compare.md"
peer_dir="$source_dir/build-runtime/hostbus-peer"
bash "$root/qemu-extensions/prototypes/hostbus/build-peer.sh" "$peer_dir"
for scenario in delay-fast delay-slow gdb-raw-step; do
    python3 "$root/qemu-extensions/prototypes/hostbus/probe-test.py" \
        --qemu "$qemu" --peer "$peer_dir/hostbus-peer" --flash "$flash" \
        --data-dir "$root/build-qemu-official-base/pc-bios" \
        --scenario "$scenario" --output "$output/hostbus-$scenario" \
        | tee "$output/hostbus-$scenario.log"
done
printf 'Combined boot/control, SIMD and dependency-barrier smoke passed. Evidence: %s\n' "$output"
