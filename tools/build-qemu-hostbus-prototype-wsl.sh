#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bash "$root/tools/prepare-qemu-extension-wsl.sh"
source_dir=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source"])' "$root/build-runtime-state/extension-checkout.json")
python3 "$root/tools/apply-qemu-hostbus-prototype.py" --source "$source_dir"
build_dir="$source_dir/build-hostbus"
mkdir -p "$build_dir"
cd "$build_dir"
if [[ ! -f build.ninja ]]; then
    bash ../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp \
        --disable-docs --disable-werror --disable-user --disable-tools \
        --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
        --disable-opengl --disable-capstone
fi
ninja -j "${BUILD_JOBS:-8}" qemu-system-xtensa
python3 "$root/tools/runtime-manifest.py" --qemu "$build_dir/qemu-system-xtensa" \
    --source "$source_dir" --output "$root/build-runtime-state/hostbus-runtime.json"
printf 'Unqualified hostbus prototype binary: %s\n' "$build_dir/qemu-system-xtensa"
