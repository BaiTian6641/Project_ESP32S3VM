#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$root/build-qemu-official-base"
base_commit=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["qemu"]["commit"])' "$root/runtime-lock.json")
repository=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["qemu"]["repository"])' "$root/runtime-lock.json")
[[ "$base_commit" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid locked QEMU revision.' >&2; exit 1; }

# Source is an independent LF checkout. Never reset or overwrite an existing checkout.
# Do not borrow objects from the recovered partial clone: a thin remote fetch can
# rely on missing promised delta bases. The owner checkout remains untouched.
if [[ ! -e "$source_dir" ]]; then
    git init "$source_dir"
    git -C "$source_dir" config core.autocrlf false
    git -C "$source_dir" fetch --depth=1 --no-tags "$repository" "$base_commit"
    git -C "$source_dir" -c core.autocrlf=false checkout --detach "$base_commit"
fi
[[ -d "$source_dir/.git" ]] || { echo 'Official source path is not the expected checkout.' >&2; exit 1; }
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$base_commit" ]] || {
    echo 'Official source revision differs from runtime-lock.json; preserve it and use another checkout.' >&2; exit 1;
}
[[ -z "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]] || {
    echo 'Official source has edits. Refusing to call this an unmodified baseline.' >&2; exit 1;
}
build_dir="$source_dir/build-wsl"
mkdir -p "$build_dir"
cd "$build_dir"
configure_args=(--target-list=xtensa-softmmu --enable-gcrypt --enable-slirp
    --disable-docs --disable-werror --disable-user --disable-tools
    --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc
    --disable-opengl --disable-capstone)
bash ../configure "${configure_args[@]}"
ninja -j "${BUILD_JOBS:-6}" qemu-system-xtensa
python3 "$root/tools/runtime-manifest.py" --qemu "$build_dir/qemu-system-xtensa" \
    --source "$source_dir" --output "$build_dir/runtime-manifest.json"
printf 'Official baseline binary: %s\n' "$build_dir/qemu-system-xtensa"
