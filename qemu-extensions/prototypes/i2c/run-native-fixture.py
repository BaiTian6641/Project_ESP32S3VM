#!/usr/bin/env python3
"""Apply a pure v3 graph through public QMP; observe ordinary IDF UART only.

Each invocation creates a fresh evidence directory. The flashed fixture must
use the profile and pads in --sdkconfig; the runner never changes firmware,
registers, GPIO routing, provider state, or firmware timeout configuration.
"""
import argparse
import hashlib
import json
import pathlib
import re
import socket
import subprocess
import tempfile
import time


PROFILES = {
    "connected": "CONNECTED",
    "wrong": "WRONG",
    "disconnected": "DISCONNECTED",
    "no_pull": "NO_PULL",
    "stuck_sda": "STUCK",
    "stuck_scl": "STUCK",
    "power_disconnected": "POWER",
    "power_undervoltage": "POWER",
    "power_overvoltage": "POWER",
}


def terminal(cid, name, domain, direction, gpio=None, role=None):
    value = dict(id=f"{cid}.{name}", name=name, role=role or name,
                 domain=domain, direction=direction)
    if gpio is not None:
        value["gpio"] = gpio
    return value


def component(cid, kind, terminals, parameters=None, model=None):
    return dict(id=cid, name=cid, kind=kind, type=model or kind,
                terminals=terminals, parameters=parameters or {})


def quantity(value, unit):
    return dict(value=value, unit=unit)


def project(mode, pads=(8, 9, 10, 11)):
    """Two independent buses, each with its own same-address native devices."""
    if mode not in PROFILES:
        raise ValueError(f"Unknown graph mode: {mode}")
    wrong_pads = (12, 13, 14, 15)
    if mode == "wrong" and set(pads) & set(wrong_pads):
        raise ValueError("Wrong-wiring fixture pads must not overlap GPIO12..15")
    terms = [terminal("U1", "vdd", "power", "input"),
             terminal("U1", "gnd", "ground", "input")]
    for pad in sorted(set(pads) | (set(wrong_pads) if mode == "wrong" else set())):
        terms.append(terminal("U1", f"io{pad}", "digital", "inout", pad, "gpio"))
    components = [
        component("U1", "mcu", terms),
        component("G", "ground", [terminal("G", "ref", "ground", "passive")]),
        component("V", "voltage-source", [terminal("V", "p", "power", "output"),
                  terminal("V", "n", "ground", "input")], {"voltage": quantity(3.3, "V")}),
    ]
    endpoints = {"gnd": ["G.ref", "V.n", "U1.gnd"], "vdd": ["V.p", "U1.vdd"]}
    device_supply = "vdd"
    if mode.startswith("power_"):
        device_supply = "device_vdd"
        endpoints[device_supply] = []
        if mode != "power_disconnected":
            voltage = 1.8 if mode == "power_undervoltage" else 6.0
            components.append(component("VD", "voltage-source", [
                terminal("VD", "p", "power", "output"), terminal("VD", "n", "ground", "input")],
                {"voltage": quantity(voltage, "V")}))
            endpoints[device_supply].append("VD.p")
            endpoints["gnd"].append("VD.n")
    for port in range(2):
        sensor, eeprom = f"SHT{port}", f"EE{port}"
        for cid, model, address in ((sensor, "sht21", 0x40), (eeprom, "24c02", 0x50)):
            components.append(component(cid, "device", [
                terminal(cid, "sda", "digital", "inout"),
                terminal(cid, "scl", "digital", "inout"),
                terminal(cid, "vdd", "power", "input"),
                terminal(cid, "gnd", "ground", "input"),
            ], {"address": quantity(address, "count")}, model))
            endpoints[device_supply].append(f"{cid}.vdd")
            endpoints["gnd"].append(f"{cid}.gnd")
        for line, pad_index in (("sda", 0), ("scl", 1)):
            master_net = f"bus{port}_{line}"
            endpoints[master_net] = [f"U1.io{pads[port * 2 + pad_index]}"]
            device_net = master_net
            if mode in ("wrong", "disconnected"):
                device_net = f"device{port}_{line}"
                endpoints[device_net] = []
                if mode == "wrong":
                    endpoints[device_net].append(f"U1.io{wrong_pads[port * 2 + pad_index]}")
            endpoints[device_net].extend((f"{sensor}.{line}", f"{eeprom}.{line}"))
            if mode != "no_pull":
                # Disconnected/wrong master nets still idle high: address NACK,
                # not a favorable hidden pull or a floating-line timeout.
                for index, net in enumerate(dict.fromkeys((master_net, device_net))):
                    cid = f"R{port}_{line}_{index}"
                    components.append(component(cid, "resistor", [
                        terminal(cid, "a", "passive", "passive"),
                        terminal(cid, "b", "passive", "passive")],
                        {"resistance": quantity(4700, "ohm")}))
                    endpoints[net].append(f"{cid}.a")
                    endpoints["vdd"].append(f"{cid}.b")
            if mode == f"stuck_{line}":
                cid = f"SHORT{port}_{line}"
                components.append(component(cid, "resistor", [
                    terminal(cid, "a", "passive", "passive"),
                    terminal(cid, "b", "passive", "passive")],
                    {"resistance": quantity(10, "ohm")}))
                endpoints[master_net].append(f"{cid}.a")
                endpoints["gnd"].append(f"{cid}.b")
    nets = [dict(id=name, name=name, endpoints=ends) for name, ends in endpoints.items()]
    return dict(version=3, id=f"i2c-native-{mode}", name=f"Ordinary native I2C {mode}",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
                firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
                components=components, nets=nets, geometry=dict(components={}, nets={}))


