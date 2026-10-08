#!/usr/bin/env python3
"""Explicit serial acquisition and offline comparison; never flashes a board."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid

PREFIX = b"ESP32S3VM_REF "
HEX = re.compile(r"^[0-9a-f]+$")
MAX_LINE = 16384
MAX_RECORDS = 4096
TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def digest(path):
    with Path(path).open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(65536), b""):
            result.update(chunk)
        return result.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def hex_bytes(value, size):
    return isinstance(value, str) and len(value) == size * 2 and bool(HEX.fullmatch(value))


def validate_state(state):
    if not isinstance(state, dict) or set(state) != {"q", "qacc", "aux", "ar_deltas"}:
        raise ValueError("Incorrect state fields")
    for field, size in {"q": 128, "qacc": 40, "aux": 36, "ar_deltas": 8}.items():
        if not hex_bytes(state[field], size):
            raise ValueError(f"Invalid {field} byte state")


def parse_log(raw, manifest=None):
    """A batch requires one begin, contiguous sequence, declared count and end."""
    records, errors = [], []
    ended = False
    for line_no, line in enumerate(raw.splitlines(), 1):
        at = line.find(PREFIX)
        if at < 0:
            continue  # ROM/IDF logs remain in raw.log, outside the record channel.
        payload = line[at + len(PREFIX):]
        if len(payload) > MAX_LINE:
            errors.append(f"line {line_no}: record exceeds limit")
            continue
        try:
            record = json.loads(payload.decode("utf-8"), object_pairs_hook=unique_object,
                                parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
            if not isinstance(record, dict) or not integer(record.get("schema")) or record["schema"] != 1:
                raise ValueError("Unsupported record schema")
            if not integer(record.get("seq")) or record["seq"] != len(records):
                raise ValueError(f"Expected sequence {len(records)}")
            kind = record.get("type")
            if ended:
                raise ValueError("Record after batch end")
            if not records:
                if kind != "begin" or record.get("fixture") not in {"simd_reference", "radio_init"}:
                    raise ValueError("Batch must begin with a known fixture")
                count = record.get("expected_records")
                if not integer(count) or not 1 <= count <= MAX_RECORDS:
                    raise ValueError("Invalid declared record count")
                if not integer(record.get("seed")) or not 0 <= record["seed"] <= 0xffffffff:
                    raise ValueError("Invalid seed")
                if not hex_bytes(record.get("source_sha256"), 32) or not hex_bytes(record.get("elf_digest"), 32):
                    raise ValueError("Missing compiled source/ELF identity")
            elif kind == "vector" and records[0]["fixture"] == "simd_reference":
                if not isinstance(record.get("case"), str) or not isinstance(record.get("operation"), str):
                    raise ValueError("Missing vector identity")
                if not integer(record.get("vector_seed")) or not 0 <= record["vector_seed"] <= 0xffffffff:
                    raise ValueError("Invalid vector seed")
                if not hex_bytes(record.get("input_q"), 128) or not hex_bytes(record.get("input_qacc"), 40):
                    raise ValueError("Invalid input byte state")
                validate_state(record.get("before"))
                validate_state(record.get("after"))
            elif kind == "event" and records[0]["fixture"] == "radio_init":
                if record.get("phase") not in {"start", "result"} or not isinstance(record.get("stage"), str):
                    raise ValueError("Invalid radio event")
                if not isinstance(record.get("attempted"), bool) or not integer(record.get("err")):
                    raise ValueError("Invalid radio result")
                if record.get("status") not in {"enter", "ok", "error", "skipped"}:
                    raise ValueError("Invalid radio status")
                expected = "skipped" if not record["attempted"] else "enter" if record["phase"] == "start" else "ok" if record["err"] == 0 else "error"
                if record["status"] != expected:
                    raise ValueError("Radio status contradicts result")
                previous = records[-1]
                if record["phase"] == "result" and (previous.get("type") != "event" or
                        previous.get("phase") != "start" or previous.get("stage") != record["stage"] or
                        previous.get("attempted") != record["attempted"]):
                    raise ValueError("Unpaired radio result")
                if record["phase"] == "start" and previous.get("phase") == "start":
                    raise ValueError("Missing preceding radio result")
            elif kind == "end":
                if record.get("complete") is not True or not integer(record.get("records")) or record["records"] != len(records) - 1:
                    raise ValueError("Invalid completion count")
                if record["records"] != records[0]["expected_records"]:
                    raise ValueError("Incomplete declared batch")
                ended = True
            else:
                raise ValueError(f"Unexpected record type: {kind}")
            if len(records) >= MAX_RECORDS + 2:
                raise ValueError("Too many records")
            records.append(record)
        except (ValueError, UnicodeError, TypeError) as exc:
            errors.append(f"line {line_no}: {exc}")
    if not ended:
        errors.append("Missing complete batch end")
    if records and manifest:
        head = records[0]
        if head.get("fixture") != manifest.get("fixture"):
            errors.append("Manifest fixture mismatch")
        if head.get("source_sha256") != manifest.get("source_sha256"):
            errors.append("Compiled source identity differs from manifest")
        expected_elf = manifest.get("artifacts", {}).get("elf_sha256")
        if expected_elf and head.get("elf_digest") != expected_elf:
            errors.append("Firmware embedded ELF digest differs from manifest")
        if head.get("expected_records") != manifest.get("expected_records"):
            errors.append("Manifest count mismatch")
        if head.get("mode", "none") != manifest.get("mode", "none"):
            errors.append("Manifest mode mismatch")
        if manifest.get("expected_cases"):
            cases = [r.get("case") for r in records if r.get("type") == "vector"]
            if ended and cases != manifest["expected_cases"]:
                errors.append("Vector catalogue/order mismatch")
        if manifest.get("expected_stages"):
            stages = [r.get("stage") for r in records if r.get("type") == "event" and r.get("phase") == "start"]
            if ended and stages != manifest["expected_stages"]:
                errors.append("Radio stage catalogue/order mismatch")
    result_errors = [r for r in records if r.get("type") == "event" and r.get("status") in {"error", "skipped"}]
    return {"schema_version": 1, "complete": ended and not errors,
            "successful": ended and not errors and not result_errors,
            "errors": errors, "records": records,
            "last_stage": next((r.get("stage") for r in reversed(records) if r.get("type") == "event"), None)}


def inventory():
    """Port enumeration only; list_ports and registry access do not open ports."""
    try:
        from serial.tools import list_ports
        return [{"port": p.device, "description": p.description, "hwid": p.hwid,
                 "vid": p.vid, "pid": p.pid, "serial_number": p.serial_number}
                for p in list_ports.comports()]
    except ImportError:
        if os.name != "nt":
            return []
        import winreg
        rows = []
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                for index in range(winreg.QueryInfoKey(key)[1]):
                    name, port, _ = winreg.EnumValue(key, index)
                    rows.append({"port": port, "description": name, "metadata": "registry fallback"})
        except FileNotFoundError:
            pass
        return rows


def durable_write(stream, data):
    """FileIO is unbuffered, but an OS write may still consume only a prefix."""
    remaining = memoryview(data)
    while remaining:
        written = stream.write(remaining)
        if not written:
            raise OSError("Journal write made no progress")
        remaining = remaining[written:]
    os.fsync(stream.fileno())


def durable_new_file(path, data):
    with Path(path).open("xb", buffering=0) as stream:
        durable_write(stream, data)


class SerialJournal:
    """Durable recovery evidence; this is deliberately never a qualified capture.

    A sibling directory leaves write_capture's exclusive output creation intact.
    The initial identity/INCOMPLETE marker is durable before any port opens. Each
    bounded read is fsynced before it can be parsed. A forced process termination
    can skip every finally block and still leave those bytes and their identity.
    """
    def __init__(self, output, manifest_bytes, context, seconds, byte_limit):
        if not 1 <= byte_limit <= 64 * 1024 * 1024:
            raise ValueError("Journal byte limit exceeded")
        output = Path(output)
        self.path = output.with_name(output.name + ".journal")
        self.path.mkdir(parents=True, exist_ok=False)
        self.session_id = str(uuid.uuid4())
        self.byte_limit = byte_limit
        self.byte_count = 0
        self.failure = None
        self.stream = None
        self.manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
        identity = {"schema_version": 1, "state": "incomplete",
                    "complete": False, "successful": False,
                    "qualification": "Recovery journal only; no finalized capture or qualification",
                    "session_id": self.session_id, "tool_sha256": TOOL_SHA256,
                    "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "python": sys.version, "context": context,
                    "manifest_sha256": self.manifest_sha256,
                    "raw_file": "raw.partial.log", "final_output": str(output.resolve()),
                    "seconds": seconds, "byte_limit": byte_limit,
                    "durability": "Exclusive unbuffered writes; fsync after each received chunk"}
        identity_bytes = (json.dumps(identity, indent=2) + "\n").encode("utf-8")
        self.identity_sha256 = hashlib.sha256(identity_bytes).hexdigest()
        durable_new_file(self.path / "manifest.json", manifest_bytes)
        durable_new_file(self.path / "journal.json", identity_bytes)
        try:
            self.stream = (self.path / "raw.partial.log").open("xb", buffering=0)
            os.fsync(self.stream.fileno())
        except BaseException:
            self.close()
            raise

    def append(self, chunk):
        if not chunk:
            return
        try:
            if self.byte_count + len(chunk) > self.byte_limit:
                raise ValueError("Serial journal byte limit exceeded")
            durable_write(self.stream, chunk)
            self.byte_count += len(chunk)
        except Exception as exc:
            self.failure = str(exc)
            raise

    def close(self):
        if self.stream is not None:
            try:
                self.stream.close()
            finally:
                self.stream = None

    def finish(self, raw, reason):
        self.close()
        raw_path = self.path / "raw.partial.log"
        raw_sha256 = digest(raw_path)
        if (self.failure or len(raw) != self.byte_count or raw_path.stat().st_size != len(raw)
                or raw_sha256 != hashlib.sha256(raw).hexdigest()
                or digest(self.path / "manifest.json") != self.manifest_sha256
                or digest(self.path / "journal.json") != self.identity_sha256):
            raise OSError("Serial journal is incomplete or differs from received bytes: "
                          + (self.failure or "integrity mismatch"))
        result = {"path": str(self.path.resolve()), "session_id": self.session_id,
                  "state": "incomplete", "complete": False, "successful": False,
                  "reason": reason, "bytes": self.byte_count,
                  "raw_sha256": raw_sha256, "manifest_sha256": self.manifest_sha256,
                  "identity_sha256": self.identity_sha256,
                  "closed_at_utc": dt.datetime.now(dt.timezone.utc).isoformat()}
        # A closed journal remains incomplete even when its bytes happen to contain
        # an end record. Only the separate final capture has batch/integrity checks.
        durable_new_file(self.path / "stop.json",
                         (json.dumps(result, indent=2) + "\n").encode("utf-8"))
        return result


def capture_serial(port, baud, seconds, byte_limit, dtr=False, rts=False,
                   serial_factory=None, cancelled=None, on_open=None, on_bytes=None):
    if serial_factory is None:
        import serial  # Optional dependency is needed only for explicit acquisition.
        serial_factory = serial.Serial
    stop = cancelled or threading.Event()
    stream = serial_factory(port=None, baudrate=baud, timeout=0.2)
    raw, reason = bytearray(), "deadline"
    try:
        # Request control lines before opening; USB drivers can still pulse them.
        stream.dtr, stream.rts = dtr, rts
        stream.port = port
        stream.open()
        if on_open is not None:
            on_open()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not stop.is_set():
            remaining = byte_limit - len(raw)
            chunk = stream.read(min(4096, remaining))[:remaining]
            if on_bytes is not None and chunk:
                on_bytes(chunk)
            raw.extend(chunk)
            if len(raw) >= byte_limit:
                reason = "byte-limit"
                break
            if b'"type":"end"' in raw or b'"type": "end"' in raw:
                if parse_log(bytes(raw))["complete"]:
                    reason = "complete"
                    break
        if stop.is_set():
            reason = "cancelled"
    except KeyboardInterrupt:
        reason = "cancelled"
    except Exception as exc:
        reason = f"serial-error: {exc}"
    finally:
        try:
            stream.close()
        except Exception as exc:
            reason = f"serial-close-error: {exc}"
    return bytes(raw), reason


def write_capture(output, raw, reason, manifest_path, context, manifest_bytes=None):
    path = Path(output)
    path.mkdir(parents=True, exist_ok=False)
    if manifest_bytes is None:
        manifest_bytes = Path(manifest_path).read_bytes()
    manifest = json.loads(manifest_bytes)
    parsed = parse_log(raw, manifest)
    (path / "raw.log").write_bytes(raw)
    (path / "manifest.json").write_bytes(manifest_bytes)
    (path / "records.json").write_text(json.dumps(parsed, indent=2) + "\n", encoding="utf-8")
    metadata = {"schema_version": 1, "session_id": str(uuid.uuid4()),
                "captured_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "tool_sha256": TOOL_SHA256, "python": sys.version,
                "reason": reason, "context": context,
                "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                "manifest": manifest, "raw_sha256": digest(path / "raw.log"),
                "complete": parsed["complete"], "successful": parsed["successful"]}
    (path / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return parsed


def compare(reference, candidate, output):
    """Reject wrong input/build identity; retain every unequal field and raw log."""
    ref, cand = Path(reference), Path(candidate)
    rm = json.loads((ref / "metadata.json").read_text(encoding="utf-8"))
    cm = json.loads((cand / "metadata.json").read_text(encoding="utf-8"))
    rb, cb = (ref / "raw.log").read_bytes(), (cand / "raw.log").read_bytes()
    rp, cp = parse_log(rb, rm["manifest"]), parse_log(cb, cm["manifest"])
    differences = []
    for name, raw, meta in [("reference", rb, rm), ("candidate", cb, cm)]:
        if hashlib.sha256(raw).hexdigest() != meta.get("raw_sha256"):
            differences.append({"field": f"{name}.raw_sha256", "error": "Capture integrity mismatch"})
    for name, folder, meta in [("reference", ref, rm), ("candidate", cand, cm)]:
        if digest(folder / "manifest.json") != meta.get("manifest_sha256") or json.loads((folder / "manifest.json").read_bytes()) != meta["manifest"]:
            differences.append({"field": f"{name}.manifest_sha256", "error": "Manifest integrity mismatch"})
        diagnostic_hash = meta.get("context", {}).get("diagnostics_sha256")
        if diagnostic_hash and (not (folder / "qemu.log").is_file() or digest(folder / "qemu.log") != diagnostic_hash):
            differences.append({"field": f"{name}.diagnostics_sha256", "error": "Diagnostic trace integrity mismatch"})
    if not rp["complete"] or not cp["complete"]:
        differences.append({"field": "batch", "reference": rp["errors"], "candidate": cp["errors"]})
    for key in {"fixture", "source_sha256", "expected_records", "artifacts", "source_files", "mode"}:
        if rm["manifest"].get(key) != cm["manifest"].get(key):
            differences.append({"field": f"manifest.{key}", "reference": rm["manifest"].get(key),
                                "candidate": cm["manifest"].get(key)})
    if rp["records"] and cp["records"]:
        for key in {"fixture", "seed", "elf_digest", "source_sha256", "mode"}:
            a, b = rp["records"][0].get(key), cp["records"][0].get(key)
            if a != b:
                differences.append({"field": f"header.{key}", "reference": a, "candidate": b})
        if len(rp["records"]) != len(cp["records"]):
            differences.append({"field": "record_count", "reference": len(rp["records"]), "candidate": len(cp["records"])})
        for index, (a, b) in enumerate(zip(rp["records"][1:], cp["records"][1:]), 1):
            if a != b:
                # Full records preserve raw bytes, identity and neighboring sequence.
                differences.append({"field": f"records[{index}]", "reference": a, "candidate": b})
    report = {"schema_version": 1, "equal": not differences, "differences": differences,
              "reference_successful": rp["successful"], "candidate_successful": cp["successful"],
              "qualification": "byte comparison only; does not qualify an entire ISA/peripheral"}
    destination = Path(output)
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for label, source in [("reference", ref), ("candidate", cand)]:
        copied = destination / label
        copied.mkdir()
        for file in ["raw.log", "metadata.json", "manifest.json", "records.json"]:
            shutil.copyfile(source / file, copied / file)
        if (source / "qemu.log").is_file():
            shutil.copyfile(source / "qemu.log", copied / "qemu.log")
    return report


def qmp_snapshot(port, enable_write_trace=False):
    """Read-only registers after pausing an already finished diagnostic run."""
    messages = []
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1) as connection:
            connection.settimeout(1)
            stream = connection.makefile("rwb")
            greeting = stream.readline(MAX_LINE)
            messages.append({"greeting": json.loads(greeting)})
            def request(execute, arguments=None):
                identity = str(len(messages))
                message = {"execute": execute, "id": identity}
                if arguments is not None:
                    message["arguments"] = arguments
                stream.write(json.dumps(message).encode() + b"\n")
                stream.flush()
                for _ in range(20):
                    reply = json.loads(stream.readline(MAX_LINE))
                    if reply.get("id") == identity:
                        messages.append({"request": message, "reply": reply})
                        return reply
                raise ValueError("Bounded QMP reply wait exhausted")
            request("qmp_capabilities")
            if enable_write_trace:
                request("trace-event-set-state", {"name": "memory_region_ops_write", "enable": True})
            else:
                request("stop")
                request("human-monitor-command", {"command-line": "info registers -a"})
            stream.close()
    except (OSError, ValueError) as exc:
        messages.append({"error": str(exc)})
    return messages


def capture_qemu(args, output, manifest_bytes):
    """Local QEMU fixture run with fresh flash; no serial hardware involved."""
    path = Path(output)
    path.mkdir(parents=True, exist_ok=False)
    flash = path / "flash.bin"
    shutil.copyfile(args.firmware, flash)
    if digest(flash) != json.loads(manifest_bytes)["artifacts"]["flash_sha256"]:
        raise ValueError("Copied flash differs from frozen manifest; artifact changed during acquisition")
    command = [args.qemu_bin, "-accel", "tcg", "-machine", "esp32s3",
               "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
               "-display", "none", "-nographic",
               "-monitor", "none", "-serial", "stdio", "-drive",
               f"file={str(flash).replace(',', ',,')},if=mtd,format=raw"]
    context = {"transport": "qemu", "command": command, "qemu_sha256": digest(args.qemu_bin),
               "firmware_sha256": digest(flash), "efuse": "runtime default; no persistent eFuse image"}
    qmp_port = None
    if args.qemu_diagnostics or args.qemu_mmio_writes:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            qmp_port = reservation.getsockname()[1]
        command += ["-d", "unimp,guest_errors", "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off"]
        context["diagnostics"] = "bounded unimp/guest_errors; QMP stop and registers after run deadline"
    if args.qemu_mmio_writes:
        context["write_trace_policy"] = "Enable after first fixture record; excludes ROM boot writes"
    if args.efuse:
        efuse = path / "efuse.bin"
        shutil.copyfile(args.efuse, efuse)
        command += ["-drive", f"file={str(efuse).replace(',', ',,')},if=none,format=raw,id=efuse",
                    "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse"]
        context["efuse"] = {"sha256": digest(efuse), "copied_fixture": str(efuse)}
    raw, diagnostics, reason = bytearray(), bytearray(), "deadline"
    # Keep UART frontend connected even when the parent executor has stdin EOF.
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lock, done, stopping = threading.Lock(), threading.Event(), threading.Event()
    trace_started = threading.Event()
    def reader(stream, destination, framed):
        nonlocal reason
        while True:
            chunk = stream.read1(4096)
            if not chunk:
                if framed and not stopping.is_set():
                    reason = "process-exit"
                if framed:
                    done.set()
                return
            with lock:
                remaining = args.byte_limit - len(raw) - len(diagnostics)
                destination.extend(chunk[:remaining])
                if len(raw) + len(diagnostics) >= args.byte_limit:
                    reason = "byte-limit"
                    done.set()
                    # Continue draining until termination so diagnostic writers
                    # cannot block the QEMU main loop and the QMP snapshot.
                if framed and b'"type":"end"' in raw and parse_log(bytes(raw))["complete"]:
                    reason = "complete"
                    done.set()
                    return
            if framed and args.qemu_mmio_writes and not trace_started.is_set() and PREFIX in raw:
                trace_started.set()
                context["trace_enable"] = qmp_snapshot(qmp_port, enable_write_trace=True)
    threads = [threading.Thread(target=reader, args=(process.stdout, raw, True), daemon=True),
               threading.Thread(target=reader, args=(process.stderr, diagnostics, False), daemon=True)]
    for thread in threads:
        thread.start()
    try:
        done.wait(args.seconds)
    except KeyboardInterrupt:
        reason = "cancelled"
    finally:
        stopping.set()
        if qmp_port is not None and process.poll() is None:
            context["qmp_snapshot"] = qmp_snapshot(qmp_port)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        for thread in threads:
            thread.join(timeout=3)
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
    context["exit_code"] = process.returncode
    # Acquisition created path for isolated flash; evidence is a fresh child.
    context["diagnostics_sha256"] = hashlib.sha256(diagnostics).hexdigest()
    context["diagnostics_bytes"] = len(diagnostics)
    parsed = write_capture(path / "capture", bytes(raw), reason, args.manifest, context, manifest_bytes)
    (path / "capture/qemu.log").write_bytes(diagnostics)
    return parsed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--inventory", action="store_true")
    mode.add_argument("--capture", action="store_true")
    mode.add_argument("--parse", type=Path)
    mode.add_argument("--compare", nargs=2, metavar=("REFERENCE", "CANDIDATE"))
    mode.add_argument("--qemu", action="store_true")
    parser.add_argument("--port")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--byte-limit", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--dtr", type=int, choices=[0, 1], default=0)
    parser.add_argument("--rts", type=int, choices=[0, 1], default=0)
    parser.add_argument("--board-context", help="User-supplied board/revision/instrument metadata JSON")
    parser.add_argument("--qemu-bin")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--efuse", type=Path)
    parser.add_argument("--qemu-diagnostics", action="store_true", help="Bounded MMIO diagnostics and paused register snapshot")
    parser.add_argument("--qemu-mmio-writes", action="store_true", help="Trace real MMIO writes into bounded qemu.log")
    args = parser.parse_args(argv)
    if not 0 < args.seconds <= 600 or not 1 <= args.byte_limit <= 64 * 1024 * 1024 or args.baud <= 0:
        parser.error("Capture duration/byte/baud bounds exceeded")
    if args.inventory:
        print(json.dumps({"ports": inventory(), "opened": False}, indent=2))
        return 0
    if not args.output or args.output.exists():
        parser.error("Supply a new --output directory; existing evidence is never overwritten")
    if args.compare:
        report = compare(*args.compare, args.output)
        print(json.dumps({"equal": report["equal"], "differences": len(report["differences"])}))
        return 0 if report["equal"] and report["reference_successful"] and report["candidate_successful"] else 1
    if not args.manifest or not args.manifest.is_file():
        parser.error("Supply a built fixture --manifest")
    # Validate provenance before any port open or emulator launch.
    manifest_bytes = args.manifest.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get("schema_version") != 1 or not hex_bytes(manifest.get("source_sha256"), 32):
        parser.error("Invalid fixture manifest")
    if args.capture:
        if not args.port:
            parser.error("Serial acquisition requires explicit --capture and --port")
        context = {"transport": "serial", "port": args.port, "baud": args.baud,
                   "requested_dtr": bool(args.dtr), "requested_rts": bool(args.rts),
                   "reset_warning": "Driver control-line pulses are possible; no flash/reset command is issued",
                   "ports": inventory(), "board": json.loads(args.board_context) if args.board_context else {"identity": "not independently supplied"}}
        journal = SerialJournal(args.output, manifest_bytes, context, args.seconds, args.byte_limit)
        try:
            raw, reason = capture_serial(args.port, args.baud, args.seconds, args.byte_limit,
                                         bool(args.dtr), bool(args.rts), on_bytes=journal.append,
                                         on_open=lambda:
                                         print(f"Capture open on {args.port}; waiting for the approved fixture run. No automatic reset or flash is issued.", flush=True))
            context["acquisition_journal"] = journal.finish(raw, reason)
        finally:
            journal.close()
        parsed = write_capture(args.output, raw, reason, args.manifest, context, manifest_bytes)
    elif args.parse:
        if args.parse.stat().st_size > args.byte_limit:
            parser.error("Offline log exceeds --byte-limit")
        parsed = write_capture(args.output, args.parse.read_bytes(), "offline-import", args.manifest,
                               {"transport": "import", "source": str(args.parse.resolve()),
                                "hardware_provenance": "not established by importing a log"}, manifest_bytes)
    else:
        if not args.qemu_bin or not args.firmware or not args.firmware.is_file():
            parser.error("QEMU run requires --qemu-bin and --firmware")
        if manifest.get("artifacts", {}).get("flash_sha256") != digest(args.firmware):
            parser.error("Firmware SHA256 differs from manifest")
        parsed = capture_qemu(args, args.output, manifest_bytes)
    print(json.dumps({"complete": parsed["complete"], "successful": parsed["successful"],
                      "errors": parsed["errors"], "last_stage": parsed["last_stage"]}))
    return 0 if parsed["successful"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, ImportError) as exc:
        print(f"Reference job failed: {exc}", file=sys.stderr)
        sys.exit(2)
