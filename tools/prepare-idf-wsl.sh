#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
profile=${1:-idf-6.1}
case "$profile" in idf-6.1|idf-5.5.5) ;; *) echo 'Choose idf-6.1 or idf-5.5.5.' >&2; exit 2 ;; esac
mapfile -t identity < <(python3 - "$root/runtime-lock.json" "$profile" <<'PY'
import json,sys
record=next(x for x in json.load(open(sys.argv[1]))['firmware_profiles'] if x['id']==sys.argv[2])
print(record['repository']); print(record['commit'])
PY
)
repository=${identity[0]}
revision=${identity[1]}
cache_root=${ESP32S3_IDF_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/esp32s3vm/idf}
cache="$cache_root/$profile-$revision"
source_dir="$cache/source"
export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$HOME/.espressif}
mkdir -p "$cache" "$IDF_TOOLS_PATH"
cache=$(realpath "$cache")
source_dir="$cache/source"
IDF_TOOLS_PATH=$(realpath "$IDF_TOOLS_PATH")
export IDF_TOOLS_PATH
require_native() {
    local filesystem
    filesystem=$(stat -f -c %T "$1")
    case "$filesystem" in ext2/ext3|ext4|btrfs|xfs|overlayfs|tmpfs) ;;
        *) echo "A native Linux filesystem is required, not $filesystem: $1" >&2; exit 1 ;;
    esac
}
require_native "$cache"
require_native "$IDF_TOOLS_PATH"
if [[ ! -e "$source_dir" ]]; then
    # Reuse native Git objects, never its working tree/version. Copying a full
    # historical DrvFS tree can itself time out before the compiler even starts.
    # No alternates/hardlinks or source reads remain tied to the seed checkout.
    staging=$(mktemp -d "$cache/source-fetch.XXXXXX")
    object_cache="$IDF_TOOLS_PATH/master/esp-idf"
    use_object_cache=false
    if [[ -d "$object_cache/.git" ]] &&
       [[ "$(git -C "$object_cache" remote get-url origin)" == "$repository" ]]; then
        case "$(stat -f -c %T "$object_cache")" in
            ext2/ext3|ext4|btrfs|xfs|overlayfs|tmpfs) use_object_cache=true ;;
        esac
    fi
    if "$use_object_cache"; then
        git clone --no-hardlinks --dissociate --no-checkout "$object_cache" "$staging/source"
        git -C "$staging/source" remote set-url origin "$repository"
    else
        git init "$staging/source"
        git -C "$staging/source" remote add origin "$repository"
    fi
    git -C "$staging/source" config core.autocrlf false
    git -C "$staging/source" fetch --depth=1 --no-tags origin "$revision"
    git -C "$staging/source" checkout --detach "$revision"
    mv "$staging/source" "$source_dir"
    rmdir "$staging"
fi
require_native "$source_dir"
[[ "$(git -C "$source_dir" rev-parse HEAD)" == "$revision" ]] || {
    echo 'Existing native IDF checkout differs from the lock; refusing to reset it.' >&2; exit 1;
}
[[ "$(git -C "$source_dir" remote get-url origin)" == "$repository" ]] || {
    echo 'Existing native IDF origin differs from the lock; refusing to rewrite it.' >&2; exit 1;
}
[[ "$(git -C "$source_dir" rev-parse --show-toplevel)" == "$source_dir" ]] || {
    echo 'Native IDF metadata points outside the independent checkout.' >&2; exit 1;
}
git -C "$source_dir" diff --quiet HEAD --ignore-submodules=all || {
    echo 'Native IDF has modified tracked files; refusing to qualify them as locked source.' >&2; exit 1;
}
git -C "$source_dir" submodule sync --recursive
git -C "$source_dir" submodule update --init --recursive --depth=1
export IDF_PATH="$source_dir"
unset IDF_PYTHON_ENV_PATH IDF_VERSION IDF_ENV_ID || true
# Reuse release-specific tools and an already valid native Python environment.
# Do not reinstall an environment that another fixture build may be using.
if ! python3 "$source_dir/tools/idf_tools.py" check; then
    python3 "$source_dir/tools/idf_tools.py" install --targets=esp32s3
fi
if ! (set +eu; source "$source_dir/export.sh" >/dev/null 2>&1); then
    python3 "$source_dir/tools/idf_tools.py" install-python-env
fi
set +eu
source "$source_dir/export.sh"
activation_status=$?
set -eu
[[ "$activation_status" == 0 ]] || { echo 'Locked IDF activation failed.' >&2; exit 1; }
require_native "$IDF_PYTHON_ENV_PATH"
mkdir -p "$cache/builds"
require_native "$cache/builds"
python3 - "$root" "$cache" "$profile" "$repository" "$revision" <<'PY'
import importlib.util, json, os, pathlib, subprocess, sys
root, cache, profile, repository, revision = sys.argv[1:]
cache = pathlib.Path(cache)
source = cache / "source"
spec = importlib.util.spec_from_file_location("runtime_manifest", pathlib.Path(root) / "tools/runtime-manifest.py")
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)
modules = identity.submodule_revisions(source)
for record in modules:
    child = source / record["path"]
    subprocess.run(["git", "-C", str(child), "diff", "--quiet", "HEAD", "--ignore-submodules=all"], check=True)
requirements = sorted((source / "tools").glob("requirements*.txt"))
requirements += sorted((source / "tools/requirements").glob("*.txt"))
manifest = {
    "schema_version": 1, "profile": profile, "repository": repository,
    "commit": identity.git_head(source), "target": "esp32s3",
    "source": str(source), "build_root": str(cache / "builds"),
    "activation": str(cache / "activate.sh"),
    "tools_path": os.environ["IDF_TOOLS_PATH"],
    "python_environment": os.environ["IDF_PYTHON_ENV_PATH"],
    "tools_manifest_sha256": identity.digest(source / "tools/tools.json"),
    "requirements": {str(p.relative_to(source)): identity.digest(p) for p in requirements},
    "submodule_revisions": modules,
    "qualification": "SDK identity only; firmware build/runtime evidence is separate",
}
if manifest["commit"] != revision:
    raise SystemExit("Native SDK HEAD differs from the lock")
(cache / "sdk-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
{
    printf 'export IDF_PATH=%q\n' "$source_dir"
    printf 'export IDF_TOOLS_PATH=%q\n' "$IDF_TOOLS_PATH"
    printf 'export ESP32S3_FIXTURE_PROFILE=%q\n' "$profile"
    printf 'export ESP32S3_IDF_BUILD_ROOT=%q\n' "$cache/builds"
    printf 'export ESP32S3_IDF_METADATA=%q\n' "$cache/sdk-manifest.json"
    printf 'unset IDF_PYTHON_ENV_PATH IDF_VERSION IDF_ENV_ID\n'
    printf 'source %q\n' "$source_dir/export.sh"
} > "$cache/activate.sh"
printf 'Locked native SDK ready. Activate with: source %q\n' "$cache/activate.sh"
printf 'Native build root: %s\nSDK identity: %s\n' "$cache/builds" "$cache/sdk-manifest.json"