def configuration(path, mode):
    values = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", path.read_text(), re.MULTILINE))
    selected = [key.removeprefix("CONFIG_I2C_NATIVE_PROFILE_") for key, value in values.items()
                if key.startswith("CONFIG_I2C_NATIVE_PROFILE_") and value == "y"]
    if selected != [PROFILES[mode]]:
        raise ValueError(f"Graph {mode} needs firmware profile {PROFILES[mode]}, sdkconfig selects {selected}")
    speed = int(values.get("CONFIG_I2C_NATIVE_SPEED_HZ", "0"))
    if speed not in (100000, 400000):
        raise ValueError("Fixture must configure CONFIG_I2C_NATIVE_SPEED_HZ=100000 or 400000")
    pads = tuple(int(values[f"CONFIG_I2C_NATIVE_{line}{port}"])
                 for port in range(2) for line in ("SDA", "SCL"))
    if len(set(pads)) != 4 or any(pad < 0 or pad > 48 or 22 <= pad <= 25 for pad in pads):
        raise ValueError(f"Both masters require four distinct valid ESP32S3 pads: {pads}")
    return dict(profile=PROFILES[mode].lower(), speed_hz=speed, pads=pads)


class Qmp:
    def __init__(self, path, transcript, deadline):
        self.deadline = deadline
        self.transcript = transcript
        self.sock = socket.socket(socket.AF_UNIX)
        self.stream = None
        try:
            self.sock.settimeout(max(.001, min(10, deadline - time.monotonic())))
            self.sock.connect(str(path))
            self.stream = self.sock.makefile("rwb", buffering=0)
            greeting = self.receive()
            if "QMP" not in greeting:
                raise RuntimeError(f"Invalid QMP greeting: {greeting}")
            self.sequence = 0
            self.call("qmp_capabilities")
        except BaseException:
            try:
                self.close()
            finally:
                raise

    def receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Host diagnostic watchdog expired during QMP")
        self.sock.settimeout(min(10, remaining))
        line = self.stream.readline()
        if not line:
            raise RuntimeError("QMP closed before response")
        result = json.loads(line)
        self.transcript.append(dict(direction="receive", message=result))
        return result

    def call(self, command, arguments=None):
        self.sequence += 1
        request = dict(execute=command, arguments=arguments or {}, id=self.sequence)
        self.transcript.append(dict(direction="send", message=request))
        self.stream.write((json.dumps(request) + "\n").encode())
        while True:
            response = self.receive()
            if "event" in response:
                continue
            if response.get("id") != request["id"]:
                raise RuntimeError(f"Unmatched QMP response: {response}")
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]

    def apply(self, document):
        self.call("stop")
        self.call("qom-set", dict(path="/machine/soc/electrical", property="project-json",
                                  value=json.dumps(document)))

    def snapshot(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))

    def close(self):
        try:
            if self.stream is not None:
                self.stream.close()
        finally:
            self.sock.close()


