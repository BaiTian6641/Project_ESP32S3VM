#!/usr/bin/env python3
"""Run built normal-IDF firmware on a local flash copy, never a corpus image."""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

from expected_vectors import FRAMES, cadence, expected_capture, fnv1a, matrix
from graph_vectors import project


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(20)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)
        self.events = []
        greeting = json.loads(self.stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError(greeting)
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        self.stream.write((json.dumps(dict(execute=command, arguments=arguments or {})) + "\n").encode())
        while True:
            result = json.loads(self.stream.readline())
            if "event" in result:
                self.events.append(result)
            elif "error" in result:
                raise RuntimeError(result)
            else:
                return result["return"]

    def snapshot(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))

    def close(self):
        self.stream.close()
        self.sock.close()


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def records(text, tag):
    return [dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
            for line in text.splitlines() if line.startswith(tag + " ")]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate(text):
    rows = matrix()
    actual_cases = records(text, "I2S_CASE")
    require(len(actual_cases) == len(rows), f"case count {len(actual_cases)} != {len(rows)}")
    for got, wanted in zip(actual_cases, rows):
        for key, value in wanted.items():
            parsed = int(got[key], 16) if key == "mask" else int(got[key]) if isinstance(value, int) else got[key]
            require(parsed == value, f"case matrix mismatch {key}: {got} != {wanted}")
    observations = records(text, "I2S_OBS")
    seen = set()
    for got in observations:
        case = int(got["case"])
        require(0 <= case < len(rows), f"unexpected case {case}")
        row = rows[case]
        receiver = int(got["controller"])
        phases = {"sustained": 8} if row["mode"] == "pdm" else {"sustained": 8, "starved": 4, "restart": 4}
        require(got["phase"] in phases and receiver in (0, 1), f"unexpected observation {got}")
        if row["mode"] == "pdm":
            require(receiver == 1 - row["source"], f"wrong raw PDM receiver {got}")
        rounds = phases[got["phase"]]
        key = (case, got["phase"], receiver)
        require(key not in seen, f"duplicate observation {key}")
        seen.add(key)
        expected = expected_capture(row, receiver, int(got["offset"]), rounds)
        require(got["expected"] == expected["hash"], f"guest expected hash disagrees with independent host {key}")
        require(got["observed"] == expected["hash"], f"observed sample hash mismatch {key}: {got}")
        require(int(got["bytes"]) == expected["bytes"] and int(got["frames"]) == expected["frames"], f"sample count mismatch {key}")
        require(got["match"] == f"{rounds}/{rounds}", f"partial matching {key}")
        require(int(got["txeof"]) >= rounds and int(got["rxeof"]) >= rounds, f"missing actual DMA callbacks {key}")
        if got["phase"] == "starved":
            require(int(got["rxovf"]) > 0, f"no genuine starvation overflow {key}")
    expected_seen = set()
    for row in rows:
        receivers = (1 - row["source"],) if row["mode"] == "pdm" else (0, 1)
        phases = ("sustained",) if row["mode"] == "pdm" else ("sustained", "starved", "restart")
        expected_seen.update((row["case"], phase, receiver) for phase in phases for receiver in receivers)
    require(seen == expected_seen, f"missing observations {expected_seen - seen}")
    timing = records(text, "I2S_CADENCE")
    timing_seen = set()
    for got in timing:
        case = int(got["case"])
        require(0 <= case < len(rows), f"unexpected timing case {case}")
        ref = cadence(rows[case])
        key = (case, got["phase"])
        require(key not in timing_seen, f"duplicate cadence {key}")
        timing_seen.add(key)
        require(got["frame_hz"] == ref["frame_hz"] and got["interval_ns"] == ref["interval_ns"], f"wrong rational cadence {key}")
        require(int(got["eof_frames"]) == FRAMES, f"wrong descriptor cadence {key}")
        intervals = int(got["intervals"])
        elapsed = int(got["elapsed_us"])
        wanted = ref["frame_us"] * intervals * FRAMES
        require(intervals >= 3 and elapsed > 0, f"insufficient cadence observation {key}")
        # CPU ISR service timestamps, not an electrical edge counter: bounded
        # allowance for ISR scheduling/esp_timer microsecond quantization.
        tolerance = max(Fraction(2000), wanted / 50)
        require(abs(elapsed - wanted) <= tolerance, f"DMA EOF cadence mismatch {key}: {elapsed} us vs {wanted} us")
    require(timing_seen == {(case, phase) for case, phase, _ in expected_seen}, "missing cadence records")
    disabled = records(text, "I2S_DISABLED")
    disabled_seen = set()
    for got in disabled:
        key = (int(got["case"]), int(got["controller"]))
        require(key not in disabled_seen, f"duplicate disabled record {key}")
        disabled_seen.add(key)
        # Pinned esp_err.h ESP_ERR_INVALID_STATE = 0x103.
        require(got["read"] == "259" and got["write"] == "259" and got["frozen"] == "1", f"disable/restart contract failed {got}")
    require(disabled_seen == {(r["case"], c) for r in rows if r["mode"] != "pdm" for c in (0, 1)}, "missing disabled records")
    caps = records(text, "I2S_CAP")
    cap_seen = set()
    for got in caps:
        cid = int(got["controller"])
        key = (cid, got["direction"], got["format"])
        require(key not in cap_seen, f"duplicate capability {key}")
        cap_seen.add(key)
        wanted = 262 if cid == 1 and got["format"] == "pcm" else 0
        require(int(got["expected"]) == wanted and int(got["observed"]) == wanted, f"controller capability mismatch {got}")
    require(cap_seen == {(c, d, f) for c in (0, 1) for d in ("tx", "rx") for f in ("raw", "pcm")}, "missing capability matrix")
    limits = records(text, "I2S_TDM_LIMIT")
    limit_seen = set()
    for got in limits:
        key = (int(got["controller"]), got["direction"], int(got["bits"]))
        require(key not in limit_seen, f"duplicate frame-limit observation {key}")
        limit_seen.add(key)
        require(got["slots"] == "8" and got["expected"] == "258" and got["observed"] == "258",
                f"oversized TDM frame was not rejected {got}")
    require(limit_seen == {(c, d, b) for c in (0, 1) for d in ("tx", "rx") for b in (24, 32)},
            "missing oversized TDM frame rejection matrix")
    done = records(text, "I2S_NATIVE_DONE")
    require(len(done) == 1 and int(done[0]["cases"]) == len(rows) and done[0]["failures"] == "0", f"firmware reported failures {done}")
    return dict(cases=len(rows), exact_sample_records=len(seen), cadence_records=len(timing_seen),
                capability_records=len(caps), frame_limit_records=len(limits),
                disable_records=len(disabled_seen))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--flash", required=True, help="built IDF fixture merged flash, copied before launch")
    parser.add_argument("--evidence", required=True, help="NEW run directory")
    parser.add_argument("--scenario", choices=("connected", "disconnected-data", "external-peer"), default="connected")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    evidence = Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    result = dict(status="FAIL", scenario=args.scenario,
                  unqualified=["external registered I2S sample peer", "PCM2PDM/PDM2PCM exact converter output"])
    proc = qmp = None
    original = Path(args.flash).resolve()
    original_hash = sha256(original)
    local_flash = evidence / "fixture-flash.bin"
    uart = evidence / "uart.log"
    qmp_path = evidence / "qmp.sock"
    try:
        document = project(args.scenario)
        (evidence / "graph.json").write_text(json.dumps(document, indent=2) + "\n")
        (evidence / "expected-matrix.json").write_text(json.dumps(matrix(), indent=2) + "\n")
        shutil.copyfile(original, local_flash)
        require(sha256(local_flash) == original_hash, "flash copy changed")
        command = [str(Path(args.qemu).resolve()), "-machine", "esp32s3", "-nographic", "-S",
                   "-serial", f"file:{uart}", "-drive", f"file={local_flash},if=mtd,format=raw",
                   "-qmp", f"unix:{qmp_path},server=on,wait=off"]
        result["command"] = command
        result["qemu_sha256"] = sha256(args.qemu)
        with (evidence / "qemu.log").open("w") as errors:
            proc = subprocess.Popen(command, stdout=errors, stderr=errors)
            deadline = time.monotonic() + 30
            while not qmp_path.exists():
                require(proc.poll() is None, f"QEMU exited {proc.returncode}")
                require(time.monotonic() < deadline, "QMP socket timeout")
                time.sleep(.05)
            qmp = Qmp(qmp_path)
            qmp.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(document)))
            result["initial_snapshot"] = qmp.snapshot()
            qmp.call("cont")
            deadline = time.monotonic() + args.timeout
            while True:
                text = uart.read_text(errors="replace") if uart.exists() else ""
                if args.scenario == "disconnected-data":
                    # Disconnected inputs are physically tied to ground through
                    # explicit resistors. Require their exact observed hash,
                    # not merely any arbitrary error/mismatch/firmware abort.
                    mismatches = [r for r in records(text, "I2S_OBS")
                                  if r["case"] == "0" and r["phase"] == "sustained"]
                    if len(mismatches) == 2:
                        require({r["controller"] for r in mismatches} == {"0", "1"}, "missing negative receiver")
                        for record in mismatches:
                            require(record["match"] == "0/8" and int(record["bytes"]) == 1536,
                                    f"negative transfer count mismatch {record}")
                            require(record["observed"] == fnv1a(bytes(1536)),
                                    f"pulled-low native input is not low {record}")
                            require(int(record["txeof"]) >= 8 and int(record["rxeof"]) >= 8,
                                    f"negative input lacks actual clocked DMA {record}")
                        result["negative_observations"] = mismatches
                        result["status"] = "PASS_NEGATIVE"
                        break
                if "I2S_NATIVE_DONE " in text:
                    require(args.scenario == "connected", "disconnected nets unexpectedly transported exact data")
                    result["checks"] = validate(text)
                    result["status"] = "PASS_GPIO_CROSS_CONTROLLER"
                    break
                require(proc.poll() is None, f"QEMU exited {proc.returncode}")
                require(time.monotonic() < deadline, "firmware completion timeout (not a sample success)")
                require("abort() was called" not in text and "Guru Meditation" not in text, "firmware aborted; not a qualified negative sample observation")
                time.sleep(.1)
            qmp.call("stop")
            result["final_snapshot"] = qmp.snapshot()
            result["qmp_events"] = qmp.events
    except Exception as exc:
        result["error"] = repr(exc)
        if args.scenario == "external-peer":
            result["status"] = "UNQUALIFIED"
        raise
    finally:
        if qmp is not None:
            qmp.close()
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        result["source_flash_sha256_before"] = original_hash
        result["source_flash_sha256_after"] = sha256(original)
        result["source_flash_unchanged"] = result["source_flash_sha256_after"] == original_hash
        if not result["source_flash_unchanged"]:
            result["status"] = "FAIL"
            result["error"] = "original corpus image changed"
        if uart.exists():
            result["uart_sha256"] = sha256(uart)
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if qmp_path.exists():
            qmp_path.unlink()
    require(result["source_flash_unchanged"], "original corpus image changed")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), unqualified=result["unqualified"])))


if __name__ == "__main__":
    main()
