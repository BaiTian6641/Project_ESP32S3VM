#!/usr/bin/env python3
"""Run the three frozen boot/control profiles on a candidate ESP32-S3 binary.

Each profile boots the real gui_esp32s3_boot_smoke_test against a fresh
writable scratch copy of its frozen original merged image; the immutable
original is hash-verified before and after. Emits one result JSON per
profile in the schema consumed by run-lcd-cam-consumers.py
memory_foundation() boot_control rows. No qualification claim beyond the
actual observed PASS/FAIL output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def digest(path):
    value = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--originals", required=True, type=Path,
                        help="Explicit frozen corpus/helper identity recipe; historical snapshots are never repinned")
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    qemu = args.qemu.resolve(strict=True)
    spec = json.loads(args.originals.resolve(strict=True).read_text())
    smoke = (ROOT / spec["smoke_executable"]["path"].replace("C:\\Users\\weyst\\Documents\\ChatGPT\\ESP32S3VM\\Project_ESP32S3VM\\", "").replace("\\", "/")).resolve(strict=True)
    require = lambda cond, msg: (_ for _ in ()).throw(AssertionError(msg)) if not cond else None
    require(digest(smoke) == spec["smoke_executable"]["sha256"], "Boot smoke executable differs from frozen original")
    args.evidence.mkdir(parents=True, exist_ok=True)
    rows = []
    for entry in spec["firmware"]:
        profile = entry["profile"]
        rel = entry["original"].replace("C:\\Users\\weyst\\Documents\\ChatGPT\\ESP32S3VM\\Project_ESP32S3VM\\", "").replace("\\", "/")
        original = (ROOT / rel).resolve(strict=True)
        require(digest(original) == entry["sha256"], f"Frozen original changed: {profile}")
        work = Path(tempfile.mkdtemp(prefix=f"boot-{profile}-", dir=args.evidence.resolve()))
        scratch = work / "boot-firmware.bin"
        shutil.copyfile(original, scratch)
        scratch.chmod(0o600)
        stdout, stderr = work / "stdout.log", work / "stderr.log"
        env = dict(os.environ, ESP32S3_QEMU_BIN=str(qemu), ESP32S3_BOOT_FIRMWARE=str(scratch),
                   QT_QPA_PLATFORM="offscreen")
        started = time.monotonic()
        with stdout.open("wb") as out, stderr.open("wb") as err:
            proc = subprocess.run([str(smoke)], cwd=work, env=env, stdout=out, stderr=err, timeout=300)
        text = stdout.read_text(errors="replace") + stderr.read_text(errors="replace")
        passed = ("PASS   : BootSmokeTest::bootsRealFirmware()" in text and
                  "FAIL!" not in text and "SKIP" not in text and proc.returncode == 0)
        result = {
            "schema_version": 1,
            "status": "PASS" if passed else "FAIL",
            "profile": profile,
            "returncode": proc.returncode,
            "host_elapsed_seconds": time.monotonic() - started,
            "command": [str(smoke)],
            "cwd": str(work),
            "environment": {"ESP32S3_QEMU_BIN": str(qemu), "ESP32S3_BOOT_FIRMWARE": str(scratch),
                            "QT_QPA_PLATFORM": "offscreen"},
            "firmware": {"path": str(scratch), "sha256": digest(scratch)},
            "stdout": {"path": str(stdout), "sha256": digest(stdout)},
            "stderr": {"path": str(stderr), "sha256": digest(stderr)},
            "test_binary_sha256": spec["smoke_executable"]["sha256"],
            "original": {"path": str(original), "sha256": entry["sha256"]},
            "original_after_sha256": digest(scratch),
            "frozen_original_unchanged": digest(original) == entry["sha256"],
        }
        require(result["frozen_original_unchanged"], f"Immutable boot original changed: {profile}")
        result_path = work / "result.json"
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        rows.append({"profile": profile, "status": result["status"], "result_path": str(result_path),
                     "result_sha256": digest(result_path)})
        print(json.dumps(rows[-1]))
    return 0 if all(row["status"] == "PASS" for row in rows) and len(rows) == 3 else 1


if __name__ == "__main__":
    raise SystemExit(main())
