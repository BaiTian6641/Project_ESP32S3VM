#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real QEMU early-restore rejection tests, with exact stopped-state checks."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location("hostbus_probe_test", Path(__file__).resolve().parents[1] / "probe-test.py")
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
require = shared.require


def command(options, directory, socket_dir, peer_port):
    return [options.qemu, "-M", "esp32s3", "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
            "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
            "-display", "none", "-monitor", "none", "-serial", f"file:{directory / 'uart.log'}", "-S",
            "-qmp", f"unix:{socket_dir}/qmp,server=on,wait=off", "-L", options.data_dir,
            "-drive", f"file={directory / 'flash.bin'},if=mtd,format=raw",
            "-chardev", f"socket,id=hostbus,host=127.0.0.1,port={peer_port},server=off,reconnect-ms=200",
            "-object", "esp32s3-hostbus-probe,id=probe,chardev=hostbus,watchdog-ms=20000"]


def snapshot(qmp):
    result = {"runstate": qmp.command("query-status"),
              "migration": qmp.command("query-migrate"), "jobs": qmp.command("query-jobs")}
    for property_name in ("phase", "resume-blocked", "virtual-ns", "armed-ns", "stopped-ns",
                          "delivered-ns", "last-error", "cpu-step-flags"):
        result[property_name] = qmp.get(property_name)
    registers = []
    for index in (0, 1):
        qmp.command("human-monitor-command", {"command-line": f"cpu {index}"})
        registers.append(qmp.command("human-monitor-command", {"command-line": "info registers"}))
    qmp.command("human-monitor-command", {"command-line": "cpu 0"})
    result["cpu_registers"] = registers
    return result


def barrier_case(options, root):
    directory = root / "barrier"
    directory.mkdir()
    shutil.copyfile(options.flash, directory / "flash.bin")
    (directory / "uart.log").touch()
    peer = shared.Peer(options.peer, directory, delay=0, latency=0, drop=True)
    process = qmp = None
    errors = open(directory / "qemu.stderr", "w")
    with tempfile.TemporaryDirectory(prefix="s3hbrst-") as sockets:
        try:
            listening = peer.wait("listening")
            args = command(options, directory, sockets, listening["port"])
            (directory / "command.json").write_text(json.dumps(args, indent=2) + "\n")
            process = subprocess.Popen(args, stdout=errors, stderr=errors)
            qmp = shared.Qmp(Path(sockets) / "qmp", process, directory)
            peer.wait("ready")
            cpus = shared.boot(qmp, directory / "uart.log", options.boot_marker, options.boot_timeout)
            deadline, _, _ = shared.arm(qmp, peer)
            expected = snapshot(qmp)
            require(expected["phase"] == "blocked" and expected["resume-blocked"] is True,
                    "Restore tests require an unresolved stopped barrier")
            checks = []
            for repeat in range(2):
                for operation in ("hmp-loadvm", "qmp-snapshot-load", "qmp-migrate-incoming"):
                    previous_events = len(qmp.events)
                    if operation == "hmp-loadvm":
                        denial = qmp.command("human-monitor-command", {"command-line": "loadvm __hostbus_absent__"})
                    elif operation == "qmp-snapshot-load":
                        denial = qmp.command("snapshot-load", {"job-id": f"blocked-load-{repeat}",
                                             "tag": "__hostbus_absent__", "vmstate": "__invalid_node__",
                                             "devices": []}, expect_error=True)["desc"]
                    else:
                        denial = qmp.command("migrate-incoming", {"uri": "tcp:127.0.0.1:1",
                                             "exit-on-error": False}, expect_error=True)["desc"]
                    require("Hostbus probe cannot" in denial and "external-peer state" in denial,
                            f"{operation} did not reject through probe-specific preflight: {denial}")
                    observed = snapshot(qmp)
                    require(observed == expected,
                            f"{operation} changed stopped state: before={expected}, after={observed}")
                    require(not any(event["event"] in ("STOP", "RESUME", "RESET")
                                    for event in qmp.events[previous_events:]),
                            f"{operation} emitted a runstate event")
                    checks.append({"operation": operation, "repeat": repeat + 1, "denial": denial})
            shared.assert_stopped(qmp, deadline)
            return {"scenario": "barrier", "passed": True, "checks": checks, "cpus": cpus,
                    "unchanged_state": expected, "native_peripheral_support": False}
        finally:
            if qmp:
                try:
                    qmp.command("quit")
                except (OSError, AssertionError):
                    pass
                qmp.close()
            if process:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            peer.close()
            errors.close()


def startup_case(options, root, name):
    directory = root / name
    directory.mkdir()
    shutil.copyfile(options.flash, directory / "flash.bin")
    peer = shared.Peer(options.peer, directory, delay=0, latency=0, drop=True)
    with tempfile.TemporaryDirectory(prefix="s3hbst-") as sockets:
        try:
            listening = peer.wait("listening")
            args = command(options, directory, sockets, listening["port"])
            args.extend(["-loadvm", "__hostbus_absent__"] if name == "startup-loadvm"
                        else ["-incoming", "tcp:127.0.0.1:1"])
            (directory / "command.json").write_text(json.dumps(args, indent=2) + "\n")
            run = subprocess.run(args, capture_output=True, text=True, timeout=15)
            (directory / "qemu.stdout").write_text(run.stdout)
            (directory / "qemu.stderr").write_text(run.stderr)
            require(run.returncode != 0, f"{name} unexpectedly proceeded past restore rejection")
            require("Hostbus probe cannot" in run.stderr and "external-peer state" in run.stderr,
                    f"{name} failed for a different reason: {run.stderr}")
            return {"scenario": name, "passed": True, "exit_code": run.returncode,
                    "denial": run.stderr.strip(), "native_peripheral_support": False}
        finally:
            peer.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--peer", required=True)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--boot-marker", default="ESP32S3VM_BOOT_OK cores=2 flash=4194304")
    parser.add_argument("--boot-timeout", type=float, default=30)
    parser.add_argument("--scenario", action="append", choices=["barrier", "startup-loadvm", "startup-incoming"])
    options = parser.parse_args()
    root = (options.output or Path(tempfile.mkdtemp(prefix="esp32s3vm-hostbus-restore-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    results = []
    try:
        for name in options.scenario or ["barrier", "startup-loadvm", "startup-incoming"]:
            result = barrier_case(options, root) if name == "barrier" else startup_case(options, root, name)
            (root / name / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            print(json.dumps(result), flush=True)
        (root / "result.json").write_text(json.dumps({"passed": True, "results": results}, indent=2) + "\n")
        return 0
    except Exception as error:
        result = {"passed": False, "error": str(error), "completed_results": results, "artifacts": str(root)}
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
