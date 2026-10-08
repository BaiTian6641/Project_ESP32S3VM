#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prototype-disabled boot/control baseline on the extension QEMU binary.

Boots the supplied IDF firmware WITHOUT the esp32s3-hostbus-probe object,
requires the boot marker and optional pinned dual-core heartbeats, then
exercises ordinary QMP control (stop/cont/reset) on the same binary. This
proves the prototype-enabled build changes did not alter native behavior
when the probe is absent.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

spec = importlib.util.spec_from_file_location("hostbus_probe_test", Path(__file__).with_name("probe-test.py"))
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
require = shared.require
Qmp = shared.Qmp
read_uart = shared.read_uart
core_heartbeats = shared.core_heartbeats
cores_executed = shared.cores_executed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--boot-marker", default="ESP32S3VM_BOOT_OK cores=2 flash=4194304")
    parser.add_argument("--boot-timeout", type=float, default=120)
    parser.add_argument("--require-core-heartbeats", action="store_true")
    options = parser.parse_args()
    root = (options.output or Path(tempfile.mkdtemp(prefix="esp32s3vm-hostbus-baseline-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    options.flash = options.flash.resolve()
    require(options.flash.stat().st_size == 4 * 1024 * 1024, "Supply a complete 4 MiB flash image")
    manifest = {"base_commit": "40edccac415693c5130f91c01d84176ae6008566",
                "qemu_sha256": shared.digest(options.qemu),
                "flash_sha256": shared.digest(options.flash),
                "probe_object_present": False,
                "scope": "prototype-disabled boot/control baseline only"}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    checks = []
    uart = root / "uart.log"
    uart.touch()
    errors = open(root / "qemu.stderr", "w")
    process = None
    qmp = None
    try:
        with tempfile.TemporaryDirectory(prefix="s3hbb-") as sockets:
            args = [options.qemu, "-M", "esp32s3", "-global",
                    "driver=esp32s3.gpio,property=strap_mode,value=0x04",
                    "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
                    "-display", "none", "-monitor", "none", "-serial", f"file:{uart}", "-S",
                    "-qmp", f"unix:{sockets}/qmp,server=on,wait=off", "-L", options.data_dir,
                    "-drive", f"file={options.flash},if=mtd,format=raw"]
            (root / "command.json").write_text(json.dumps(args, indent=2) + "\n")
            require(not any("hostbus-probe" in arg for arg in args),
                    "Baseline must not contain the probe object")
            process = subprocess.Popen(args, stdout=errors, stderr=errors)
            qmp = Qmp(Path(sockets) / "qmp", process, root)
            cpus = qmp.command("query-cpus-fast")
            require(len(cpus) == 2, f"Expected two ESP32-S3 CPUs: {cpus}")
            checks.append("two cpus configured")
            qmp.command("cont")
            end = time.monotonic() + options.boot_timeout
            marker_seen = False
            while time.monotonic() < end:
                text = read_uart(uart)
                evidence = core_heartbeats(text)
                if options.boot_marker in text and (not options.require_core_heartbeats
                                                    or cores_executed(evidence)):
                    marker_seen = True
                    break
                time.sleep(0.05)
            require(marker_seen, f"Boot marker absent: {read_uart(uart)[-2000:]}")
            checks.append("boot marker")
            if options.require_core_heartbeats:
                evidence = core_heartbeats(read_uart(uart))
                require(cores_executed(evidence),
                        f"Both cores did not execute iterations 1/2/3: {evidence}")
                checks.append("both cores executed iterations 1/2/3")
            qmp.command("stop")
            require(qmp.command("query-status")["status"] == "paused", "stop not acknowledged")
            checks.append("stop control")
            qmp.command("cont")
            require(qmp.command("query-status")["status"] == "running", "cont did not resume")
            checks.append("cont control")
            qmp.command("system_reset")
            end = time.monotonic() + 5
            while not any(event["event"] == "RESET" for event in qmp.events):
                require(time.monotonic() < end, "RESET event missing after system_reset")
                time.sleep(0.02)
            checks.append("system_reset control")
            qmp.command("quit")
            process.wait(timeout=10)
            process = None
    except Exception as error:
        detail = traceback.format_exc()
        (root / "failure.traceback.txt").write_text(detail)
        print(json.dumps({"passed": False, "completed_checks": checks, "error": str(error),
                          "artifacts": str(root), "traceback": detail}), flush=True)
        return 1
    finally:
        if qmp is not None:
            qmp.close()
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        errors.close()
    result = {"passed": True, "checks": checks, "artifacts": str(root),
              "native_peripheral_support": False,
              "scope": "prototype-disabled boot/control baseline only"}
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
