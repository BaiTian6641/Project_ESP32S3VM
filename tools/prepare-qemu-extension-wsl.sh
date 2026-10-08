#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base_dir="$root/build-qemu-official-base"
base_commit=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["qemu"]["commit"])' "$root/runtime-lock.json")
[[ "$base_commit" =~ ^[0-9a-f]{40}$ ]] || exit 2
extension_dir=${ESP32S3_QEMU_EXTENSION_DIR:-"$HOME/.cache/esp32s3vm/qemu-extension-${base_commit:0:12}"}
[[ -d "$base_dir/.git" ]] || { echo 'Build/prepare the official source first.' >&2; exit 1; }
[[ "$(git -C "$base_dir" rev-parse HEAD)" == "$base_commit" ]] || {
    echo 'Official source does not match the lock.' >&2; exit 1;
}
if [[ ! -e "$extension_dir" ]]; then
    mkdir -p -- "$(dirname -- "$extension_dir")"
    # Independent Git objects and a native Linux working tree keep incremental
    # core builds fast; all actual prototype source/patches stay tracked in repo.
    git -c core.autocrlf=false clone --no-hardlinks --no-checkout "$base_dir" "$extension_dir"
    git -C "$extension_dir" -c core.autocrlf=false checkout --detach "$base_commit"
    git -C "$extension_dir" switch -c codex/hostbus-prototype
fi
[[ -d "$extension_dir/.git" && "$(git -C "$extension_dir" rev-parse HEAD)" == "$base_commit" ]] || {
    echo 'Existing extension checkout differs; preserve it and choose an explicit new cache path.' >&2; exit 1;
}
python3 - "$root" "$extension_dir" "$base_commit" <<'PY'
import json,pathlib,sys
root,source,revision=sys.argv[1:]
output=pathlib.Path(root)/'build-runtime-state/extension-checkout.json'
output.parent.mkdir(parents=True,exist_ok=True)
if output.exists():
    previous=json.loads(output.read_text())
    if previous['source'] != source or previous['base_commit'] != revision:
        raise SystemExit('Existing extension metadata differs; preserve it and use an explicit new profile.')
else:
    output.write_text(json.dumps({'schema_version':1,'source':source,'base_commit':revision,
        'patches':[],'qualification':'source preparation only; inspect hostbus-extension-source.json for applied inputs'},indent=2)+'\n')
print(f'Native WSL extension checkout: {source}')
PY
