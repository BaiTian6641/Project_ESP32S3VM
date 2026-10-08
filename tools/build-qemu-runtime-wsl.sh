#!/usr/bin/env bash
# Content-addressed combined runtime; official and worker lanes stay untouched.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=$(python3 "$root/tools/prepare-qemu-runtime.py" \
    --profile "${ESP32S3_RUNTIME_PROFILE:-$root/qemu-extensions/runtime-profile.json}")
build_dir="$source_dir/build-runtime"
mkdir -p "$build_dir"
cd "$build_dir"
if [[ ! -f build.ninja ]]; then
    bash ../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp \
        --disable-docs --disable-werror --disable-user --disable-tools \
        --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
        --disable-opengl --disable-capstone
fi
ninja -j "${BUILD_JOBS:-4}" qemu-system-xtensa
python3 "$root/tools/runtime-manifest.py" --qemu "$build_dir/qemu-system-xtensa" \
    --source "$source_dir" \
    --output "$root/build-runtime-state/$(basename "$source_dir")-runtime.json"
printf 'Combined runtime (qualification is fixture-specific): %s\n' "$build_dir/qemu-system-xtensa"
