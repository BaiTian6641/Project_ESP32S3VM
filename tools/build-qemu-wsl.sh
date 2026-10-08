#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ -f "$root/qemu/configure" ]] || { echo 'Recover the pinned qemu submodule first (see README).' >&2; exit 1; }
# A Linux checkout avoids CRLF from Git for Windows without rewriting the
# recovered source tree. Objects are shared locally; all build edits are here.
source_dir="$root/build-qemu-source"
if [[ ! -d "$source_dir" ]]; then
    git -c core.autocrlf=false clone --shared --no-checkout "$root/qemu" "$source_dir"
    git -C "$source_dir" -c core.autocrlf=false checkout --detach "$(git -C "$root" rev-parse HEAD:qemu)"
fi
mkdir -p "$source_dir/build-wsl"
cd "$source_dir/build-wsl"
bash ../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp \
    --disable-docs --disable-werror --disable-user --disable-tools \
    --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
    --disable-opengl --disable-capstone
ninja -j "${BUILD_JOBS:-6}" qemu-system-xtensa
printf 'QEMU extension binary: %s\n' "$PWD/qemu-system-xtensa"
