#!/usr/bin/env python3
"""Collect real native external RGB/CAM consumers; never build or inject payloads.

Run in the activated native SDK Python environment (ruamel.yaml is supplied by
Component Manager). Each invocation retains one fresh writable flash and all
QMP/HMP requests, raw captures, pause boundaries, UART and failure evidence.
Only the independent consumer oracle determines the qualification scope.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import traceback

sys.dont_write_bytecode = True
LANE = pathlib.Path(__file__).resolve().parent
ROOT = LANE.parents[2]
SDK_LOCKS = {
    "idf-5.5.5": "b774170ff46c393eeb5e495ea37936038d3f4f4f",
    "idf-6.1": "fff9895c82d744c7237be8847347bdd1b07c6643",
}
PROFILES = {name: name.removeprefix("lcd-") for name in (
    "lcd-rgb-double", "lcd-rgb-bounce", "lcd-camera-rgb565",
    "lcd-camera-yuv422", "lcd-camera-jpeg")}
ELECTRICAL = "/machine/soc/electrical"
CONTROLLER = "/machine/soc/lcd_cam"
PANELS = "/machine/soc/lcd-panels"
SENSORS = "/machine/soc/ov2640-sensors"
JPEG_SHA = "6c15edd7f3b74f37d02ab4e74f13276aff4bd0f63dd9b6031bf6ebba5e020c13"
JPEG_COMMIT = "746e83ddbea0db9c3d24993a87c4c737a60337ae"
CAMERA_COMMIT = "b3c2aa273a88c7debf2b1683448933ca2b28aad9"
END = "LCDCAM END inspect_API_errors_and_independent_electrical_capture"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def append(path, value):
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, separators=(",", ":")) + "\n")


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, f"Cannot import {path}")
    value = importlib.util.module_from_spec(spec)
    sys.modules[name] = value
    spec.loader.exec_module(value)
    return value


class Provenance:
    def __init__(self, evidence):
        self.evidence, self.hashes = evidence, {}

    def pin(self, path, expected=None):
        path = pathlib.Path(path).resolve(strict=True)
        actual = digest(path)
        require(expected is None or actual == expected, f"Input hash differs: {path}")
        self.hashes[str(path)] = actual
        return path

    def command(self, command):
        started = time.monotonic_ns()
        result = subprocess.run(command, capture_output=True, timeout=120)
        append(self.evidence / "provenance-commands.jsonl", dict(
            command=command, start_host_monotonic_ns=started,
            end_host_monotonic_ns=time.monotonic_ns(), returncode=result.returncode,
            stdout=result.stdout.decode(errors="replace"), stderr=result.stderr.decode(errors="replace")))
        require(result.returncode == 0, f"Provenance command failed: {command}")
        return result.stdout

    def git(self, source, *args):
        return self.command(["git", "-C", str(source), *args])

    def runtime(self, args):
        runtime, prepared = load(args.runtime_manifest), load(args.prepared_source_record)
        require(runtime["schema_version"] == prepared["schema_version"] == 1,
                "Unsupported runtime/prepared-source record schema")
        # This is the standard tools/prepare-qemu-runtime.py schema. The initial
        # memory-only prefix+delta record is deliberately not a substitute.
        require(all(key in prepared for key in ("source", "profile", "inputs", "applied_files", "fingerprint")),
                "Prerequisite: standard integrated source record; memory-only overlay is not accepted")
        require(pathlib.Path(prepared["source"]).resolve() == args.qemu_source,
                "Prepared source checkout differs from supplied checkout")
        profile = prepared["profile"]
        require(profile["base_commit"] == prepared["base_commit"], "Prepared base/profile mismatch")
        fingerprint = hashlib.sha256(json.dumps(dict(profile=profile, inputs=prepared["inputs"]),
            sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        require(fingerprint == prepared["fingerprint"], "Prepared profile/input fingerprint differs")
        required_targets = {"hw/misc/esp32s3_lcd_cam.c", "hw/misc/esp32s3_lcd_panel_service.c",
            "hw/misc/esp32s3_ov2640_service.c", "hw/misc/ssi_psram.c",
            "hw/misc/esp32s3_cache.c", "hw/misc/esp32s3_psram_cache.c", "hw/dma/esp_gdma.c"}
        require(required_targets <= prepared["applied_files"].keys(),
                "Prerequisite: binary/source profile must contain actual LCD, sensor, panel, GDMA and shared SSI/cache")
        targets = {item["destination"] for item in profile["copies"]}
        targets.update(target for item in profile["patches"] for target in item["targets"])
        require(targets == prepared["applied_files"].keys(), "Prepared target inventory differs from profile")
        for name, expected in prepared["applied_files"].items():
            self.pin(args.qemu_source / name, expected)
        for item in profile["copies"] + profile["patches"]:
            source = ROOT / "qemu-extensions" / item["source"]
            data = source.read_bytes()
            if "destination" in item:
                data = data.replace(b"\r\n", b"\n")
            require(hashlib.sha256(data).hexdigest() == prepared["inputs"][item["source"]],
                    f"Current profile input differs from prepared receipt: {source}")
            self.pin(source)
        qemu = runtime["qemu"]
        require(pathlib.Path(qemu["path"]).resolve() == args.qemu and qemu["machine"] == "esp32s3",
                "Runtime manifest executable/machine differs")
        self.pin(args.qemu, qemu["sha256"])
        require(args.qemu.is_relative_to(args.qemu_source), "Executable must be in its recorded prepared checkout")
        source = qemu["source"]
        require(pathlib.Path(source["path"]).resolve() == args.qemu_source and
                source["commit"] == prepared["base_commit"] == self.git(args.qemu_source, "rev-parse", "HEAD").decode().strip(),
                "Runtime source path/base/HEAD differ")
        tracked = self.git(args.qemu_source, "status", "--porcelain", "--untracked-files=no").decode().strip()
        require(tracked == source["tracked_changes"], "Actual tracked source state differs from runtime manifest")
        changes = self.git(args.qemu_source, "status", "--porcelain=v1", "--untracked-files=all", "-z",
                           "--", ".", ":(exclude)build-runtime")
        for entry in changes.split(b"\0"):
            if not entry:
                continue
            require(entry[:2] in (b" M", b"??") and entry[3:].decode() in targets,
                    f"Unexpected/staged/renamed source change: {entry!r}")
        names = self.git(args.qemu_source, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
        identity = {name: digest(args.qemu_source / name) for name in sorted(set(names.decode().split("\0")) - {""})
                    if (args.qemu_source / name).is_file()}
        save(self.evidence / "qemu-source-identity.json", dict(commit=source["commit"], file_sha256=identity,
            tracked_diff_sha256=hashlib.sha256(self.git(args.qemu_source, "diff", "--binary", "HEAD")).hexdigest()))
        for row in runtime["firmware"]:
            self.pin(pathlib.Path(row["path"]), row["sha256"])
        require(any(pathlib.Path(row["path"]).resolve() == args.merged_flash for row in runtime["firmware"]),
                "Runtime manifest must bind this exact merged firmware image")
        save(self.evidence / "runtime-manifest.json", runtime)
        save(self.evidence / "prepared-source-record.json", prepared)
        return dict(manifest=runtime, source_record=prepared)

    def firmware(self, args):
        from ruamel.yaml import YAML
        yaml = YAML(typ="safe")
        preparation = load(args.preparation_receipt)
        receipts = args.preparation_receipt.parent
        fixture_path = self.pin(receipts / "fixture-receipt.json", preparation["fixture_receipt_sha256"])
        package_path = self.pin(receipts / "package-receipt.json", preparation["component_package_receipt_sha256"])
        inventory_path = self.pin(receipts / "source-inventory.json", preparation["fixture_source_inventory_sha256"])
        fixture, package, inventory = load(fixture_path), load(package_path), load(inventory_path)
        require(fixture["source_files"] == inventory and fixture["source_inventory_sha256"] == digest(inventory_path),
                "Fixture snapshot inventory/receipt differs")
        sdk_meta = load(args.sdk_metadata)
        sdk_profile = sdk_meta["profile"]
        require(sdk_profile in SDK_LOCKS and sdk_meta["commit"] == SDK_LOCKS[sdk_profile] and
                sdk_meta["target"] == "esp32s3", "SDK manifest does not name a supported locked S3 release")
        sdk = pathlib.Path(sdk_meta["source"]).resolve(strict=True)
        require(self.git(sdk, "rev-parse", "HEAD").decode().strip() == SDK_LOCKS[sdk_profile], "Actual SDK HEAD differs")
        require(not self.git(sdk, "status", "--porcelain", "--untracked-files=no"), "Actual SDK tracked/submodule source is not clean")
        self.pin(sdk / "tools/tools.json", sdk_meta["tools_manifest_sha256"])
        for row in sdk_meta["submodule_revisions"]:
            require(row["commit"] == row["expected_commit"] == self.git(sdk / row["path"], "rev-parse", "HEAD").decode().strip(),
                    f"SDK submodule HEAD differs: {row['path']}")
        workspace = next(row for row in preparation["sdk_projects"] if row["sdk"] == sdk_profile)
        root = pathlib.Path(workspace["source_root"]).resolve(strict=True)
        require(args.native_fixture == pathlib.Path(workspace["project_path"]).resolve(), "Fixture is not the prepared SDK workspace")
        require(args.build_dir == pathlib.Path(workspace["generated_build_root"]).resolve() / args.profile and
                args.build_dir.is_relative_to(pathlib.Path(sdk_meta["build_root"]).resolve()), "Build path differs from canonical native SDK/profile root")
        snapshot = pathlib.Path(fixture["snapshot_root"]).resolve(strict=True) / "source"
        for row in inventory:
            for base in (root, snapshot):
                path = self.pin(base / row["path"], row["sha256"])
                require(path.stat().st_size == row["bytes"], f"Snapshot source length differs: {path}")
                require(not path.stat().st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH), f"Prepared source is writable: {path}")
        fs = json.loads(self.command(["findmnt", "--json", "--target", str(args.build_dir), "--output", "TARGET,SOURCE,FSTYPE,OPTIONS"]))
        require(fs["filesystems"][0]["fstype"] == "ext4", "Firmware build is not on the canonical native ext4 filesystem")
        description = self.pin(args.build_dir / "project_description.json")
        flashes_path = self.pin(args.build_dir / "flasher_args.json")
        compile_path = self.pin(args.build_dir / "compile_commands.json")
        cache_path = self.pin(args.build_dir / "CMakeCache.txt")
        desc, flashes, commands = load(description), load(flashes_path), load(compile_path)
        require(desc["project_name"] == "lcd_cam_native" and desc["target"] == "esp32s3", "Not ordinary compiled lcd_cam_native S3 firmware")
        require(pathlib.Path(desc["project_path"]).resolve() == args.native_fixture and
                pathlib.Path(desc["build_dir"]).resolve() == args.build_dir and pathlib.Path(desc["idf_path"]).resolve() == sdk,
                "Build description uses a different source/build/SDK")
        defaults = [args.native_fixture / "sdkconfig.defaults", root / "tests/firmware/memory_native" / f"sdkconfig.{args.profile}.defaults",
                    root / "tests/firmware/memory_native/sdkconfig.lcd-cam-quad.defaults"]
        require([pathlib.Path(path).resolve() for path in desc["config_defaults"].split(";")] == defaults, "Compiled configuration defaults/order differ")
        config = self.pin(pathlib.Path(desc["config_file"]))
        require(config.is_relative_to(args.build_dir), "Generated configuration must be outside the immutable fixture")
        cfg = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", config.read_text(), re.M))
        required = {"CONFIG_IDF_TARGET": '"esp32s3"', "CONFIG_LCD_CAM_NATIVE_PSRAM": "y", "CONFIG_SPIRAM": "y",
            "CONFIG_SPIRAM_MODE_QUAD": "y", "CONFIG_SPIRAM_SPEED_40M": "y", "CONFIG_SPIRAM_BOOT_INIT": "y",
            "CONFIG_SPIRAM_USE_CAPS_ALLOC": "y", "CONFIG_SPIRAM_MEMTEST": "y", "CONFIG_ESP32S3_DATA_CACHE_LINE_32B": "y"}
        for path in defaults[1:]:
            required.update(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", path.read_text(), re.M))
        require(all(cfg.get(key) == value for key, value in required.items()), "Build configuration is not the requested external quad40 profile")
        forbidden = ("CONFIG_SPIRAM_IGNORE_NOTFOUND", "CONFIG_SPIRAM_MODE_OCT", "CONFIG_SPIRAM_ECC_ENABLE",
                     "CONFIG_SPIRAM_FETCH_INSTRUCTIONS", "CONFIG_SPIRAM_RODATA")
        require(all(cfg.get(key) != "y" for key in forbidden), "Unqualified PSRAM fallback/octal/XIP/ECC configuration")
        if args.profile.startswith("lcd-camera-"):
            require(cfg.get("CONFIG_CAMERA_CONVERTER_ENABLED") != "y", "Camera input converter changes direct source bytes")
        if args.profile == "lcd-camera-jpeg":
            require(cfg.get("CONFIG_JD_USE_ROM") != "y", "JPEG must use the real pinned software decoder")
        elf, app = self.pin(args.build_dir / desc["app_elf"]), self.pin(args.build_dir / desc["app_bin"])
        app_data = app.read_bytes()
        require(len(app_data) >= 208 and struct.unpack_from("<I", app_data, 32)[0] == 0xabcd5432,
                "App lacks the ordinary IDF app descriptor")
        require(app_data[176:208].hex() == digest(elf), "App embedded ELF SHA256 differs")
        image = args.merged_flash.read_bytes()
        require(len(image) == 4 * 1024 * 1024, "Merged flash must have an actual 4MiB extent")
        merged = []
        for offset, filename in flashes["flash_files"].items():
            path = self.pin(args.build_dir / filename)
            data, start = path.read_bytes(), int(offset, 0)
            require(start + len(data) <= len(image) and image[start:start + len(data)] == data,
                    f"Merged flash differs from actual built input: {filename}")
            merged.append(dict(offset=start, path=str(path), bytes=len(data), sha256=digest(path)))
        require(any(pathlib.Path(row["path"]) == app for row in merged), "Merged inputs do not include the supplied compiled app")
        components = desc["build_component_info"]
        package_root = pathlib.Path(package["component_path"]).resolve(strict=True)
        require(package["archive_sha256"] == preparation["component_archive_sha256"] == JPEG_SHA and
                package["source_commit"] == JPEG_COMMIT and package["version"] == "1.3.1", "JPEG package is not the exact public pin")
        self.pin(pathlib.Path(package["archive_native"]), JPEG_SHA)
        for row in package["package_files"]:
            path = self.pin(package_root.parent / row["path"], row["sha256"])
            require(path.stat().st_size == row["bytes"], f"JPEG package length differs: {path}")
        camera_lock = load(self.pin(args.native_fixture / "component-lock.json"))
        require(camera_lock["commit"] == CAMERA_COMMIT and camera_lock["source_modified"] is False, "Camera component is not the real official pin")
        lock_path = self.pin(args.native_fixture / "dependencies.lock")
        with lock_path.open(encoding="utf-8") as stream:
            lock = yaml.load(stream)
        require(lock["target"] == "esp32s3" and re.fullmatch(r"[0-9a-f]{64}", lock["manifest_hash"]), "Missing real resolved S3 Component Manager lock")
        jpeg_rows = [(name, row) for name, row in lock["dependencies"].items() if name.split("/")[-1] == "esp_jpeg"]
        require(len(jpeg_rows) == 1 and jpeg_rows[0][1]["version"] == "1.3.1", "Resolved lock must contain exact esp_jpeg1.3.1")
        jpeg_dependency = jpeg_rows[0][1]
        require(jpeg_dependency["source"]["type"] == "local" and
                pathlib.Path(jpeg_dependency["source"]["path"]).resolve() == package_root,
                "Resolved JPEG dependency is not the prepared real local public package")
        jpeg_components = [(name, row) for name, row in components.items() if pathlib.Path(row["dir"]).resolve() == package_root]
        camera_components = [(name, row) for name, row in components.items() if pathlib.Path(row["dir"]).resolve() == args.native_fixture / "components/esp32-camera"]
        require(len(jpeg_components) == len(camera_components) == 1, "Real built camera/JPEG component paths missing or ambiguous")
        jpeg_name, jpeg_info = jpeg_components[0]
        camera_name, camera_info = camera_components[0]
        require(jpeg_name in camera_info["reqs"] and jpeg_name in camera_info["managed_reqs"],
                "Actual public Component Manager camera->JPEG REQUIRES edge missing")
        with (args.native_fixture / "components/esp32-camera/idf_component.yml").open(encoding="utf-8") as stream:
            camera_manifest = yaml.load(stream)
        require(camera_manifest["dependencies"]["esp_jpeg"] == {"version": "^1.3.1", "public": True},
                "Real camera manifest public dependency differs")
        compiled = {}
        command_files = set()
        for row in commands:
            path = pathlib.Path(row["file"])
            if not path.is_absolute():
                path = pathlib.Path(row["directory"]) / path
            path = self.pin(path)
            command_files.add(path)
            compiled[str(path)] = dict(sha256=digest(path), compile_command=row)
        source_paths = set()
        for info in components.values():
            for name in info.get("sources", []):
                path = self.pin(pathlib.Path(name))
                source_paths.add(path)
                require(path.stat().st_mtime_ns <= elf.stat().st_mtime_ns, f"Compiled source newer than ELF: {path}")
        main = args.native_fixture / "main/lcd_cam_native.c"
        require(main in command_files and main in source_paths, "Real consumer source absent from compiled command/component inventories")
        require(all(pathlib.Path(name).resolve() in command_files for name in camera_info["sources"] + jpeg_info["sources"]),
                "Camera/JPEG source absent from real compiler commands")
        save(self.evidence / "compiled-source-identity.json", compiled)
        for path in (description, flashes_path, compile_path, config, cache_path, lock_path, fixture_path, package_path, inventory_path):
            shutil.copyfile(path, self.evidence / path.name)
        save(self.evidence / "sdk-metadata.json", sdk_meta)
        return dict(sdk=sdk_meta, native_filesystem=fs, source_inventory_sha256=digest(inventory_path),
            description=desc, configuration=cfg, merged_inputs=merged, resolved_dependency_lock=lock,
            jpeg_component=jpeg_info, camera_component=camera_info, compiled_source_identity="compiled-source-identity.json")

    def memory_foundation(self, args):
        """Bind new receipt references to actual TAP and ordinary SDK evidence."""
        receipt = load(args.memory_qualification_receipt)
        require(receipt["schema_version"] == 1 and receipt["status"] == "PASS",
                "Prerequisite: passing renewed same-binary memory qualification receipt")
        for key, path in (("qemu", args.qemu), ("prepared_source_record", args.prepared_source_record),
                          ("runtime_manifest", args.runtime_manifest)):
            row = receipt[key]
            require(pathlib.Path(row["path"]).resolve() == path and row["sha256"] == digest(path),
                    f"Prerequisite: memory foundation used a different final {key}")
            self.pin(path, row["sha256"])
        prepared = load(args.prepared_source_record)
        qtest = receipt["qtest"]
        require(qtest["returncode"] == 0 and
                pathlib.Path(qtest["environment"]["QTEST_QEMU_BINARY"]).resolve() == args.qemu,
                "Memory QTest did not execute this final binary successfully")
        test_binary = self.pin(pathlib.Path(qtest["command"][0]), qtest["binary_sha256"])
        require(test_binary.is_relative_to(args.qemu_source) and test_binary.name == "esp32s3-memory-test",
                "Memory QTest executable is not in the final prepared checkout")
        self.pin(args.qemu_source / "tests/qtest/esp32s3-memory-test.c", qtest["source_sha256"])
        expected = {"/esp32s3-memory/" + name for name in (
            "visibility", "alias-remap", "boundaries", "absent", "alignment", "reset",
            "spi1-ssi", "spi1-high-duplex", "spi1-asymmetric", "spi0-phases",
            "spi0-clock-reset", "cache-geometry")}
        for key in ("stdout", "stderr"):
            self.pin(pathlib.Path(qtest[key]["path"]), qtest[key]["sha256"])
        tap = pathlib.Path(qtest["stdout"]["path"]).read_text(errors="replace")
        require(re.search(r"^1\.\.12$", tap, re.M) and
                not re.search(r"^(?:not ok|Bail out!)|^ok\b.*#\s*(?:SKIP|TODO)\b", tap, re.M | re.I),
                "Actual memory TAP must pass all12 without skip/TODO/bailout")
        rows = re.findall(r"^ok\s+(\d+)\s+(\S+)(?:\s.*)?$", tap, re.M)
        names = set()
        for _, name in rows:
            parts = [part for part in name.split("/") if part]
            require(parts and parts[0] == "xtensa", "Actual memory TAP architecture is not Xtensa")
            names.add("/" + "/".join(parts[1:]))
        require(len(rows) == 12 and names == expected and sorted(int(row[0]) for row in rows) == list(range(1, 13)),
                "Actual memory TAP names/count differ from full approved12-test inventory")
        helper_path = self.pin(LANE / "run-native-fixture.py")
        helper = module(helper_path, "memory_native_foundation_uart")
        consumers = receipt["sdk_consumers"]
        require(len(consumers) == 4 and {(row["idf_profile"], row["mode"]) for row in consumers} ==
                {(sdk, mode) for sdk in SDK_LOCKS for mode in ("internal", "psram")},
                "Prerequisite: renewed actual internal/PSRAM data evidence from both locked SDKs")
        checked = []
        for consumer in consumers:
            sdk, mode = consumer["idf_profile"], consumer["mode"]
            pin = consumer["result"]
            result_path = self.pin(pathlib.Path(pin["path"]), pin["sha256"])
            proof = load(result_path)
            require(proof["status"] == "PASS" and proof["mode"] == mode and proof["idf_profile"] == sdk and
                    proof["original_flash_unchanged"] is True, "Actual ordinary memory SDK run did not pass")
            provenance = proof["provenance"]
            require(provenance["sdk"]["profile"] == sdk and provenance["sdk"]["commit"] == SDK_LOCKS[sdk] and
                    provenance["artifacts"].get(str(args.qemu)) == digest(args.qemu),
                    "Ordinary memory data proof used a different SDK or final QEMU binary")
            identity_path = self.pin(result_path.parent / "qemu-source-identity.json",
                                     provenance["qemu_source_identity_sha256"])
            identity = load(identity_path)
            require(identity["commit"] == prepared["base_commit"] and
                    all(identity["file_sha256"].get(name) == expected for name, expected in prepared["applied_files"].items()),
                    "Ordinary memory data proof borrowed a different prepared prefix/target set")
            require(consumer["uart_logs"], "Missing actual ordinary memory UART data")
            logs = []
            for uart in consumer["uart_logs"]:
                path = self.pin(pathlib.Path(uart["path"]), uart["sha256"])
                require(proof["evidence_sha256"].get(path.name) == uart["sha256"],
                        "Memory UART is not retained by the actual ordinary run result")
                logs.append(helper.validate_uart(path.read_text(errors="replace"), mode, sdk))
            require(all(command[0] == str(args.qemu) and "snapshot=on" not in " ".join(command)
                        for command in proof["commands"]), "Memory data proof did not use this final executable/writable flash")
            checked.append(dict(idf_profile=sdk, mode=mode, result_sha256=pin["sha256"], observed_uart=logs))
        controls = receipt["spi1_controls"]
        require(len(controls) == 4 and {(row["phase"], row["test"]) for row in controls} ==
                {(phase, test) for phase in ("before", "after") for test in ("spi1-high-duplex", "spi1-asymmetric")},
                "Prerequisite: actual before-fix failures and after-fix SPI1 controls")
        for control in controls:
            binary = self.pin(pathlib.Path(control["binary"]["path"]), control["binary"]["sha256"])
            require(pathlib.Path(control["environment"]["QTEST_QEMU_BINARY"]).resolve() == binary,
                    "SPI1 control environment differs from recorded actual binary")
            test_executable = self.pin(pathlib.Path(control["command"][0]), control["test_binary_sha256"])
            require(test_executable.name == "esp32s3-memory-test" and "-p" in control["command"] and
                    control["test"] in " ".join(control["command"]), "SPI1 control command omitted its actual selected test")
            logs = []
            for stream in ("stdout", "stderr"):
                path = self.pin(pathlib.Path(control[stream]["path"]), control[stream]["sha256"])
                logs.append(path.read_text(errors="replace"))
            diagnostic = "\n".join(logs)
            if control["phase"] == "after":
                require(binary == args.qemu and control["returncode"] == 0 and
                        re.search(r"^ok\s+\d+\s+\S*" + re.escape(control["test"]) + r"(?:\s|$)", diagnostic, re.M) and
                        not re.search(r"^(?:not ok|Bail out!)|#\s*(?:SKIP|TODO)\b", diagnostic, re.M | re.I),
                        "Final-binary SPI1 control did not actually pass")
            else:
                require(binary != args.qemu and control["returncode"] != 0 and
                        re.search(r"not ok|Bail out!|assertion.*failed|ERROR:", diagnostic, re.I),
                        "Before-fix SPI1 control lacks an actual failing observation")
        boots = receipt["boot_control"]
        require(len(boots) == 3 and {row["profile"] for row in boots} ==
                {"idf-5.5.5", "idf-6.1", "arduino-idf-5.5.5"}, "Prerequisite: three actual frozen boot controls")
        for boot_reference in boots:
            boot_path = self.pin(pathlib.Path(boot_reference["result_path"]), boot_reference["result_sha256"])
            boot = load(boot_path)
            require(boot_reference["status"] == boot["status"] == "PASS" and
                    boot_reference["profile"] == boot["profile"] and boot["returncode"] == 0 and
                    pathlib.Path(boot["environment"]["ESP32S3_QEMU_BIN"]).resolve() == args.qemu,
                    "Frozen boot control did not pass on this final executable")
            executable = self.pin(pathlib.Path(boot["command"][0]), boot["test_binary_sha256"])
            require(executable.name == "gui_esp32s3_boot_smoke_test", "Boot control does not reference the actual frozen smoke executable")
            firmware = self.pin(pathlib.Path(boot["firmware"]["path"]), boot["firmware"]["sha256"])
            require(pathlib.Path(boot["environment"]["ESP32S3_BOOT_FIRMWARE"]).resolve() == firmware and
                    boot["original_after_sha256"] == boot["firmware"]["sha256"], "Frozen boot control original image changed")
            logs = []
            for stream in ("stdout", "stderr"):
                path = self.pin(pathlib.Path(boot[stream]["path"]), boot[stream]["sha256"])
                logs.append(path.read_text(errors="replace"))
            diagnostic = "\n".join(logs)
            require(re.search(r"PASS\s*:\s*BootSmokeTest::bootsRealFirmware\(\)", diagnostic) and
                    not re.search(r"(?:FAIL!|SKIP)\s*:", diagnostic), "Actual boot smoke output did not pass without skip")
        save(self.evidence / "memory-qualification-receipt.json", receipt)
        return dict(receipt_sha256=digest(args.memory_qualification_receipt),
                    approved_memory_qtest_count=12, sdk_consumers=checked, same_final_binary=True,
                    same_prepared_targets=True, spi1_before_after_control_count=4, frozen_boot_control_count=3)


class Qmp:
    def __init__(self, sock, transcript, deadline):
        self.sock, self.transcript, self.deadline, self.sequence = sock, transcript, deadline, 0
        self.stream = sock.makefile("rb")
        require("QMP" in self.receive(), "Missing actual QMP greeting")
        self.call("qmp_capabilities")

    def receive(self):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, "Diagnostic host watchdog expired during QMP; no guest timeout qualification")
        self.sock.settimeout(remaining)
        line = self.stream.readline()
        require(line, "QMP disconnected")
        value = json.loads(line)
        append(self.transcript, dict(host_monotonic_ns=time.monotonic_ns(), direction="receive", message=value))
        return value

    def call(self, name, arguments=None):
        self.sequence += 1
        request = dict(execute=name, arguments=arguments or {}, id=self.sequence)
        append(self.transcript, dict(host_monotonic_ns=time.monotonic_ns(), direction="send", message=request))
        self.sock.sendall(json.dumps(request).encode() + b"\r\n")
        while True:
            reply = self.receive()
            if "event" in reply:
                continue
            require(reply.get("id") == self.sequence, "Unmatched QMP response")
            require("return" in reply, f"Actual QMP command failed: {reply}")
            return reply["return"]

    def get(self, path, prop):
        value = self.call("qom-get", dict(path=path, property=prop))
        require(isinstance(value, str), f"Public {path} {prop} must return JSON text")
        return json.loads(value)

    def set(self, path, prop, value):
        self.call("qom-set", dict(path=path, property=prop, value=json.dumps(value, separators=(",", ":"))))

    def close(self):
        self.stream.close()
        self.sock.close()


def physical_words(qmp, address, count):
    """Read only explicitly safe registers or internal descriptor words via HMP."""
    text = qmp.call("human-monitor-command", dict(command_line=f"xp /{count}wx 0x{address:x}"))
    words = []
    for line in text.splitlines():
        match = re.fullmatch(r"\s*(?:0x)?([0-9a-fA-F]+):\s*((?:0x[0-9a-fA-F]+\s*)+)\s*", line)
        require(match is not None, f"Unexpected actual HMP xp response: {text!r}")
        require(int(match[1], 16) == address + len(words) * 4, "HMP physical read address/order differs")
        words.extend(int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", match[2]))
    require(len(words) == count, "HMP physical read length differs")
    return words


def gdma_snapshot(qmp, walk_descriptors=True):
    # Canonical esp32s3_gdma.h: five channels, IN/OUT 0x60 each,
    # CONF0/CONF1/RAW 0x00..08; LINK..PERI_SEL 0x20..48. Never
    # read POP/PUSH (0x1c), never write W1C, links or cached payloads.
    channels = []
    names = ("link", "state", "suc_eof_desc", "err_eof_desc", "desc", "bf0_desc", "bf1_desc")
    for channel in range(5):
        for direction in ("IN", "OUT"):
            base = 0x6003f000 + channel * 0xc0 + (direction == "OUT") * 0x60
            conf0, conf1, raw = physical_words(qmp, base, 3)
            registers = dict(zip(names, physical_words(qmp, base + 0x20, 7)))
            registers["peri_sel"] = physical_words(qmp, base + 0x48, 1)[0]
            registers.update(conf0=conf0, conf1=conf1, int_raw=raw)
            item = dict(channel=channel, direction=direction, base=base, registers=registers, descriptors=[])
            link = registers["link"]
            item.update(park=bool(link & (1 << (24 if direction == "IN" else 23))),
                        stop=bool(link & (1 << (21 if direction == "IN" else 20))),
                        peripheral=registers["peri_sel"] & 63)
            if item["peripheral"] == 5 and walk_descriptors:
                roots = [registers[name] for name in ("bf0_desc", "desc", "suc_eof_desc", "bf1_desc") if registers[name]]
                if link & 0xfffff:
                    roots.append(0x3fc00000 | (link & 0xfffff))
                seen = set()
                for root in roots:
                    address = root
                    while address and address not in seen:
                        require(len(seen) < 1024, "Actual GDMA descriptor walk exceeds finite 1024-node bound")
                        require(0x3fc80000 <= address <= 0x3fd00000 - 12 and address % 4 == 0,
                                f"Actual peri5 descriptor is not internal aligned DRAM: 0x{address:x}")
                        seen.add(address)
                        dw0, dw1, dw2 = physical_words(qmp, address, 3)
                        item["descriptors"].append(dict(address=address, words=[dw0, dw1, dw2], size=dw0 & 4095,
                            length=(dw0 >> 12) & 4095, owner=bool(dw0 & 0x80000000), eof=bool(dw0 & 0x40000000),
                            buffer=dw1, next=dw2))
                        address = dw2
                item["roots"] = roots
            channels.append(item)
    return dict(kind="actual-read-only-hmp-gdma", channels=channels, payload_reads=False,
                fifo_pop_reads=False, register_writes=False)


class Collector:
    def __init__(self, qmp, evidence, result, camera):
        self.qmp, self.evidence, self.result, self.camera = qmp, evidence, result, camera
        self.panel_frames, self.sensor_captures, self.controller_snapshots = [], [], []
        self.panel_next, self.sensor_last, self.stops = 0, 0, 0
        self.active_seen = False

    def status(self):
        return dict(controller=self.qmp.get(CONTROLLER, "status-json"),
                    panels=self.qmp.get(PANELS, "status-json"), sensors=self.qmp.get(SENSORS, "status-json"))

    def observe(self, phase, text, physical=False):
        status = self.status()
        snapshot = dict(status["controller"])
        snapshot["_host_observation"] = dict(phase=phase, host_monotonic_ns=time.monotonic_ns(),
            uart_prefix=text, uart_bytes=len(text.encode()), paused=not self.qmp.call("query-status")["running"])
        if physical:
            deleted = re.search(r"^LCDCAM API op=(?:rgb_delete|camera_external_deinit) err=ESP_OK code=0$", text, re.M)
            snapshot["_host_observation"]["gdma"] = gdma_snapshot(self.qmp, not deleted and END not in text)
            direction = "IN" if self.camera else "OUT"
            self.active_seen |= any(row["direction"] == direction and row["peripheral"] == 5 and not row["park"]
                                    for row in snapshot["_host_observation"]["gdma"]["channels"])
        self.controller_snapshots.append(snapshot)
        append(self.evidence / "status-snapshots.jsonl", dict(phase=phase, host_monotonic_ns=time.monotonic_ns(), **status,
            host_observation=snapshot["_host_observation"]))
        return status

    def panel(self, status):
        panels = status["panels"]["panels"]
        require(len(panels) == 1 and panels[0]["component_id"] == "D", "Applied real panel endpoint missing/ambiguous")
        panel = panels[0]
        total = panel["captures"]
        if total <= self.panel_next:
            return
        require(panel["first_retained_sequence"] <= self.panel_next, "Actual panel metadata ring evicted uncollected frames")
        captured = []
        for offset in range(self.panel_next, total, 64):
            self.qmp.set(PANELS, "capture-request-json", dict(componentId="D", offset=offset, count=min(64, total-offset)))
            capture = self.qmp.get(PANELS, "capture-json")
            save(self.evidence / f"panel-history-{offset:06d}.json", capture)
            require(capture["total_at_request"] == total and [row["sequence"] for row in capture["frames"]] == list(range(offset, min(total, offset+64))),
                    "Actual immutable panel metadata history differs")
            captured.append(capture)
        sequence, data, windows = total-1, bytearray(), []
        length = panel["width"] * panel["height"] * 3
        for offset in range(0, length, 65536):
            self.qmp.set(PANELS, "framebuffer-request-json", dict(componentId="D", sequence=sequence, offset=offset, count=min(65536, length-offset)))
            window = self.qmp.get(PANELS, "framebuffer-json")
            save(self.evidence / f"panel-{sequence:06d}-window-{offset:06d}.json", window)
            require(window["sequence"] == sequence and window["offset"] == offset and window["count"] == min(65536, length-offset), "Panel byte window identity/extent differs")
            chunk = bytes.fromhex(window["hex"])
            require(len(chunk) == window["count"], "Actual panel hex length differs")
            windows.append(window)
            data.extend(chunk)
        metadata = windows[0]
        identity = {key: value for key, value in metadata.items() if key not in ("hex", "offset", "count")}
        require(all({key: value for key, value in row.items() if key not in ("hex", "offset", "count")} == identity for row in windows), "Panel immutable framebuffer lease changed")
        require(len(data) == length == metadata["total_bytes"] and hashlib.sha256(data).hexdigest() == metadata["sha256_rgb888"], "Actual full panel byte capture/hash differs")
        binary = f"panel-{sequence:06d}.rgb888"
        (self.evidence / binary).write_bytes(data)
        self.panel_frames.append(dict(metadata=metadata, rgb888=bytes(data), windows=windows, capture=captured[-1]))
        self.result["panel_frames"].append(dict(sequence=sequence, metadata_file=f"panel-{sequence:06d}-window-000000.json", binary=binary,
            unavailable_byte_sequences=list(range(self.panel_next, sequence)), metadata_only_not_full_capture=True))
        self.panel_next = total

    def sensor(self, status):
        require(len(status["sensors"]) == 1 and status["sensors"][0]["component_id"] == "D", "Applied real sensor endpoint missing/ambiguous")
        sensor = status["sensors"][0]
        frame = sensor["capture_frame"]
        if frame <= self.sensor_last or not sensor["capture_byte_count"]:
            return
        self.qmp.set(SENSORS, "capture-request-json", dict(component_id="D", frame=frame, offset=0, count=0))
        metadata = self.qmp.get(SENSORS, "capture-json")
        save(self.evidence / f"sensor-{frame:06d}-metadata.json", metadata)
        length = metadata["byte_count"]
        data, windows = bytearray(), []
        excluded = ("offset", "count", "bytes", "timestamps_ns")
        identity = {key: value for key, value in metadata.items() if key not in excluded}
        for offset in range(0, length, 4096):
            count = min(4096, length-offset)
            self.qmp.set(SENSORS, "capture-request-json", dict(component_id="D", frame=frame, offset=offset, count=count))
            window = self.qmp.get(SENSORS, "capture-json")
            save(self.evidence / f"sensor-{frame:06d}-window-{offset:06d}.json", window)
            require({key: value for key, value in window.items() if key not in excluded} == identity and
                    window["offset"] == offset and window["count"] == count, "Actual sensor current-frame lease changed while stopped")
            require(len(window["bytes"]) == len(window["timestamps_ns"]) == count and
                    all(type(value) is int and 0 <= value <= 255 for value in window["bytes"]), "Actual sensor sample window extent/type differs")
            windows.append(window)
            data.extend(window["bytes"])
        require(len(data) == length, "Incomplete actual sensor source capture")
        binary = f"sensor-{frame:06d}.bin"
        (self.evidence / binary).write_bytes(data)
        self.sensor_captures.append(dict(metadata=metadata, data=bytes(data), windows=windows, lease_status=sensor))
        self.result["sensor_captures"].append(dict(frame=frame, metadata_file=f"sensor-{frame:06d}-metadata.json", binary=binary,
            skipped_source_frames=list(range(self.sensor_last+1, frame)), source_only_not_cam_rx_proof=True))
        self.sensor_last = frame

    def freeze(self, reason, uart):
        self.stops += 1
        pause = dict(sequence=self.stops, reason=reason, stop_requested_host_monotonic_ns=time.monotonic_ns(),
                     boundary="host-observed completion/milestone, not exact silicon timing")
        self.result["pauses"].append(pause)
        append(self.evidence / "pause-boundaries.jsonl", dict(event="stop-request", **pause))
        self.qmp.call("stop")
        require(not self.qmp.call("query-status")["running"], "Actual QEMU did not stop for capture lease")
        pause["stopped_host_monotonic_ns"] = time.monotonic_ns()
        text = uart.read_text(errors="replace") if uart.exists() else ""
        (self.evidence / f"pause-{self.stops:04d}-uart.log").write_text(text, encoding="utf-8")
        status = self.observe(reason, text, physical=True)
        if self.camera:
            self.sensor(status)
        else:
            self.panel(status)
        append(self.evidence / "pause-boundaries.jsonl", dict(event="capture-complete", **pause))
        return pause, text

    def resume(self, pause):
        pause["cont_requested_host_monotonic_ns"] = time.monotonic_ns()
        self.qmp.call("cont")
        require(self.qmp.call("query-status")["running"], "Actual QEMU did not resume after lease collection")
        pause["resumed_host_monotonic_ns"] = time.monotonic_ns()
        append(self.evidence / "pause-boundaries.jsonl", dict(event="resume", **pause))


def discover(qmp, evidence):
    objects = qmp.call("qom-list", dict(path="/machine/soc"))
    save(evidence / "qom-soc-objects.json", objects)
    required = {ELECTRICAL: ("project-json", "snapshot-json"), CONTROLLER: ("status-json",),
        PANELS: ("status-json", "capture-request-json", "capture-json", "framebuffer-request-json", "framebuffer-json"),
        SENSORS: ("status-json", "capture-request-json", "capture-json")}
    for path, properties in required.items():
        require(path.rsplit("/", 1)[1] in {row["name"] for row in objects},
                f"Prerequisite: actual integrated QEMU public object absent: {path}; no memory-only fallback")
        rows = qmp.call("qom-list", dict(path=path))
        save(evidence / f"qom-{path.rsplit('/', 1)[1]}-properties.json", rows)
        require(set(properties) <= {row["name"] for row in rows}, f"Prerequisite: missing actual public capture properties on {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    names = ("qemu", "qemu-source", "runtime-manifest", "prepared-source-record", "native-fixture",
             "build-dir", "sdk-metadata", "merged-flash", "preparation-receipt", "memory-qualification-receipt", "evidence")
    for name in names:
        parser.add_argument(f"--{name}", required=True, type=pathlib.Path)
    parser.add_argument("--profile", required=True, choices=PROFILES)
    parser.add_argument("--watchdog-seconds", type=float, default=300, help="Diagnostic host bound only; no retries or guest timeout PASS")
    args = parser.parse_args()
    if not 1 <= args.watchdog_seconds <= 1800:
        parser.error("--watchdog-seconds must be 1..1800")
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f"{args.profile}-{time.strftime('%Y%m%dT%H%M%S')}-", dir=args.evidence.resolve()))
    result = dict(schema_version=1, status="FAIL", profile=args.profile, evidence=str(evidence),
        commands=[], pauses=[], panel_frames=[], sensor_captures=[], host_watchdog_seconds=args.watchdog_seconds,
        hardware_qualified=False, injected_payloads=False, persistence_qualified=False)
    proc = qmp = collector = None
    provenance = Provenance(evidence)
    original_hash = None
    started = time.monotonic()
    scratch, uart = evidence / "writable-flash.bin", evidence / "uart.log"
    try:
        for name in names[:-1]:
            key = name.replace("-", "_")
            setattr(args, key, getattr(args, key).resolve(strict=True))
        for key in ("qemu", "runtime_manifest", "prepared_source_record", "sdk_metadata", "merged_flash",
                    "preparation_receipt", "memory_qualification_receipt"):
            provenance.pin(getattr(args, key))
        original_hash = digest(args.merged_flash)
        result["original_flash_before_sha256"] = original_hash
        result["runtime_provenance"] = provenance.runtime(args)
        result["firmware_provenance"] = provenance.firmware(args)
        result["memory_foundation"] = provenance.memory_foundation(args)
        oracle = module(LANE / "lcd_cam_consumer_oracle.py", "lcd_cam_consumer_oracle")
        provenance.pin(LANE / "lcd_cam_consumer_oracle.py")
        provenance.pin(pathlib.Path(__file__))
        provenance.pin(LANE / "capture-runner-contract.json")
        provenance.pin(ROOT / "qemu-extensions/prototypes/lcd-cam/reference/capture_reference.py")
        provenance.pin(ROOT / "qemu-extensions/prototypes/lcd-cam/reference/provenance.json")
        graph = module(args.native_fixture / "graph_vectors.py", "lcd_cam_native_graph").project(PROFILES[args.profile])
        save(evidence / "project.json", graph)
        shutil.copyfile(args.merged_flash, scratch)
        scratch.chmod(0o600)
        require(digest(scratch) == original_hash, "Fresh writable flash differs from immutable original")
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        command = [str(args.qemu), "-machine", "esp32s3", "-nographic", "-S", "-monitor", "none", "-m", "8M",
            "-serial", f"file:{uart}", "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
            "-drive", f"file={str(scratch).replace(',', ',,')},if=mtd,format=raw,cache=writethrough",
            "-d", "guest_errors", "-D", str(evidence / "guest-errors.log")]
        result["commands"].append(dict(command=command, host_monotonic_ns=time.monotonic_ns()))
        save(evidence / "commands.json", result["commands"])
        deadline = time.monotonic() + args.watchdog_seconds
        with (evidence / "stdout.log").open("wb") as out, (evidence / "stderr.log").open("wb") as err:
            proc = subprocess.Popen(command, stdout=out, stderr=err)
            while True:
                require(proc.poll() is None, f"QEMU exited before QMP: {proc.returncode}; see stderr.log")
                require(time.monotonic() < deadline, "Diagnostic watchdog expired connecting initial QMP")
                try:
                    sock = socket.create_connection(("127.0.0.1", port), timeout=min(1, deadline-time.monotonic()))
                    break
                except ConnectionRefusedError:
                    time.sleep(.01)
            qmp = Qmp(sock, evidence / "qmp.jsonl", deadline)
            require(not qmp.call("query-status")["running"], "-S did not retain CPU before real graph Apply")
            discover(qmp, evidence)
            qmp.set(ELECTRICAL, "project-json", graph)
            require(qmp.get(ELECTRICAL, "project-json") == graph, "Actual applied public graph differs")
            save(evidence / "applied-electrical-snapshot.json", qmp.get(ELECTRICAL, "snapshot-json"))
            collector = Collector(qmp, evidence, result, args.profile.startswith("lcd-camera-"))
            collector.observe("applied-before-cpu", "", physical=True)
            qmp.call("cont")
            milestones = 0
            while True:
                require(proc.poll() is None, f"QEMU exited before consumer completion: {proc.returncode}")
                require(time.monotonic() < deadline, "Diagnostic host watchdog expired; guest consumer remains unqualified")
                text = uart.read_text(errors="replace") if uart.exists() else ""
                status = collector.status()
                append(evidence / "running-status.jsonl", dict(host_monotonic_ns=time.monotonic_ns(), **status))
                runtime = qmp.call("query-status")
                require(runtime["running"], f"Unexpected real QEMU pause: {runtime}; see retained electrical/guest diagnostics")
                require(not re.search(r"^LCDCAM (?:ERROR|UNQUALIFIED)\b", text, re.M), "Actual firmware rejected the external consumer; see uart.log")
                marks = len(re.findall(r"^LCDCAM (?:CAM_TWO_HELD|CAM_HOLD_BEGIN|CAM_HOLD_END|CAM_RETURN_BEGIN|CAM_REARM|RGB_SWAP_REQUEST|RGB_STOPPED)\b.*$", text, re.M))
                completed = END in text
                if collector.camera:
                    changed = any(row["capture_frame"] > collector.sensor_last and row["capture_byte_count"] for row in status["sensors"])
                else:
                    changed = any(row["captures"] > collector.panel_next for row in status["panels"]["panels"])
                counter = "cam_bytes" if collector.camera else "lcd_words"
                in_flight = (not collector.active_seen and
                             status["controller"][counter] > collector.controller_snapshots[-1][counter])
                if changed or marks != milestones or completed or in_flight:
                    reason = ("firmware-end" if completed else "completed-source-frame" if changed else
                              "observed-uart-milestone" if marks != milestones else "actual-in-flight-controller-progress")
                    pause, text = collector.freeze(reason, uart)
                    milestones = len(re.findall(r"^LCDCAM (?:CAM_TWO_HELD|CAM_HOLD_BEGIN|CAM_HOLD_END|CAM_RETURN_BEGIN|CAM_REARM|RGB_SWAP_REQUEST|RGB_STOPPED)\b.*$", text, re.M))
                    if END in text:
                        save(evidence / "final-electrical-snapshot.json", qmp.get(ELECTRICAL, "snapshot-json"))
                        proof = oracle.verify(args.profile, text, collector.panel_frames, collector.sensor_captures, collector.controller_snapshots)
                        save(evidence / "oracle-proof.json", proof)
                        result["oracle"] = proof
                        require(proof.get("status") in ("PASS", "QUALIFIED_LIMITED"), "Oracle returned no explicit qualification status")
                        result["status"] = proof["status"]
                        break
                    collector.resume(pause)
                time.sleep(.001)
            qmp.call("quit")
            proc.wait(timeout=10)
            require(proc.returncode == 0, f"QEMU failed orderly termination: {proc.returncode}")
            result["qemu_returncode"] = proc.returncode
    except Exception as exc:
        result["status"] = "PREREQUISITE" if "Prerequisite:" in str(exc) or isinstance(exc, (FileNotFoundError, ModuleNotFoundError)) else "FAIL"
        result["error"] = f"{type(exc).__name__}: {exc}"
        (evidence / "failure-traceback.log").write_text(traceback.format_exc(), encoding="utf-8")
    finally:
        if collector is not None:
            save(evidence / "controller-snapshots.json", collector.controller_snapshots)
        if proc is not None and proc.poll() is None:
            if qmp is not None:
                qmp.deadline = time.monotonic() + 5
                try:
                    state = qmp.call("query-status")
                    if not state["running"]:
                        qmp.call("cont")
                    qmp.call("quit")
                    proc.wait(timeout=5)
                except Exception as exc:
                    result["cleanup_qmp_error"] = f"{type(exc).__name__}: {exc}"
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        if qmp is not None:
            try:
                qmp.close()
            except OSError as exc:
                result["cleanup_close_error"] = f"{type(exc).__name__}: {exc}"
                result["status"] = "FAIL"
        if proc is not None:
            result["qemu_returncode_after_cleanup"] = proc.returncode
        if scratch.exists():
            result["writable_flash_final_sha256"] = digest(scratch)
        if original_hash is not None:
            try:
                result["original_flash_after_sha256"] = digest(args.merged_flash)
                result["original_flash_unchanged"] = result["original_flash_after_sha256"] == original_hash
            except OSError as exc:
                result["original_flash_unchanged"] = False
                result["original_flash_read_error"] = f"{type(exc).__name__}: {exc}"
        changed = []
        for name, expected in provenance.hashes.items():
            path = pathlib.Path(name)
            try:
                unchanged = path.is_file() and digest(path) == expected
            except OSError:
                unchanged = False
            if not unchanged:
                changed.append(name)
        result["input_hashes"] = provenance.hashes
        result["changed_inputs"] = changed
        if changed or result.get("original_flash_unchanged") is False:
            result["status"], result["error"] = "FAIL", "Immutable qualification input changed during collection"
        result["host_elapsed_seconds"] = time.monotonic() - started
        result["evidence_sha256"] = {str(path.relative_to(evidence)): digest(path) for path in evidence.rglob("*") if path.is_file()}
        save(evidence / "result.json", result)
    print(json.dumps(dict(status=result["status"], profile=args.profile, evidence=str(evidence), error=result.get("error"))))
    return 0 if result["status"] == "PASS" else 2 if result["status"] in ("PREREQUISITE", "QUALIFIED_LIMITED") else 1


if __name__ == "__main__":
    raise SystemExit(main())
