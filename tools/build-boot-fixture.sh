#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fixture=${1:-boot_smoke}
profile=${ESP32S3_FIXTURE_PROFILE:-idf-6.1}
case "$fixture" in
    boot_smoke) image=boot-smoke.merged.bin; test_binary=gui_esp32s3_boot_smoke_test ;;
    i2c_sensor) image=i2c-sensor.merged.bin; test_binary=gui_esp32s3_native_i2c_test ;;
    *) echo "Unknown firmware fixture: $fixture" >&2; exit 1 ;;
esac
mapfile -t identity < <(python3 - "$root/runtime-lock.json" "$profile" <<'PY'
import json,sys
profiles=json.load(open(sys.argv[1]))['firmware_profiles']
record=next((x for x in profiles if x['id']==sys.argv[2] and x['id'] in ('idf-6.1','idf-5.5.5')), None)
if not record: raise SystemExit('Unknown locked IDF profile')
print(record['repository']); print(record['commit'])
PY
)
[[ ${#identity[@]} == 2 ]] || exit 2
repository=${identity[0]}
revision=${identity[1]}
cache="${ESP32S3_IDF_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/esp32s3vm/idf}/$profile-$revision"
activation=${ESP32S3_IDF_ENV:-$cache/activate.sh}
[[ -f "$activation" ]] || {
    echo "Prepare the locked native SDK first: bash tools/prepare-idf-wsl.sh $profile" >&2; exit 1;
}
set +eu # Vendor activation scripts assume neither nounset nor errexit.
source "$activation"
activation_status=$?
set -eu
[[ "$activation_status" == 0 && -f "${IDF_PATH:-}/tools/idf.py" ]] || {
    echo 'ESP-IDF activation failed.' >&2; exit 1;
}
[[ "$(git -C "$IDF_PATH" rev-parse HEAD)" == "$revision" &&
   "$(git -C "$IDF_PATH" remote get-url origin)" == "$repository" ]] || {
    echo 'Activated IDF does not match the requested locked fixture profile.' >&2; exit 1;
}
build_root=${ESP32S3_IDF_BUILD_ROOT:-$cache/builds}
mkdir -p "$build_root"
for native_path in "$IDF_PATH" "$build_root" "$IDF_TOOLS_PATH" "$IDF_PYTHON_ENV_PATH"; do
    case "$(stat -f -c %T "$native_path")" in ext2/ext3|ext4|btrfs|xfs|overlayfs|tmpfs) ;;
        *) echo "SDK source, tools and build output must reside on native Linux disk: $native_path" >&2; exit 1 ;;
    esac
done
# Content-addressed inputs keep frozen repository build images/configs untouched.
# Only these two fixtures' authored inputs are copied; generated sdkconfig is not.
project=$(python3 - "$root/tests/firmware/$fixture" "$build_root" "$fixture" <<'PY'
import hashlib, json, pathlib, shutil, sys
original, build_root, fixture = sys.argv[1:]
original, build_root = pathlib.Path(original), pathlib.Path(build_root).resolve()
inputs = [original / "CMakeLists.txt", original / "sdkconfig.defaults"]
inputs += sorted(p for p in (original / "main").rglob("*") if p.is_file())
files = {str(p.relative_to(original)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
source_hash = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
project = build_root / fixture / source_hash / "source"
manifest = project.parent / "source-manifest.json"
if not manifest.exists():
    project.mkdir(parents=True, exist_ok=True)
    for path in inputs:
        target = project / path.relative_to(original)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    manifest.write_text(json.dumps({"original_source": str(original), "source_sha256": source_hash,
                                   "source_files": files}, indent=2) + "\n")
for name, expected in files.items():
    if hashlib.sha256((project / name).read_bytes()).hexdigest() != expected:
        raise SystemExit("Native fixture snapshot changed; refusing to overwrite it")
print(project)
PY
)
cd "$project"
build_dir="$project/../build"
python "$IDF_PATH/tools/idf.py" -B "$build_dir" -DIDF_TARGET=esp32s3 \
    "-DSDKCONFIG=$build_dir/sdkconfig" "-DSDKCONFIG_DEFAULTS=$project/sdkconfig.defaults" build
(cd "$build_dir" && python -m esptool --chip esp32s3 merge_bin --fill-flash-size 4MB -o "$image" @flash_args)
export ESP32S3_BOOT_FIRMWARE="$build_dir/$image"
export ESP32S3_I2C_FIRMWARE="$build_dir/$image"
export ESP32S3_QEMU_BIN="${ESP32S3_QEMU_BIN:-$(command -v qemu-system-xtensa)}"
python3 "$root/tools/runtime-manifest.py" --qemu "$ESP32S3_QEMU_BIN" --idf "$IDF_PATH" \
    --firmware "$build_dir/$image" --firmware "$build_dir/esp32s3_$fixture.elf" \
    --output "$build_dir/runtime-manifest.json"
printf 'Firmware image: %s\nSource manifest: %s\n' "$build_dir/$image" "$project/../source-manifest.json"
"$root/gui-esp32s3-simulator/build-wsl/$test_binary"
