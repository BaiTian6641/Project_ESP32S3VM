#!/usr/bin/env python3
"""Prepare by default; execute only explicitly approved fixture writes + restore."""
import argparse
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = {
    "simd": ROOT / "tests/firmware/simd_reference/build",
    "wifi": ROOT / "tests/firmware/radio_init/build/wifi",
    "ble": ROOT / "tests/firmware/radio_init/build/ble",
}


def sha(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def validate(deployment, selected):
    if deployment.get("schema_version") != 1 or deployment.get("board", {}).get("port") != "COM5":
        raise ValueError("Only the prepared COM5 deployment is accepted")
    board = deployment["board"]
    if board.get("chip") != "ESP32-S3" or board.get("flash_bytes") != 16777216:
        raise ValueError("Unexpected prepared board")
    if board.get("secure_boot") is not False or board.get("flash_encryption") is not False:
        raise ValueError("Secured profiles cannot use this temporary deployment")
    backup = Path(deployment["files"]["backup"]["path"]).resolve(strict=True)
    hardware_root = (ROOT / "build-hardware-host").resolve()
    if backup.parent != hardware_root or not backup.name.endswith(".bin"):
        raise ValueError("Backup must be a .bin image directly below build-hardware-host")
    paths = {"backup": backup}
    for name in selected:
        folder = FIXTURES[name]
        filename = "simd_reference.merged.bin" if name == "simd" else "radio_init.merged.bin"
        image = (folder / filename).resolve(strict=True)
        record = deployment["files"][name]
        if Path(record["path"]).resolve() != image:
            raise ValueError(f"{name}: prepared path differs")
        manifest_path = folder / "reference-manifest.json"
        if sha(manifest_path) != record.get("manifest_sha256"):
            raise ValueError(f"{name}: frozen provenance manifest changed")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest["artifacts"]["flash_sha256"] != record["sha256"]:
            raise ValueError(f"{name}: frozen fixture manifest differs")
        if manifest.get("capture_grace_ms") != 15000:
            raise ValueError(f"{name}: USB capture startup grace is not established")
        elf = folder / ("esp32s3_simd_reference.elf" if name == "simd" else "esp32s3_radio_init.elf")
        if sha(elf) != manifest["artifacts"]["elf_sha256"]:
            raise ValueError(f"{name}: ELF differs from the frozen image provenance")
        paths[name] = image
    for name, path in paths.items():
        record = deployment["files"][name]
        expected_bytes = 16777216 if name == "backup" else 4194304
        if path.stat().st_size != expected_bytes or sha(path) != record["sha256"]:
            raise ValueError(f"{name}: artifact changed from the frozen deployment")
    return paths


def execute_with_restore(selected, flash, capture, restore, verify_restore, record,
                         no_restore=False):
    """Recover only after programming is attempted, including partial writes.

    flash(name, mark_mutation) must call the supplied checkpoint immediately
    before the first mutating API call, after all preflight/identity checks.
    A rejected original backup must never itself trigger restoration.
    no_restore records the explicit operator waiver instead of recovering.
    """
    must_restore = False
    record.setdefault("restoration", "not-needed")
    try:
        for name in selected:
            def mark_mutation():
                nonlocal must_restore
                must_restore = True  # A failed write may already erase sectors.
                record.setdefault("mutation_attempted", []).append(name)
            flash(name, mark_mutation)
            record["captures"][name] = capture(name)
    finally:
        if must_restore:
            if no_restore:
                # Explicit waiver: never swallow an in-flight failure.
                record["restoration"] = "waived"
            else:
                record["restoration"] = "attempting"
                restore()
                verify_restore()
                record["restoration"] = "verified"


def freeze_flash_bytes(deployment, paths):
    """Read once and validate the exact immutable bytes passed to esptool.

    File paths are provenance only after this point. Restoration keeps its own
    verified in-memory snapshot even if the file changes during acquisition.
    """
    payloads = {}
    for name, path in paths.items():
        data = path.read_bytes()
        expected_bytes = 16777216 if name == "backup" else 4194304
        if len(data) != expected_bytes or hashlib.sha256(data).hexdigest() != deployment["files"][name]["sha256"]:
            raise RuntimeError(f"{name}: bytes differ from frozen deployment")
        payloads[name] = data
    return payloads


def check_live_identity(esp, deployment, *, security=False, flash=False):
    """Read the board on the SAME open connection used by the next operation."""
    mac = esp.read_mac()
    if len(mac) != 6 or any(type(value) is not int or not 0 <= value <= 255 for value in mac):
        raise RuntimeError("Invalid live board identity")
    identity = ":".join(f"{value:02x}" for value in mac)
    if hashlib.sha256(identity.encode()).hexdigest() != deployment.get("hardware_id_sha256"):
        raise RuntimeError("Connected board identity differs; refusing to write or restore")
    if esp.CHIP_NAME != "ESP32-S3" or esp.get_chip_revision() != 2 or esp.get_crystal_freq() != 40:
        raise RuntimeError("Board type/revision/crystal differs; refusing to write or restore")
    if security:
        information = esp.get_security_info()
        flags = information.get("parsed_flags", {})
        count = information.get("flash_crypt_cnt")
        if flags.get("SECURE_BOOT_EN") is not False or type(count) is not int or count < 0 or count.bit_count() % 2:
            raise RuntimeError("Board security state differs; refusing to write or restore")
    if flash:
        flash_id = esp.flash_id()
        capacity_code = (flash_id >> 16) & 0xff
        if not 16 <= capacity_code <= 30 or (1 << capacity_code) != deployment["board"]["flash_bytes"]:
            raise RuntimeError("Flash capacity differs; refusing to write or restore")


class HardwareOperations:
    """One identified ROM/stub connection per verify/program transaction.

    Identity, security and capacity are read as structured values, not combined
    text logs. Stub upload uses the same serial handle. The original verification
    and first write are one transaction; restore and verification are another.
    Neither lets the application run between protected operations.
    """
    def __init__(self, deployment, paths, output, api=None):
        self.deployment, self.paths, self.output = deployment, paths, output
        self.payloads = freeze_flash_bytes(deployment, paths)
        self.first_write = True
        if api is None:
            import esptool
            from esptool import cmds
            if esptool.__version__ != "5.4.0":
                raise RuntimeError("Use the prepared pinned esptool 5.4.0 host environment")
            api = cmds
        self.api = api

    @contextmanager
    def identified_connection(self, name):
        with (self.output / f"{name}.log").open("w", encoding="utf-8") as log:
            with redirect_stdout(log), redirect_stderr(log):
                with self.api.connect_esp(port="COM5", chip="esp32s3", before="usb-reset",
                                          connect_attempts=3, open_port_attempts=1) as rom:
                    check_live_identity(rom, self.deployment, security=True)
                    stub = self.api.run_stub(rom)
                    if stub._port is not rom._port:
                        raise RuntimeError("Stub changed serial connection; refusing operation")
                    # esptool's internal retry can reopen COM5 without our identity
                    # guard. A failed write must propagate to an independently
                    # identified recovery connection instead of retrying blindly.
                    stub.WRITE_FLASH_ATTEMPTS = 1
                    self.api.attach_flash(stub)
                    check_live_identity(stub, self.deployment, flash=True)
                    yield stub

    def flash(self, name, mark_mutation):
        validate(self.deployment, [name])
        print(f"Temporarily program approved {name} fixture", flush=True)
        with self.identified_connection(f"flash-{name}") as esp:
            if self.first_write:
                self.api.verify_flash(esp, [(0, self.payloads["backup"])])
            # Catch reassignment/connection drift immediately before mutation;
            # verification and programming retain one quiescent serial handle.
            check_live_identity(esp, self.deployment, flash=True)
            mark_mutation()
            self.api.write_flash(esp, [(0, self.payloads[name])],
                                 flash_mode="keep", flash_freq="keep", flash_size="keep")
            self.first_write = False
            self.api.reset_chip(esp, "hard-reset")

    def restore(self):
        if hashlib.sha256(self.payloads["backup"]).hexdigest() != self.deployment["files"]["backup"]["sha256"]:
            raise RuntimeError("Backup changed; cannot restore unverified bytes")
        print("Restore and verify complete original 16 MiB image", flush=True)
        with self.identified_connection("restore-and-verify") as esp:
            check_live_identity(esp, self.deployment, flash=True)
            self.api.write_flash(esp, [(0, self.payloads["backup"])],
                                 flash_mode="keep", flash_freq="keep", flash_size="keep")
            # No reset/application execution between restoration and verification.
            check_live_identity(esp, self.deployment, flash=True)
            self.api.verify_flash(esp, [(0, self.payloads["backup"])])
            self.api.reset_chip(esp, "hard-reset")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selection", choices=["simd", "all"], default="simd")
    parser.add_argument("--execute", action="store_true", help="Only after explicit user approval of this deployment")
    parser.add_argument("--no-restore", action="store_true",
                        help="Operator explicitly waived restoring the previous firmware; state is recorded")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    selected = ["simd", "wifi", "ble"] if args.selection == "all" else ["simd"]
    deployment_path = ROOT / "build-hardware-host/deployment-manifest.json"
    deployment = json.loads(deployment_path.read_text(encoding="utf-8"))
    paths = validate(deployment, selected)
    plan = {"selection": selected, "port": "COM5", "fixture_offset": "0x0",
            "images": {name: str(paths[name]) for name in selected},
            "restore": None if args.no_restore else str(paths["backup"]),
            "erase_all": False, "efuse_writes": False, "execution_requested": args.execute,
            "restore_waived": bool(args.no_restore)}
    if not args.execute:
        print(json.dumps(plan, indent=2))
        print("Preparation only. No port was opened and no flash operation was executed.")
        return 0
    if not args.output or args.output.exists():
        parser.error("Execution requires a new --output evidence directory")
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / "build-hardware-host"):
        parser.error("Evidence must remain below the local hardware environment")
    output.mkdir(parents=True)
    (output / "deployment-manifest.json").write_bytes(deployment_path.read_bytes())
    record = {"schema_version": 1, "plan": plan, "captures": {}, "restoration": "not-needed",
              "authorization": "explicit operator --execute; parent must have recorded the human approval"}
    try:
        operations = HardwareOperations(deployment, paths, output)

        def capture(name):
            folder = FIXTURES[name]
            capture_path = output / name
            capture_path.mkdir()
            command = [sys.executable, str(ROOT / "tools/hardware-reference.py"), "--capture", "--port", "COM5",
                       "--manifest", str(folder / "reference-manifest.json"), "--seconds", "45",
                       "--output", str(capture_path / "capture"), "--board-context", json.dumps(deployment["board"])]
            with (capture_path / "acquisition.log").open("wb") as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=55)
            return {"return_code": completed.returncode, "path": str(capture_path / "capture")}

        execute_with_restore(selected, operations.flash, capture, operations.restore, lambda: None, record,
                             no_restore=args.no_restore)
    except BaseException as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        (output / "result.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Hardware run complete; restoration {record['restoration']}. Results: {output}")
    return 0 if all(item["return_code"] == 0 for item in record["captures"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())
