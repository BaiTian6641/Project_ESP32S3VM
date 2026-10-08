#!/usr/bin/env python3
"""Drive only public QMP Apply; ordinary IDF firmware supplies all GPIO activity."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time


def terminal(cid, role, domain, gpio=None):
    t = dict(id=f"{cid}.{role}", name=role, role=role, domain=domain,
             direction="unspecified")
    if gpio is not None:
        t["gpio"] = gpio
    return t


def project(closed):
    def comp(cid, kind, terms, parameters=None):
        return dict(id=cid, name=cid, type=kind, kind=kind,
                    terminals=terms, parameters=parameters or {})
    def quantity(value, unit):
        return dict(value=value, unit=unit)
    components = [
        comp("U1", "mcu", [terminal("U1", "vdd", "power"),
             terminal("U1", "gnd", "ground"), terminal("U1", "io4", "digital", 4),
             terminal("U1", "io5", "digital", 5)]),
        comp("G", "ground", [terminal("G", "ref", "ground")]),
        comp("V", "voltage-source", [terminal("V", "p", "power"),
             terminal("V", "n", "ground")], {"voltage": quantity(3.3, "V")}),
        comp("Pull", "resistor", [terminal("Pull", "a", "passive"),
             terminal("Pull", "b", "passive")], {"resistance": quantity(10000, "ohm")}),
        comp("SW", "switch", [terminal("SW", "a", "passive"),
             terminal("SW", "b", "passive")], {"state": quantity("closed" if closed else "open", "state"),
             "on_resistance": quantity(10, "ohm")}),
    ]
    nets = [dict(id=cid, name=cid, endpoints=ends) for cid, ends in [
        ("gnd", ["G.ref", "V.n", "U1.gnd", "Pull.b"]),
        ("vdd", ["V.p", "U1.vdd"]),
        ("out", ["U1.io4", "SW.a"]),
        ("input", ["SW.b", "U1.io5", "Pull.a"]),
    ]]
    return dict(version=3, id="ordinary-native-fixture", name="Ordinary native fixture",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
                firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
                components=components, nets=nets, geometry=dict(components={}, nets={}))


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)
        self.events = []
        greeting = json.loads(self.stream.readline())
        assert "QMP" in greeting, greeting
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        self.stream.write((json.dumps(dict(execute=command, arguments=arguments or {})) + "\n").encode())
        while True:
            result = json.loads(self.stream.readline())
            if "event" in result:
                self.events.append(result)
                continue
            if "error" in result:
                raise RuntimeError(result)
            return result["return"]

    def snapshot(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))

    def apply(self, document):
        self.call("stop")
        self.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(document)))

    def close(self):
        self.stream.close()
        self.sock.close()


def wait_for(log, predicate, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace") if log.exists() else ""
        if predicate(text):
            return text
        time.sleep(.05)
    raise AssertionError(f"UART condition timed out: {log}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--flash", required=True)
    parser.add_argument("--evidence", required=True)
    args = parser.parse_args()
    evidence = pathlib.Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    socket_dir = pathlib.Path(tempfile.mkdtemp(dir="/tmp", prefix="s3-elec-"))
    qmp_path = socket_dir / "qmp.sock"
    uart = socket_dir / "uart.log"
    archived_uart = evidence / "fixture-uart.log"
    stderr = evidence / "fixture-stderr.log"
    command = [args.qemu, "-machine", "esp32s3", "-nographic", "-S", "-serial", f"file:{uart}",
               "-drive", f"file={args.flash},if=mtd,format=raw", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    result = dict(command=command, checks=[], snapshots=[])
    qmp = None
    with stderr.open("w") as errors:
        proc = subprocess.Popen(command, stdout=errors, stderr=errors)
        try:
            deadline = time.monotonic() + 20
            while not qmp_path.exists() and time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited {proc.returncode}")
                time.sleep(.05)
            qmp = Qmp(qmp_path)
            qmp.apply(project(False))
            result["snapshots"].append(qmp.snapshot())
            qmp.call("cont")
            wait_for(uart, lambda text: "NATIVE_ELECTRICAL_READY" in text)
            wait_for(uart, lambda text: len(re.findall(r"OBS tick=\d+ out=1 input=0 irq=0", text)) >= 2)
            result["checks"].append("open switch: firmware output high never bypasses disconnected net; input0,irq0")
            qmp.apply(project(True))
            closed_offset = uart.stat().st_size
            qmp.call("cont")
            pattern = r"OBS tick=\d+ out=(\d) input=(\d) irq=(\d+) irq_level=(\d)"
            def connected_ready(text):
                readings = re.findall(pattern, text[closed_offset:])
                levels = {out for out, inp, irq, _ in readings if out == inp and int(irq) > 0}
                return levels == {"0", "1"} and any(int(irq) >= 2 for _, _, irq, _ in readings)
            closed = wait_for(uart, connected_ready)
            observations = re.findall(pattern, closed[closed_offset:])
            assert any(out == inp == "1" and int(irq) > 0 for out, inp, irq, _ in observations), observations
            assert any(out == inp == "0" and int(irq) > 0 for out, inp, irq, _ in observations), observations
            result["checks"].append("closed switch: ordinary gpio_get_level follows both levels and CPU ISR count rises")
            qmp.call("stop")
            result["snapshots"].append(qmp.snapshot())
            before = result["snapshots"][-1]
            qmp.apply(project(False))
            open_offset = uart.stat().st_size
            after = qmp.snapshot()
            result["snapshots"].append(after)
            assert int(after["generation"]) == int(before["generation"]) + 1
            qmp.call("cont")
            reopened = wait_for(uart, lambda text: len(re.findall(pattern, text[open_offset:])) >= 4)
            readings = re.findall(pattern, reopened[open_offset:])
            assert all(inp == "0" for _, inp, _, _ in readings), readings
            # A falling edge at Apply is allowed; thereafter output activity cannot generate input IRQs.
            assert len({irq for _, _, irq, _ in readings[1:]}) == 1, readings
            result["checks"].append("reopened switch: input held0 by explicit resistor; no subsequent cross-net IRQ")
            qmp.call("stop")
            result["snapshots"].append(qmp.snapshot())
            result["events"] = qmp.events
            result["status"] = "PASS"
        except Exception as exc:
            result["status"] = "FAIL"
            result["error"] = repr(exc)
            if qmp is not None:
                for key, capture in (("failure_runstate", lambda: qmp.call("query-status")),
                                     ("failure_snapshot", qmp.snapshot)):
                    try:
                        result[key] = capture()
                    except Exception as diagnostic:
                        result[key + "_capture_error"] = repr(diagnostic)
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if qmp is not None:
                qmp.close()
            # Producer is closed. On archive failure the native directory is
            # intentionally retained: never delete the only live UART copy.
            if uart.exists():
                archived_uart.write_bytes(uart.read_bytes())
            result["hashes"] = {str(path): hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
                                for path in [args.qemu, args.flash, str(archived_uart)] if pathlib.Path(path).exists()}
            (evidence / "fixture-result.json").write_text(json.dumps(result, indent=2) + "\n")
            shutil.rmtree(socket_dir)


if __name__ == "__main__":
    main()
