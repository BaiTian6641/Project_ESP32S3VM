#!/usr/bin/env python3
"""Execute the corrected ordinary-IDF 124-case image with actual pin-loop RX.

Linux/WSL usage: python3 wire_verify.py --qemu NATIVE_BINARY --flash MERGED_BIN
  --elf MATCHING_ELF --firmware-manifest BUILD_RESULT_JSON --gdb XTENSA_GDB
  --evidence NEW_DIRECTORY
Use --scenario floating-data or disconnected-data for real graph negatives.
The original image is hash-checked before/after; only a new copy is writable.
Read-only GDB source breakpoints attribute finite recorder ranges, not ISR
cadence or an unlabeled combined stream. Each range retains actual MMIO config.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

from expected_vectors import matrix
from graph_vectors import component, project, terminal
from record_reference import Segment, logical_frame, qualify_segment, read_capture
from run_fixture import Qmp, records, require, sha256, validate

WITHDRAWN_WIRE_FLASH_SHA256 = "965df0d22949e21a8bc0afcb1577a9c8740d57f857804237bed9735d121aa066"


def firmware_identity(manifest_path, flash, elf):
    manifest_path = manifest_path.resolve()
    manifest = json.loads(manifest_path.read_text())
    artifacts = manifest["artifacts"]
    for kind, actual in (("merged_flash", flash), ("elf", elf)):
        entry = artifacts[kind]
        declared = Path(entry["path"])
        declared = declared if declared.is_absolute() else manifest_path.parent / declared
        require(declared.resolve() == actual.resolve(), f"firmware manifest {kind} path mismatch")
        require(sha256(actual) == entry["sha256"] and actual.stat().st_size == entry["bytes"],
                f"firmware manifest {kind} bytes mismatch")
    require(artifacts["merged_flash"]["sha256"] != WITHDRAWN_WIRE_FLASH_SHA256,
            "old wire965 image is build-only; corrected TDM image required")
    entry = artifacts["firmware_c_source"]
    source = Path(entry["path"])
    source = source if source.is_absolute() else manifest_path.parent / source
    require(sha256(source) == entry["sha256"], "immutable firmware source snapshot changed")
    return source.resolve(), dict(manifest_path=str(manifest_path), manifest_sha256=sha256(manifest_path),
                                  source_path=str(source.resolve()), source_sha256=entry["sha256"],
                                  debugger_manifest=manifest["debugger_manifest"])


def boundary_plan(source):
    lines = source.read_text().splitlines()

    def find(statement, after=0, before=None):
        matches = [index + 1 for index in range(after, before or len(lines))
                   if lines[index].strip() == statement]
        require(len(matches) == 1, f"firmware boundary statement is not unique: {statement}")
        return matches[0]

    normal_end = find("static void pdm_capabilities(void)")
    raw_begin = find("static void raw_pdm(unsigned source, unsigned master)")
    raw_end = find("static void tdm_frame_limits(void)")
    entries = []
    for phase in ("sustained", "starved", "restart"):
        statement = next((line.strip() for line in lines[:normal_end]
                          if line.strip().startswith(f'capture(case_id, "{phase}",')), None)
        require(statement is not None, f"missing actual {phase} capture statement")
        begin = find(statement, before=normal_end)
        if phase == "sustained":
            end = find("uint32_t before = counts[master].sent;", after=begin, before=normal_end)
        else:
            candidates = [index + 1 for index in range(begin, normal_end)
                          if lines[index].strip() == "stop_pair(tx, rx, master);"]
            require(candidates, f"missing actual {phase} stop statement")
            end = candidates[0]
        entries.extend([dict(line=begin, phase=phase, edge="begin", raw_pdm=False),
                        dict(line=end, phase=phase, edge="end", raw_pdm=False)])
    entries.extend([dict(line=find("size_t moved = 0;", after=raw_begin, before=raw_end),
                         phase="sustained", edge="begin", raw_pdm=True),
                    dict(line=find("ESP_ERROR_CHECK(i2s_channel_disable(tx));", after=raw_begin, before=raw_end),
                         phase="sustained", edge="end", raw_pdm=True)])
    return dict(source=str(source), boundaries=entries,
                complete_line=find("for (;;) vTaskDelay(pdMS_TO_TICKS(1000));"))


def graph(scenario):
    document = project("disconnected-data" if scenario == "disconnected-data" else "connected")
    ground = next(net for net in document["nets"] if net["id"] == "gnd")
    # GPIO_IN is genuinely read by the ROM before any I2S output is enabled.
    # Physically resolve data idle too; 40-ohm TX later overrides these pulls.
    for source in (20, 6):
        net_id = f"tx{source}" if scenario == "disconnected-data" else f"data{source}"
        net = next(net for net in document["nets"] if net["id"] == net_id)
        cid = f"Idledata{source}"
        document["components"].append(component(cid, "resistor", [
            terminal(cid, "a", "passive"), terminal(cid, "b", "passive")],
            {"resistance": {"value": 10000, "unit": "ohm"}}))
        ground["endpoints"].append(f"{cid}.b")
        net["endpoints"].append(f"{cid}.a")
    if scenario == "floating-data":
        document["id"] = "i2s-native-floating-data"
        for source, destination in ((20, 7), (6, 21)):
            net = next(net for net in document["nets"] if net["id"] == f"data{source}")
            net["endpoints"].remove(f"U1.io{destination}")
            document["nets"].append(dict(id=f"floating{destination}", name=f"floating{destination}",
                                         endpoints=[f"U1.io{destination}"]))
    return document


def physical_qualification(evidence, text):
    rows = matrix()
    require(len(rows) == 124, "124-case wire reference unavailable")
    boundaries = [json.loads(line) for line in (evidence / "boundaries.jsonl").read_text().splitlines()]
    require(not any("error" in entry for entry in boundaries), "guest boundary attribution failed")
    require(sum(bool(entry.get("complete")) for entry in boundaries) == 1, "ordinary guest completion breakpoint missing")
    serial_cases = {int(row["case"]): row for row in records(text, "I2S_CASE")}
    require(set(serial_cases) == set(range(124)), "missing serial CASE attribution")
    starts, stops = {}, {}
    for entry in boundaries:
        if entry.get("complete"):
            continue
        case = entry["case"]
        require(case in serial_cases, "guest boundary case lacks ordinary serial CASE")
        row = rows[case]
        require(entry["raw_pdm"] == (row["mode"] == "pdm"),
                "guest boundary source function disagrees with serial case mode")
        entry["serial_case"] = serial_cases[case]
        target = starts if entry["edge"] == "begin" else stops
        key = (entry["case"], entry["phase"])
        require(key not in target, f"duplicate actual boundary {key}")
        target[key] = entry
    expected = {(row["case"], phase) for row in rows for phase in
                (("sustained",) if row["mode"] == "pdm" else ("sustained", "starved", "restart"))}
    require(set(starts) == expected and set(stops) == expected, "missing finite per-case phase ranges")
    captures = [read_capture(evidence / "rx" / f"i2s{controller}.rx.bin") for controller in (0, 1)]
    reports, failures = [], []
    plans = [[], []]
    for case, phase in sorted(expected):
        row = rows[case]
        receivers = (1 - row["source"],) if row["mode"] == "pdm" else (0, 1)
        for controller in receivers:
            start = starts[(case, phase)]["event_counts"][controller]
            stop = stops[(case, phase)]["event_counts"][controller]
            capture = captures[controller]
            original_range = dict(start=start, stop=stop)
            try:
                require(0 <= start < stop <= len(capture.events), "empty or invalid observed physical range")
                active = [slot for slot in range(8) if row["mask"] & (1 << slot)]
                # Guest CPU breakpoints can fall between physical slot edges.
                # Omit only boundary partial frames, never interior failures.
                while start < stop and (capture.events[start].slot != active[0] or
                                         logical_frame(capture.events[start], row) < 0):
                    start += 1
                while stop > start and capture.events[stop - 1].slot != active[-1]:
                    stop -= 1
                segment = Segment(case=case, start=start, stop=stop, boundary="start" if phase == "restart" else "selection",
                                  drops="allow")
                report = qualify_segment(capture, segment)
                report.update(case=case, phase=phase, controller=controller,
                              guest_boundary_range=original_range,
                              omitted_boundary_events=(start - original_range["start"] + original_range["stop"] - stop),
                              attribution="serial CASE plus read-only guest source breakpoint and live MMIO configuration",
                              start_configuration=starts[(case, phase)]["actual_i2s_configuration"],
                              stop_configuration=stops[(case, phase)]["actual_i2s_configuration"])
                reports.append(report)
                plans[controller].append(dict(case=case, start=start, stop=stop, boundary=segment.boundary, drops="allow"))
            except Exception as exc:
                failures.append(dict(case=case, phase=phase, controller=controller,
                                     guest_boundary_range=original_range, error=repr(exc)))
    for controller, segments in enumerate(plans):
        (evidence / f"capture-plan-i2s{controller}.json").write_text(json.dumps(dict(segments=segments), indent=2) + "\n")
    report = dict(status="PASS_PHYSICAL_RX124" if not failures else "FAIL_PHYSICAL_RX",
                  case_count=124, expected_selected_segments=724,
                  qualified_segments=len(reports), failed_segments=failures, segments=reports,
                  actual_record_sha256={f"i2s{controller}": capture.sha256 for controller, capture in enumerate(captures)},
                  full_matrix_qualified=not failures,
                  unqualified=["PCM2PDM/PDM2PCM exact converter output", "external peer: separate 68-case runner"],
                  evidence_kind="actual resolved RX samples and physical timestamp cadence; not CPU ISR cadence")
    (evidence / "physical-reference.json").write_text(json.dumps(report, indent=2) + "\n")
    require(not failures, f"{len(failures)} finite physical sample/cadence segments failed")
    return dict(qualified_segments=len(reports), cases=124, record_hashes=report["actual_record_sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "flash", "elf", "firmware-manifest", "gdb", "evidence"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--scenario", choices=("connected", "disconnected-data", "floating-data"), default="connected")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    (evidence / "rx").mkdir()
    original = args.flash.resolve()
    before = sha256(original)
    result = dict(status="FAIL", scenario=args.scenario, source_flash_sha256_before=before,
                  hardware_used=False, unqualified=["PCM2PDM/PDM2PCM exact converter output"])
    proc = observer = qmp = None
    log = observer_log = None
    try:
        source, identity = firmware_identity(args.firmware_manifest, original, args.elf)
        result["firmware_identity"] = identity
        (evidence / "boundary-plan.json").write_text(json.dumps(boundary_plan(source), indent=2) + "\n")
        local = evidence / "fixture-flash.bin"
        shutil.copyfile(original, local)
        require(sha256(local) == before, "local flash copy changed")
        document = graph(args.scenario)
        (evidence / "graph.json").write_text(json.dumps(document, indent=2) + "\n")
        boot_document = document
        (evidence / "boot-graph.json").write_text(json.dumps(boot_document, indent=2) + "\n")
        (evidence / "expected-matrix.json").write_text(json.dumps(matrix(), indent=2) + "\n")
        qmp_path = evidence / "qmp.sock"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        command = [str(args.qemu.resolve()), "-machine", "esp32s3", "-nographic", "-S",
                   "-icount", "shift=0,align=off,sleep=off",
                   "-serial", f"file:{evidence / 'uart.log'}", "-drive", f"file={local},if=mtd,format=raw",
                   "-qmp", f"unix:{qmp_path},server=on,wait=off", "-gdb", f"tcp:127.0.0.1:{port}",
                   "-global", f"esp32s3-i2s.record-directory={evidence / 'rx'}", "-d", "guest_errors"]
        result.update(command=command, qemu_sha256=sha256(args.qemu), elf_sha256=sha256(args.elf))
        environment = dict(os.environ, I2S_WIRE_EVIDENCE=str(evidence), I2S_WIRE_SCENARIO=args.scenario)
        (evidence / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
        log = (evidence / "qemu.log").open("wb")
        proc = subprocess.Popen(command, stdout=log, stderr=log, env=environment)
        deadline = time.monotonic() + 30
        while not qmp_path.exists():
            require(proc.poll() is None and time.monotonic() < deadline, "QEMU/QMP startup failed")
            time.sleep(.05)
        qmp = Qmp(qmp_path)
        qmp.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(boot_document)))
        result["initial_snapshot"] = qmp.snapshot()
        if args.scenario == "connected":
            observer_command = [str(args.gdb.resolve()), "-nx", "--batch", str(args.elf.resolve()),
                                "-ex", "set pagination off", "-ex", "set confirm off",
                                "-ex", f"target remote 127.0.0.1:{port}",
                                "-ex", f"source {Path(__file__).with_name('wire_gdb_boundaries.py').resolve()}"]
            result["observer_command"] = observer_command
            result["gdb_sha256"] = sha256(args.gdb)
            observer_log = (evidence / "gdb.log").open("wb")
            observer = subprocess.Popen(observer_command, stdout=observer_log, stderr=observer_log, env=environment)
        else:
            qmp.call("cont")
        deadline = time.monotonic() + args.timeout
        while True:
            uart = evidence / "uart.log"
            text = uart.read_text(errors="replace") if uart.exists() else ""
            if args.scenario == "connected" and observer is not None and observer.poll() is not None:
                require(observer.returncode == 0, "guest attribution observer failed")
                if "I2S_NATIVE_DONE " in text:
                    result["console_checks"] = validate(text)
                    result["status"] = "PENDING_PHYSICAL_REFERENCE"
                    break
            if args.scenario == "disconnected-data" and len(records(text, "I2S_OBS")) >= 2:
                observations = records(text, "I2S_OBS")[:2]
                require(all(row["case"] == "0" and row["phase"] == "sustained" and row["match"] == "0/8"
                            for row in observations), "disconnected graph did not produce exact mismatching first-row observations")
                result["negative_observations"] = observations
                result["status"] = "PENDING_NEGATIVE_REFERENCE"
                break
            if args.scenario == "floating-data":
                native_log = (evidence / "qemu.log").read_text(errors="replace")
                if ("consumed input floating, contended or unknown" in native_log or
                        "solver-owned pad(s) sampled UNKNOWN" in native_log):
                    require(any(row.get("case") == "0" for row in records(text, "I2S_CASE")),
                            "floating graph paused during boot, not an attributed I2S input consumption")
                    result["status"] = "PENDING_FLOATING_REFERENCE"
                    break
            if args.scenario != "floating-data":
                native_log = (evidence / "qemu.log").read_text(errors="replace")
                require("solver-owned pad(s) sampled UNKNOWN" not in native_log,
                        "actual GPIO consumption paused on an unresolved owned net")
            require(proc.poll() is None, f"QEMU exited {proc.returncode}")
            require(time.monotonic() < deadline, "ordinary firmware/observer timeout")
            require("abort() was called" not in text and "Guru Meditation" not in text, "ordinary firmware aborted")
            time.sleep(.05)
        qmp.call("stop")
        result["final_snapshot"] = qmp.snapshot()
    except Exception as exc:
        result.update(status="FAIL", error=repr(exc))
    finally:
        if observer is not None and observer.poll() is None:
            observer.terminate()
            try:
                observer.wait(10)
            except subprocess.TimeoutExpired:
                observer.kill()
                observer.wait()
        if qmp is not None:
            try:
                qmp.call("quit")
            except (OSError, ValueError):
                pass
            qmp.close()
        if proc is not None and proc.poll() is None:
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        for stream in (log, observer_log):
            if stream:
                stream.close()
        try:
            if result["status"] == "PENDING_PHYSICAL_REFERENCE":
                result["physical_checks"] = physical_qualification(evidence, text)
                result["status"] = "PASS_ORDINARY_WIRE124_PHYSICAL_RX"
            elif result["status"] == "PENDING_NEGATIVE_REFERENCE":
                captures = [read_capture(evidence / "rx" / f"i2s{controller}.rx.bin") for controller in (0, 1)]
                require(all(len(capture.events) >= 1536 and all(event.sample == 0 for event in capture.events)
                            for capture in captures), "actual disconnected inputs were not clocked exact zero")
                result["status"] = "PASS_DISCONNECTED_PULLED_LOW"
            elif result["status"] == "PENDING_FLOATING_REFERENCE":
                require(all((evidence / "rx" / f"i2s{controller}.rx.bin").stat().st_size == 16 for controller in (0, 1)),
                        "floating inputs invented actual valid RX samples")
                require(any(row.get("case") == "0" for row in records(text, "I2S_CASE")),
                        "floating negative has no actual ordinary I2S case attribution")
                result["status"] = "PASS_FLOATING_FAIL_CLOSED"
        except Exception as exc:
            result.update(status="FAIL", error=repr(exc))
        result["source_flash_sha256_after"] = sha256(original)
        result["source_flash_unchanged"] = before == result["source_flash_sha256_after"]
        if not result["source_flash_unchanged"]:
            result.update(status="FAIL", error="original image changed")
        result["evidence_hashes"] = {str(path.relative_to(evidence)): sha256(path)
                                     for path in evidence.rglob("*") if path.is_file()}
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence))))
    return 0 if result["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
