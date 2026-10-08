#!/usr/bin/env bash
# Build only. Never opens a serial port or flashes hardware.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fixture=${1:-simd_reference}
radio_mode=${2:-wifi}
case "$fixture" in simd_reference|radio_init|all) ;; *) echo "Expected simd_reference, radio_init or all" >&2; exit 2 ;; esac
case "$radio_mode" in wifi|ble|both) ;; *) echo "Radio mode must be wifi, ble or both" >&2; exit 2 ;; esac

activate_eim() {
    while IFS= read -r assignment; do
        key=${assignment%%=*}
        value=${assignment#*=}
        if [[ "$key" =~ ^[A-Z_][A-Z_0-9]*$ ]]; then
            if [[ "$key" == PATH ]]; then value="$value:$PATH"; fi
            export "$key=$value"
        fi
    done < <(bash "$1" -e)
}
set +eu
if [[ -n "${ESP32S3_IDF_ENV:-}" ]]; then
    if [[ "$ESP32S3_IDF_ENV" == *activate_idf* ]]; then
        activate_eim "$ESP32S3_IDF_ENV"
    else
        source "$ESP32S3_IDF_ENV"
    fi
elif [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
    source "$IDF_PATH/export.sh"
elif [[ -f "$HOME/.espressif/tools/activate_idf_master.sh" ]]; then
    activate_eim "$HOME/.espressif/tools/activate_idf_master.sh"
fi
set -eu
[[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/tools/idf.py" ]] || {
    echo "Provide an ESP32-S3 ESP-IDF environment through ESP32S3_IDF_ENV or IDF_PATH" >&2; exit 2;
}

build_one() {
    local name=$1
    local project="$root/tests/firmware/$name"
    local build="$project/build"
    local count=84
    local mode=none
    if [[ "$name" == radio_init ]]; then
        build="$project/build/$radio_mode"
        mode=$radio_mode
        case "$mode" in wifi) count=16 ;; ble) count=18 ;; both) count=32 ;; esac
    fi
    mkdir -p -- "$build"
    (
        cd -- "$project"
        python "$IDF_PATH/tools/idf.py" -B "$build" -DIDF_TARGET=esp32s3 \
            -DSDKCONFIG="$build/sdkconfig" -DREFERENCE_RADIO_MODE="$mode" reconfigure build
        cd -- "$build"
        python -m esptool --chip esp32s3 merge-bin --fill-flash-size 4MB \
            -o "$name.merged.bin" @flash_args
        python - "$root" "$name" "$mode" "$count" "$build" <<'PY'
import hashlib, json, os, pathlib, subprocess, sys
root, name, mode, count, build = sys.argv[1:]
root, build = pathlib.Path(root), pathlib.Path(build)
source = root / "tests/firmware" / name
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
source_paths = [source / "CMakeLists.txt", source / "sdkconfig.defaults"]
source_paths += [path for path in (source / "main").rglob("*") if path.is_file()]
sources = {str(path.relative_to(root)): sha(path) for path in source_paths}
elf = build / ("esp32s3_" + name + ".elf")
sdkconfig = (build / "sdkconfig").read_text()
compiler = json.loads((build / "project_description.json").read_text())["c_compiler"]
idf = pathlib.Path(os.environ["IDF_PATH"])
vendor = {}
for component in ["esp_wifi", "esp_phy", "bt"]:
    for archive in (idf / "components" / component).rglob("*.a"):
        if "esp32s3" in archive.parts:
            vendor[str(archive.relative_to(idf))] = sha(archive)
manifest = {
    "schema_version": 1, "fixture": name, "mode": mode,
    "capture_grace_ms": 15000,
    "expected_records": int(count), "source_sha256": sha(source / "main" / (name + ".c")),
    "source_files": sources, "idf_path": os.environ["IDF_PATH"],
    "idf_commit": subprocess.check_output(["git", "-C", os.environ["IDF_PATH"], "rev-parse", "HEAD"], text=True).strip(),
    "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
    "vendor_libraries": vendor,
    "console": {"primary": "uart0", "baud": 115200,
                "secondary_usb_serial_jtag": "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y" in sdkconfig,
                "hardware_connection": "not tested; select the port carrying this console explicitly"},
    "artifacts": {"elf_sha256": sha(elf), "flash_sha256": sha(build / (name + ".merged.bin")),
                  "sdkconfig_sha256": sha(build / "sdkconfig")},
    "board": "ESP32-S3 profile; actual hardware identity supplied during acquisition",
    "nvs": "merged image starts erased; QEMU runner copies it per run",
    "hardware_flash": "not performed; explicit authorization still required"
}
if name == "simd_reference":
    manifest["expected_cases"] = [f"op{op}-case{case}" for op in range(7) for case in range(12)]
else:
    manifest["expected_stages"] = ["nvs_flash_init"]
    if mode in {"wifi", "both"}:
        manifest["expected_stages"] += ["esp_netif_init", "esp_event_loop_create_default", "esp_wifi_init",
                                       "esp_wifi_set_mode", "esp_wifi_start", "esp_wifi_stop", "esp_wifi_deinit"]
    if mode in {"ble", "both"}:
        manifest["expected_stages"] += ["esp_bt_controller_init", "esp_bt_controller_enable", "esp_bluedroid_init",
                                       "esp_bluedroid_enable", "esp_bluedroid_disable", "esp_bluedroid_deinit",
                                       "esp_bt_controller_disable", "esp_bt_controller_deinit"]
(build / "reference-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("Reference manifest: " + str(build / "reference-manifest.json"))
PY
    )
}
if [[ "$fixture" == all ]]; then
    build_one simd_reference
    build_one radio_init
else
    build_one "$fixture"
fi