def uart_observations(text):
    return dict(boot=re.findall(r"^I2C_NATIVE_BOOT .*$", text, re.MULTILINE),
                phases=re.findall(r"^I2C_NATIVE_PHASE .*$", text, re.MULTILINE),
                checks=re.findall(r"^I2C_NATIVE_CHECK .*$", text, re.MULTILINE),
                statuses=re.findall(r"^I2C_NATIVE_STATUS .*$", text, re.MULTILINE),
                bytes=re.findall(r"^I2C_NATIVE_BYTES .*$", text, re.MULTILINE),
                times=re.findall(r"^I2C_NATIVE_(?:CONVERSION|TIME) .*$", text, re.MULTILINE),
                completion=re.findall(r"^I2C_NATIVE_DONE .*$", text, re.MULTILINE))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_uart(text, config):
    profile, speed, pads = config["profile"], config["speed_hz"], config["pads"]
    require(re.search(rf"^I2C_NATIVE_BOOT profile={profile}$", text, re.MULTILINE),
            "UART boot profile does not match graph and sdkconfig")
    require(re.search(rf"^I2C_NATIVE_DONE profile={profile} failures=0 result=PASS$", text, re.MULTILINE),
            "Ordinary firmware did not complete with failures=0 result=PASS")
    require(not re.search(r"^I2C_NATIVE_CHECK .*result=FAIL$", text, re.MULTILINE), "Firmware reported failed check")
    pullup = "none" if profile == "no_pull" else "external"
    for port in range(2):
        require(re.search(rf"^I2C_NATIVE_PHASE port={port} profile={profile} sda={pads[port*2]} scl={pads[port*2+1]} speed_hz={speed} pullup={pullup}$",
                          text, re.MULTILINE), f"Controller {port} firmware pad/profile/speed mismatch")
        checks = dict(re.findall(rf"^I2C_NATIVE_CHECK port={port} name=(\w+) result=(\w+)$", text, re.MULTILINE))
        expected = {"new_bus", "delete_bus"}
        if profile == "connected":
            expected |= {"probe_sht21", "probe_eeprom", "absent_address_nack", "unsupported_data_nack",
                         "conversion_busy_nack", "conversion_delay", "sample_crc8", "sample_status",
                         "consecutive_conversion_change", "initial_eeprom48_matches", "burst65_write",
                         "eeprom_page_wrap_and_change", "restart_vs_stop", "restart_does_not_commit",
                         "stop_commits_staged_page", "remove_sensor", "remove_eeprom"}
            rows = dict(re.findall(rf"^I2C_NATIVE_BYTES port={port} name=(\w+) data=([0-9a-f]+)$", text, re.MULTILINE))
            initial = bytes((i * 7 + port * 19 + 3) & 255 for i in range(48))
            changed = bytes((0xd0 + i + port) & 255 for i in range(16)) + initial[16:]
            require(rows.get("initial_eeprom48") == initial.hex(), f"Controller {port} initial EEPROM bytes differ")
            require(rows.get("changed_eeprom48") == changed.hex(), f"Controller {port} page-wrapped EEPROM bytes differ")
            require(rows.get("stop_eeprom48") == changed.hex(), f"Controller {port} stop-separated bytes differ")
            committed = bytes((0x50 + i + port) & 255 for i in range(16)) + initial[16:]
            require(rows.get("before_stop_page16") == changed[:16].hex(), f"Controller {port} RESTART committed early")
            require(rows.get("after_stop_eeprom48") == committed.hex(), f"Controller {port} STOP did not commit")
            times = re.findall(rf"^I2C_NATIVE_CONVERSION port={port} command=(f3|f5) elapsed_us=(\d+)$", text, re.MULTILINE)
            require(len(times) == 3 and sum(command == "f3" for command, _ in times) == 2,
                    f"Controller {port} missing conversion time evidence")
            require(all(int(elapsed) >= (100000 if command == "f3" else 40000) for command, elapsed in times),
                    f"Controller {port} conversion time too short")
        else:
            expected |= {"graph_sensor_unreachable", "graph_eeprom_unreachable"}
        require(all(checks.get(name) == "PASS" for name in expected),
                f"Controller {port} missing passing acceptance checks: {sorted(name for name in expected if checks.get(name) != 'PASS')}")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--flash", required=True, type=pathlib.Path)
    parser.add_argument("--sdkconfig", required=True, type=pathlib.Path)
    parser.add_argument("--mode", required=True, choices=PROFILES)
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    parser.add_argument("--watchdog-seconds", type=float, default=600,
                        help="Bounded host diagnostic only; native electrical simulation is slower than guest time")
    args = parser.parse_args()
    if not 1 <= args.watchdog_seconds <= 600:
        parser.error("--watchdog-seconds must be between 1 and 600")
    for field in ("qemu", "flash", "sdkconfig"):
        path = getattr(args, field).resolve(strict=True)
        if not path.is_file():
            parser.error(f"--{field} must name a file")
        setattr(args, field, path)
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f"{args.mode}-{time.strftime('%Y%m%dT%H%M%S')}-",
                                           dir=args.evidence.resolve()))
    uart, stderr, stdout = (evidence / name for name in ("uart.log", "stderr.log", "stdout.log"))
    result = dict(status="FAIL", mode=args.mode, evidence=str(evidence),
                  host_watchdog_seconds=args.watchdog_seconds,
                  qualification="Timed transaction fast path; no edge/glitch/slave/10-bit/multimaster qualification",
                  command=[], snapshots=[], qmp_transcript=[])
    proc = qmp = None
    started = time.monotonic()
    try:
        config = configuration(args.sdkconfig, args.mode)
        result["firmware_configuration"] = config
        graph = project(args.mode, config["pads"])
        (evidence / "project.json").write_text(json.dumps(graph, indent=2) + "\n")
        # UNIX sockets have a short path limit; keep this ephemeral path outside
        # potentially long evidence roots. Only transport is ephemeral.
        with tempfile.TemporaryDirectory(prefix="i2c-qmp-") as transport:
            qmp_path = pathlib.Path(transport) / "qmp.sock"
            command = [str(args.qemu), "-machine", "esp32s3", "-nographic", "-S",
                       "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
                       "-monitor", "none", "-serial", f"file:{uart}",
                       "-drive", f"file={args.flash},if=mtd,format=raw,snapshot=on",
                       "-qmp", f"unix:{qmp_path},server=on,wait=off"]
            result["command"] = command
            (evidence / "command.json").write_text(json.dumps(command, indent=2) + "\n")
            deadline = started + args.watchdog_seconds
            with stdout.open("wb") as output, stderr.open("wb") as errors:
                proc = subprocess.Popen(command, stdout=output, stderr=errors)
                try:
                    while not qmp_path.exists():
                        if proc.poll() is not None:
                            raise RuntimeError(f"QEMU exited {proc.returncode} before QMP; see stderr.log")
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Host diagnostic watchdog expired waiting for QMP")
                        time.sleep(.05)
                    qmp = Qmp(qmp_path, result["qmp_transcript"], deadline)
                    qmp.apply(graph)
                    result["snapshots"].append(dict(phase="applied-before-boot", graph=qmp.snapshot()))
                    qmp.call("cont")
                    while True:
                        text = uart.read_text(errors="replace") if uart.exists() else ""
                        if re.search(r"^I2C_NATIVE_DONE profile=\w+ failures=\d+ result=(?:PASS|FAIL)$",
                                     text, re.MULTILINE):
                            break
                        if proc.poll() is not None:
                            raise RuntimeError(f"QEMU exited {proc.returncode} before UART completion")
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Host diagnostic watchdog expired; firmware outcome unqualified (see UART/stderr)")
                        time.sleep(.05)
                    qmp.call("stop")
                    result["snapshots"].append(dict(phase="firmware-completed", graph=qmp.snapshot()))
                    validate_uart(text, config)
                    result["status"] = "PASS"
                finally:
                    try:
                        if qmp is not None:
                            qmp.close()
                    finally:
                        if proc.poll() is None:
                            proc.terminate()
                            try:
                                proc.wait(5)
                            except subprocess.TimeoutExpired:
                                proc.kill()
                                proc.wait()
                        result["qemu_returncode_after_cleanup"] = proc.returncode
    except Exception as exc:
        result["status"] = "FAIL"
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        result["host_elapsed_seconds"] = time.monotonic() - started
        result["uart"] = uart_observations(uart.read_text(errors="replace") if uart.exists() else "")
        sources = [args.qemu, args.flash, args.sdkconfig, pathlib.Path(__file__).resolve()]
        sources.extend(path for path in (evidence / "project.json", evidence / "command.json", uart, stderr, stdout) if path.exists())
        result["hashes"] = {str(path): sha256(path) for path in sources}
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), error=result.get("error"))))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
