#!/usr/bin/env python3
"""Ordinary IDF firmware or actual native MMIO/qtest pulse qualification.

See fixtures.json instructions. Requires explicit --qemu, --evidence and either
--flash (--sdkconfig for firmware) or --build. QEMU opens a writable evidence
copy, never the user's flash disk. The firmware path never writes MMIO or
injects counts/inputs. qtest uses the qtest accelerator (no guest CPU), real
registers and public v3 nets, observing each next timer deadline. ISR guest-us
and electrical virtual-ns records remain separate. No averaged GPIO source,
surrogate peripheral, exact SDM silicon-sequence oracle or mock fallback.
"""
import argparse
import copy
import hashlib
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time
import traceback

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
GPIO, IOMUX, LEDC, PCNT, SDM, SYSTEM = 0x60004000, 0x60009000, 0x60019000, 0x60017000, 0x60004f00, 0x600c0000


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def store(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def graph(mode):
    fixtures = json.loads((HERE / "fixtures.json").read_text())
    p = copy.deepcopy(fixtures["project"])
    p["id"] += "-" + mode
    for name, endpoints in fixtures["routes"][mode].items():
        p["nets"].append(dict(id=name, name=name, endpoints=endpoints))
    used = {e for net in p["nets"] for e in net["endpoints"]}
    for terminal in p["components"][0]["terminals"]:
        if terminal["id"] not in used:
            p["nets"].append(dict(id=terminal["name"], name=terminal["name"], endpoints=[terminal["id"]]))
    return p


class Transport:
    def __init__(self, path, deadline, log):
        self.deadline, self.log = deadline, log
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(max(.001, min(10, deadline - time.monotonic())))
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)

    def receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Host transport watchdog expired")
        self.sock.settimeout(min(10, remaining))
        line = self.stream.readline().decode()
        if not line:
            raise RuntimeError("Native transport closed")
        self.log.write("< " + line)
        self.log.flush()
        return line.strip()

    def send(self, line):
        self.log.write("> " + line + "\n")
        self.log.flush()
        self.stream.write((line + "\n").encode())

    def close(self):
        self.stream.close()
        self.sock.close()


class Qmp(Transport):
    def __init__(self, *args):
        super().__init__(*args)
        self.seq = 0
        require("QMP" in json.loads(self.receive()), "Invalid QMP greeting")
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        self.seq += 1
        self.send(json.dumps(dict(execute=command, arguments=arguments or {}, id=self.seq)))
        while True:
            response = json.loads(self.receive())
            if "event" in response:
                continue
            require(response.get("id") == self.seq, "Mismatched QMP response")
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]

    def snapshot(self):
        value = json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))
        require(value.get("status") in ("settled", "floating"), f"Electrical graph unavailable: {value.get('diagnostic')}")
        return value


class Qtest(Transport):
    def call(self, line):
        self.send(line)
        while True:
            response = self.receive()
            if response.startswith("IRQ "):
                continue
            require(response == "OK" or response.startswith("OK "), f"qtest {line}: {response}")
            return int(response.split()[1], 0) if " " in response else None

    def read(self, address):
        return self.call(f"readl {address:#x}")

    def write(self, address, value):
        self.call(f"writel {address:#x} {value & 0xffffffff:#x}")

    def bits(self, address, mask, enabled):
        value = self.read(address)
        self.write(address, value | mask if enabled else value & ~mask)

    def step(self, ns=None):
        return self.call("clock_step" + (f" {ns}" if ns is not None else ""))


def build(args, evidence):
    idf = args.idf.resolve(strict=True)
    commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    require(commit == IDF_COMMIT, f"IDF checkout must be pinned {IDF_COMMIT}, found {commit}")
    fixture = ROOT / "tests/firmware/pulse_native"
    build_dir = evidence / "idf-build"
    defaults = evidence / "sdkconfig.defaults"
    text = (fixture / "sdkconfig.defaults").read_text().replace("CONFIG_PULSE_NATIVE_CONNECTED=y", "CONFIG_PULSE_NATIVE_" + args.mode.upper().replace("-", "_") + "=y")
    text = text.replace("CONFIG_PULSE_NATIVE_ALL=y", "CONFIG_PULSE_NATIVE_" + args.feature.upper().replace("-", "_") + "=y")
    if args.adc_provider_ready:
        text += "CONFIG_PULSE_NATIVE_ADC_PROVIDER_READY=y\n"
    defaults.write_text(text)
    config = evidence / "sdkconfig"
    python = args.idf_python.expanduser().absolute()
    require(python.is_file(), "--idf-python must name activated pinned IDF interpreter")
    env = dict(os.environ, IDF_PATH=str(idf), IDF_PYTHON_ENV_PATH=str(python.parent.parent))
    env["PATH"] = str(python.parent) + os.pathsep + env.get("PATH", "")
    command = [str(python), str(idf / "tools/idf.py"), "-B", str(build_dir), "-DIDF_TARGET=esp32s3",
               f"-DSDKCONFIG={config}", f"-DSDKCONFIG_DEFAULTS={defaults}", "build"]
    with (evidence / "build.log").open("wb") as f:
        subprocess.run(command, cwd=fixture, env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
    flash = evidence / "pulse-native.merged.bin"
    merge = [str(python), "-m", "esptool", "--chip", "esp32s3", "merge-bin", "--fill-flash-size", "4MB", "-o", str(flash), "@flash_args"]
    with (evidence / "merge.log").open("wb") as f:
        subprocess.run(merge, cwd=build_dir, env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
    store(evidence / "build-pin.json", dict(idf_commit=commit, command=command, merge=merge,
          flash_sha256=digest(flash), sdkconfig_sha256=digest(config)))
    return flash, config


def configuration(args):
    text = args.sdkconfig.read_text()
    for symbol in ("IDF_TARGET_ESP32S3", "PULSE_NATIVE_" + args.mode.upper().replace("-", "_"),
                   "PULSE_NATIVE_" + args.feature.upper().replace("-", "_")):
        require(re.search(rf"^CONFIG_{symbol}=y$", text, re.M), f"Firmware sdkconfig does not select {symbol}")
    adc = bool(re.search(r"^CONFIG_PULSE_NATIVE_ADC_PROVIDER_READY=y$", text, re.M))
    require(adc == bool(args.adc_provider_ready), "ADC sdkconfig/provenance mismatch; do not guess readiness")
    for symbol, value in (("ESP_CONSOLE_UART_CUSTOM", "y"),
                          ("ESP_CONSOLE_UART_NUM", "0"),
                          ("ESP_CONSOLE_UART_TX_GPIO", "43"),
                          ("ESP_CONSOLE_UART_RX_GPIO", "44"),
                          ("ESP_CONSOLE_UART_BAUDRATE", "2000000")):
        require(re.search(rf"^CONFIG_{symbol}={value}$", text, re.M),
                f"Actual compiled console does not select {symbol}={value}")


def validate_uart(text, args):
    require(re.search(rf"^PULSE_BOOT wiring={args.mode} idf=6.1 timestamp_unit=guest_us$", text, re.M), "Missing native firmware boot declaration")
    require(re.findall(r"^PULSE_DONE wiring=(\S+) failures=(\d+) time_us=\d+$", text, re.M) == [(args.mode, "0")], "Firmware incomplete or assertions failed")
    checks = re.findall(r"^PULSE_CHECK phase=(\S+) name=(\S+) result=(PASS|FAIL) time_us=(\d+)$", text, re.M)
    require(checks and all(c[2] == "PASS" for c in checks), "Native firmware reported failed acceptance")
    phases = set(re.findall(r"^PULSE_PHASE name=(\S+) time_us=\d+$", text, re.M))
    expected = set()
    if args.feature in ("all", "ledc-pcnt"):
        expected |= {"ledc_apb_count", "pcnt_negative_direction", "pcnt_gate", "pcnt_stop", "ledc_fade", "ledc_pause", "ledc_gate", "ledc_xtal_count", "ledc_reset", "ledc_reset_reconfigured"}
        expected.add("ledc_timer_rebind")
    if args.feature in ("all", "mcpwm"):
        expected |= {"mcpwm_up", "mcpwm_down", "mcpwm_up_down", "mcpwm_gpio_sync", "mcpwm_fault_ost", "mcpwm_fault_cbc", "mcpwm_gate", "mcpwm_stop", "mcpwm_reset"}
        expected |= {"mcpwm1_up", "mcpwm1_down", "mcpwm1_up_down"}
    if args.feature in ("all", "sdm"):
        expected |= {"sdm_allocation", "sdm_density", "sdm_api_disable"}
        require(re.findall(r"^PULSE_SDM density=(-?\d+) sample_rate_hz=312500 submit_us=\d+ end_us=\d+$", text, re.M) == ["-128", "-90", "0", "90", "127"], "Missing actual SDM density stages")
        require(len(re.findall(r"^PULSE_SDM_CHANNEL ", text, re.M)) == 8, "SDM did not allocate all eight channels")
        if args.adc_provider_ready:
            require(len(re.findall(r"^PULSE_ADC ", text, re.M)) == 80, "Missing actual ADC conversions")
        else:
            require("PULSE_UNAVAILABLE stage=sdm_adc" in text and "PULSE_ADC " not in text, "ADC unavailability was not honest")
    require(expected <= phases, f"Missing feature stages: {sorted(expected - phases)}")
    callbacks = [dict(phase=p, index=int(i), kind=int(k), value=int(v), callback_us=int(t)) for p, i, k, v, t in re.findall(r"^PULSE_CALLBACK phase=(\S+) index=(\d+) kind=(\d+) value=(-?\d+) callback_us=(\d+)$", text, re.M)]
    captures = [e for e in callbacks if e["kind"] in (3, 4)]
    if args.feature in ("all", "mcpwm"):
        require(bool(captures) == (args.mode != "disconnected"), "Capture payload disagrees with actual physical wiring")
        for name in ("mcpwm_up", "mcpwm_down", "mcpwm_up_down", "mcpwm1_up", "mcpwm1_down", "mcpwm1_up_down"):
            measured = [e for e in captures if e["phase"] == name]
            for a, b in zip(measured, measured[1:]):
                require(b["callback_us"] >= a["callback_us"], f"Nonmonotonic actual ISR time {name}")
            if args.mode != "disconnected":
                require(len(measured) > 12, f"Insufficient actual capture pulses for {name}")
                hz_match = re.search(rf"^PULSE_CAPTURE_CONFIG phase={name} resolution_hz=(\d+)$", text, re.M)
                require(hz_match is not None, f"Missing actual capture resolution {name}")
                hz = int(hz_match.group(1))
                initial = measured[2:12]
                rising = [e["value"] & 0xffffffff for e in initial if e["kind"] == 3]
                periods = [(b - a) & 0xffffffff for a, b in zip(rising, rising[1:])]
                require(periods and all(abs(ticks - hz // 1000) <= 1 for ticks in periods),
                        f"Actual capture cadence does not match 1ms timer {name}: {periods}")
                for a, b in zip(initial, initial[1:]):
                    require(a["kind"] != b["kind"], f"Capture missed physical edge polarity {name}")
                high_ticks = [((b["value"] - a["value"]) & 0xffffffff) for a, b in zip(initial, initial[1:])
                              if a["kind"] == 3 and b["kind"] == 4]
                # Asymmetric LL peak is period_ticks-1: down 999->250 costs 749us.
                symmetric = name.endswith("_up_down")
                width_us = 490 if symmetric else (240 if name.endswith("_up") else 739)
                if args.mode == "wrong-wire" and not symmetric:
                    width_us = 980 - width_us
                require(high_ticks and all(abs(ticks - hz * width_us // 1000000) <= 1 for ticks in high_ticks),
                        f"Actual deadtime/counter-mode width mismatch {name}: {high_ticks}")
    return dict(checks=checks, callbacks=callbacks, capture_events=captures,
                unavailable=re.findall(r"^PULSE_UNAVAILABLE .*$", text, re.M),
                records=re.findall(r"^PULSE_(?:COUNT|WINDOW|FADE|SYNC|SDM|ADC|CAPTURE_CONFIG).*$", text, re.M))


def pad(snapshot, number):
    matches = [p for p in snapshot["pads"] if p["gpio"] == number]
    require(len(matches) == 1, f"Physical GPIO{number} is not in actual graph snapshot")
    return matches[0]


def level(snapshot, number):
    p = pad(snapshot, number)
    require(p["digital_valid"] and p["diagnostic"] in ("low", "high"), f"Unknown real GPIO{number}: {p}")
    return int(p["diagnostic"] == "high")


def observe(qt, qmp, ns, pads, name, evidence, max_events=30000, registers=None):
    start = qt.step(0)
    snap = qmp.snapshot()
    previous = {p: level(snap, p) for p in pads}
    observations = [dict(ns=start, levels=previous.copy(), rc_v=pad(snap, 1)["voltage_v"], solver_ns=int(snap["timestamp_ns"]))]
    changes = []
    current = start
    for _ in range(max_events):
        current = qt.step()
        require(current > observations[-1]["ns"], "Native next-deadline clock failed to progress")
        snap = qmp.snapshot()
        values = {p: level(snap, p) for p in pads}
        for p in pads:
            if values[p] != previous[p]:
                # One settled value per timer deadline. No host timestamp is used.
                changes.append(dict(ns=current, gpio=p, level=values[p], solver_ns=int(snap["timestamp_ns"])))
        previous = values
        sampled_registers = {key: qt.read(address) for key, address in (registers or {}).items()}
        observations.append(dict(ns=current, levels=values, rc_v=pad(snap, 1)["voltage_v"],
                                 solver_ns=int(snap["timestamp_ns"]), registers=sampled_registers))
        if current - start >= ns:
            break
    else:
        raise AssertionError(f"Bounded observer exhausted {max_events} deadlines in {name}")
    store(evidence / f"{name}-events.json", dict(start_ns=start, end_ns=current, events=changes, observations=observations))
    statistics = {}
    for p in pads:
        high = sum((b["ns"] - a["ns"]) * a["levels"][p] for a, b in zip(observations, observations[1:]))
        rising = [e["ns"] for e in changes if e["gpio"] == p and e["level"]]
        periods = [b - a for a, b in zip(rising, rising[1:])]
        statistics[str(p)] = dict(high_fraction=high / (current - start), rising_edges=len(rising),
                                  rising_periods_ns=periods, transitions=sum(e["gpio"] == p for e in changes))
    tail = [o for o in observations if o["ns"] - start >= (current - start) // 2]
    rc_valid = len(tail) >= 2 and all(o["rc_v"] is not None for o in tail)
    rc_mean = rc_ripple = None
    if rc_valid:
        durations = [b["ns"] - a["ns"] for a, b in zip(tail, tail[1:])]
        rc_mean = sum(a["rc_v"] * d for a, d in zip(tail, durations)) / sum(durations)
        rc_ripple = max(o["rc_v"] for o in tail) - min(o["rc_v"] for o in tail)
    overlap = sum(b["ns"] - a["ns"] for a, b in zip(observations, observations[1:])
                  if 6 in a["levels"] and 7 in a["levels"] and a["levels"][6] and a["levels"][7])
    return dict(phase=name, start_ns=start, end_ns=current, statistics=statistics, simultaneous_high_ns=overlap,
                rc_mean_v=rc_mean, rc_ripple_v=rc_ripple,
                observer="resolved physical pad per next timer deadline, exact observation ns; not silicon metrology")


def route(qt, output, signal):
    qt.write(IOMUX + 4 * (output + 1), (1 << 12) | (1 << 9) | (2 << 10))
    qt.write(GPIO + 0x554 + 4 * output, signal | (1 << 10))
    qt.write(GPIO + 0x24, 1 << output)


def run_qtest(qt, qmp, args, evidence):
    results = []
    qt.write(SYSTEM + 0x60, 0x00028401)  # pinned central-clock owner: PLL CPU160MHz, APB80MHz
    # Enable actual input buffers, not an input-line injection or cached bit.
    for p in (1, 3, 4, 5, 11, 14, 15):
        qt.write(IOMUX + 4 * (p + 1), (1 << 12) | (1 << 9) | (2 << 10))
    if args.feature in ("all", "ledc-pcnt"):
        qt.bits(SYSTEM + 0x18, (1 << 10) | (1 << 11), True)
        qt.bits(SYSTEM + 0x20, (1 << 10) | (1 << 11), True)
        qt.bits(SYSTEM + 0x20, (1 << 10) | (1 << 11), False)
        route(qt, 4, 73)  # LEDC_LS_SIG_OUT0_IDX
        route(qt, 14, 256)  # GPIO data register output
        qt.write(GPIO + 0x8, 1 << 14)
        qt.write(GPIO + 0x154 + 4 * 33, 5 | (1 << 7))
        qt.write(GPIO + 0x154 + 4 * 35, 15 | (1 << 7))
        qt.write(PCNT + 8, (0x8000 << 16) | 0x7fff)
        qt.write(PCNT, (1 << 18) | (1 << 22))  # rising +1, low level invert, no filter
        qt.write(PCNT + 0x60, (1 << 16) | 1)
        qt.write(PCNT + 0x60, 1 << 16)
        qt.write(LEDC + 0xd0, (1 << 31) | 1)  # APB 80MHz
        # 10-bit resolution, divider 78.125 (fixed8 20000), period 1ms.
        timer = 10 | (20000 << 4)
        qt.write(LEDC + 0xa0, timer | (1 << 23) | (1 << 25))
        qt.write(LEDC + 4, 0)
        qt.write(LEDC + 8, 256 << 4)
        qt.write(LEDC + 0xc, 1 << 31)
        qt.write(LEDC, (1 << 2) | (1 << 4))
        qt.write(LEDC + 0xa0, timer | (1 << 25))
        for name, negative in (("ledc_count", False), ("pcnt_negative", True)):
            if negative:
                qt.write(GPIO + 0xc, 1 << 14)
            before = qt.read(PCNT + 0x30) & 0xffff
            report = observe(qt, qmp, 12000000, (4, 5), name, evidence)
            after = qt.read(PCNT + 0x30) & 0xffff
            delta = ((after - before + 32768) & 0xffff) - 32768
            rises = report["statistics"]["5"]["rising_edges"]
            require(delta == (-rises if negative else rises), f"Actual PCNT count mismatch {name}: {delta}/{rises}")
            require(rises > 5 if args.mode == "connected" else rises == 0, "Actual negative-wire PCNT condition mismatch")
            periods = report["statistics"]["4"]["rising_periods_ns"]
            require(periods and set(periods) == {1000000}, f"Actual LEDC cadence mismatch: {periods}")
            require(abs(report["statistics"]["4"]["high_fraction"] - .25) < .03, "Actual LEDC duty mismatch")
            report["actual_counter_delta"] = delta
            results.append(report)
        # Paused counter still sees actual source pulses but must hold count.
        qt.bits(PCNT + 0x60, 2, True)
        before = qt.read(PCNT + 0x30)
        results.append(observe(qt, qmp, 4000000, (4, 5), "pcnt_pause", evidence))
        require(qt.read(PCNT + 0x30) == before, "PCNT changed while paused")
        qt.bits(PCNT + 0x60, 1, True)
        require(qt.read(PCNT + 0x30) & 0xffff == 0, "PCNT reset did not clear actual counter")
        qt.write(PCNT + 0x60, 1 << 16)
        # Gate with a fixed duration because disabled source has no next edge.
        qt.bits(SYSTEM + 0x18, 1 << 11, False)
        before = qt.read(PCNT + 0x30)
        gate_start = qt.step(0)
        qt.step(3000000)
        require(qt.read(PCNT + 0x30) == before, "LEDC gate generated actual counted edges")
        results.append(dict(phase="ledc_gate", start_ns=gate_start, end_ns=qt.step(0), actual_counter=before, snapshot=qmp.snapshot()))
        qt.bits(SYSTEM + 0x18, 1 << 11, True)
        results.append(observe(qt, qmp, 4000000, (4, 5), "ledc_gate_resume", evidence))
        qt.write(LEDC, 1 << 4)  # signal disabled, idle low
        qt.bits(SYSTEM + 0x20, 1 << 11, True)
        qt.bits(SYSTEM + 0x20, 1 << 11, False)
        require(qt.read(LEDC + 8) == 0, "LEDC reset retained programmed duty")
    if args.feature in ("all", "mcpwm"):
        for group in range(2):
            base = 0x6002c000 if group else 0x6001e000
            gate = 1 << (20 if group else 17)
            signals = 169 if group else 160
            prefix = f"mcpwm{group}"
            qt.bits(SYSTEM + 0x18, gate, True)
            qt.bits(SYSTEM + 0x20, gate, True)
            qt.bits(SYSTEM + 0x20, gate, False)
            for p in (6, 7, 8, 9, 10, 12, 13):
                qt.write(IOMUX + 4 * (p + 1), (1 << 12) | (1 << 9) | (2 << 10))
            route(qt, 6, 160 + 6 * group)
            route(qt, 7, 161 + 6 * group)
            route(qt, 12, 256)
            route(qt, 13, 256)
            qt.write(GPIO + 0xc, (1 << 12) | (1 << 13))
            qt.write(GPIO + 0x154 + 4 * (signals + 6), 8 | (1 << 7))
            capture_epoch = qt.step(0)
            qt.write(base + 0xe8, 1)
            qt.write(base + 0xf0, 1 | (1 << 1) | (1 << 2))
            qt.write(base, 159)  # PLL160MHz / 160 = 1MHz group clock
            qt.write(base + 4, 999 << 8)
            qt.write(base + 0x38, 0)
            qt.write(base + 0x40, 250)
            qt.write(base + 0x50, 0x12)  # TEZ high / compareA low
            qt.write(base + 0x54, 0x21)
            qt.write(base + 0x5c, 9)
            qt.write(base + 0x60, 9)
            qt.write(base + 0x58, 1 << 14)  # active-high complementary with 10us deadtime
            qt.write(base + 8, 0x0a)
            capture_registers = {"capture0_ticks": base + 0xfc, "capture_edge": base + 0x108,
                                 "interrupt_raw": base + 0x114, "timer0_status": base + 0x10}
            report = observe(qt, qmp, 10000000, (6, 7, 8), f"{prefix}_deadtime_capture", evidence,
                             registers=capture_registers)
            require(report["simultaneous_high_ns"] == 0, "Physical MCPWM complementary outputs overlap")
            for p, expected in ((6, .24), (7, .74)):
                stats = report["statistics"][str(p)]
                require(stats["rising_periods_ns"] and set(stats["rising_periods_ns"]) == {1000000},
                        f"Physical MCPWM GPIO{p} has wrong cadence")
                require(abs(stats["high_fraction"] - expected) < .03, f"Physical MCPWM GPIO{p} deadtime width mismatch")
            capstats = report["statistics"]["8"]
            expected_pad = 6 if args.mode == "connected" else 7
            require(capstats == report["statistics"][str(expected_pad)] if args.mode != "disconnected"
                    else capstats["transitions"] == 0, "Actual capture physical net differs from selected wire")
            # Verify the actual controller capture payload, not merely the wire.
            trace = json.loads((evidence / f"{report['phase']}-events.json").read_text())
            transitions = [e for e in trace["events"] if e["gpio"] == 8]
            observations = {o["ns"]: o for o in trace["observations"]}
            for edge in transitions:
                regs = observations[edge["ns"]]["registers"]
                expected_ticks = ((edge["ns"] - capture_epoch) * 80000000 // 1000000000) & 0xffffffff
                error = ((regs["capture0_ticks"] - expected_ticks + 0x80000000) & 0xffffffff) - 0x80000000
                require(abs(error) <= 1, f"Actual {prefix} capture timestamp differs from resolved edge")
                require(bool(regs["capture_edge"] & 1) == (not edge["level"]), f"Actual {prefix} capture polarity mismatch")
                require(regs["interrupt_raw"] & (1 << 27), f"Actual {prefix} capture IRQ missing")
            require(bool(transitions) == (args.mode != "disconnected"), f"Actual {prefix} capture transition coverage mismatch")
            require(bool(qt.read(base + 0x114) & (1 << 27)) == (args.mode != "disconnected"),
                    f"Actual {prefix} capture IRQ disagrees with physical wiring")
            report["capture_payload_edges_checked"] = len(transitions)
            results.append(report)
            qt.write(base + 4, (999 << 8) | (1 << 24))  # period shadow on TEZ, no method-bit leakage
            period_report = observe(qt, qmp, 4000000, (6, 7, 8), f"{prefix}_period_shadow", evidence,
                                    registers=capture_registers)
            require(period_report["statistics"]["6"]["rising_periods_ns"] and
                    set(period_report["statistics"]["6"]["rising_periods_ns"]) == {1000000},
                    f"Actual {prefix} period-shadow method leaked into counter period")
            results.append(period_report)
            qt.write(base + 0x3c, 1)  # compare shadow on TEZ
            qt.write(base + 0x40, 400)
            results.append(observe(qt, qmp, 4000000, (6, 7, 8), f"{prefix}_compare_shadow", evidence,
                                   registers=capture_registers))
            qt.write(GPIO + 0x154 + 4 * signals, 9 | (1 << 7))
            qt.write(base + 0x34, 4)  # GPIO sync0 -> timer0
            qt.write(base + 0xc, 1 | (100 << 4))
            qt.write(GPIO + 8, 1 << 12)
            sync_ns = qt.step(1)
            sync_status = qt.read(base + 0x10) & 0xffff
            require(sync_status in (100, 101) if args.mode != "disconnected" else sync_status not in (100, 101),
                    "Actual GPIO sync route did not produce expected timer count")
            results.append(dict(phase=f"{prefix}_gpio_sync", ns=sync_ns, actual_timer_status=sync_status, snapshot=qmp.snapshot()))
            qt.write(GPIO + 0xc, 1 << 12)
            qt.write(GPIO + 0x154 + 4 * (signals + 3), 10 | (1 << 7))
            qt.write(base + 0xe4, 1 | (1 << 3))
            qt.write(base + 0x68, (1 << 7) | (1 << 14))  # F0 OST A low
            qt.write(GPIO + 8, 1 << 13)
            fault_ns = qt.step(1000)  # fault detection runs on the 1MHz group clock
            brake_status = qt.read(base + 0x70)
            require(bool(brake_status & 2) == (args.mode != "disconnected"), "Actual physical fault/brake route mismatch")
            results.append(dict(phase=f"{prefix}_fault_ost", ns=fault_ns, actual_brake_status=brake_status, snapshot=qmp.snapshot()))
            qt.write(GPIO + 0xc, 1 << 13)
            qt.step(1000)  # settle the physical exit on the same group clock
            qt.write(base + 0x6c, 1)  # OST recovery after physical fault disappears
            results.append(observe(qt, qmp, 4000000, (6, 7, 8), f"{prefix}_fault_recovery", evidence,
                                   registers=capture_registers))
            qt.bits(SYSTEM + 0x18, gate, False)
            status = qt.read(base + 0x10)
            qt.step(3000000)
            require(qt.read(base + 0x10) == status, "Actual MCPWM gated counter advanced")
            results.append(dict(phase=f"{prefix}_gate", ns=qt.step(0), actual_timer_status=status, snapshot=qmp.snapshot()))
            qt.bits(SYSTEM + 0x18, gate, True)
            results.append(observe(qt, qmp, 4000000, (6, 7, 8), f"{prefix}_gate_resume", evidence,
                                   registers=capture_registers))
            qt.write(base + 8, 8)  # stop at next empty
            qt.step(2000000)
            status = qt.read(base + 0x10)
            qt.step(2000000)
            require(qt.read(base + 0x10) == status, "Actual MCPWM stopped counter advanced")
            results.append(dict(phase=f"{prefix}_stop", ns=qt.step(0), actual_timer_status=status,
                                interrupt_raw=qt.read(base + 0x114), snapshot=qmp.snapshot()))
            qt.bits(SYSTEM + 0x20, gate, True)
            qt.bits(SYSTEM + 0x20, gate, False)
            require(qt.read(base + 0x50) == 0 and qt.read(base + 0x114) == 0, "MCPWM reset retained actions/interrupts")
    if args.feature in ("all", "sdm"):
        route(qt, 11, 93)  # GPIO_SD0_OUT_IDX
        qt.write(SDM + 0x20, 1 << 31)  # register clock override, not output enable
        qt.write(SDM + 0x24, 1 << 30)  # actual shared functional clock
        for density in (-128, -90, 0, 90, 127):
            qt.write(SDM, (255 << 8) | (density & 255))  # 80MHz/256=312500Hz
            report = observe(qt, qmp, 13107200, (11,), f"sdm_density_{density}", evidence)
            fraction = report["statistics"]["11"]["high_fraction"]
            expected = (density + 128) / 256
            require(abs(fraction - expected) < .025, f"Documented actual SDM density mismatch {density}: {fraction}")
            require(report["rc_mean_v"] is not None and report["rc_ripple_v"] is not None,
                    "SDM passive RC observation is unavailable; no analog value fabricated")
            require(all(period % 3200 == 0 for period in report["statistics"]["11"]["rising_periods_ns"]),
                    "Actual SDM changes violate APB/256 slot cadence")
            require(abs(report["rc_mean_v"] - 3.3 * expected) < .12,
                    f"Real passive RC mean disagrees with documented density {density}: {report['rc_mean_v']}")
            require(report["rc_ripple_v"] < .15, f"Real passive RC ripple too large: {report['rc_ripple_v']}")
            report["density"] = density
            report["expected_documented_mean_fraction"] = expected
            report["exact_silicon_sequence"] = "UNAVAILABLE: hidden second-order widths/startup source not qualified"
            results.append(report)
        qt.bits(SDM + 0x24, 1 << 30, False)
        before = level(qmp.snapshot(), 11)
        start = qt.step(0)
        qt.step(1000000)
        require(level(qmp.snapshot(), 11) == before, "SDM shared clock gate changed output")
        results.append(dict(phase="sdm_function_clock_gate", start_ns=start, end_ns=qt.step(0), snapshot=qmp.snapshot()))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    parser.add_argument("--execution", required=True, choices=("firmware", "qtest"))
    parser.add_argument("--mode", required=True, choices=("connected", "disconnected", "wrong-wire"))
    parser.add_argument("--feature", default="all", choices=("all", "ledc-pcnt", "mcpwm", "sdm"))
    parser.add_argument("--flash", type=pathlib.Path)
    parser.add_argument("--sdkconfig", type=pathlib.Path)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--idf", type=pathlib.Path,
                        default=pathlib.Path.home() / f".cache/esp32s3vm/idf/idf-6.1-{IDF_COMMIT}/source")
    parser.add_argument("--idf-python", type=pathlib.Path, default=pathlib.Path.home() / ".espressif/python_env/idf6.1_py3.12_env/bin/python")
    parser.add_argument("--adc-provider-ready", type=pathlib.Path, help="Externally produced ADC readiness evidence JSON with status READY")
    parser.add_argument("--watchdog-seconds", type=float, default=300)
    args = parser.parse_args()
    if not 1 <= args.watchdog_seconds <= 86400:
        parser.error("--watchdog-seconds must be 1..86400 (host guard, not peripheral acceptance timing)")
    if not args.build and (not args.flash or (args.execution == "firmware" and not args.sdkconfig)):
        parser.error("Use --build or --flash (and --sdkconfig for firmware)")
    args.evidence.mkdir(parents=True, exist_ok=True)
    durable = pathlib.Path(tempfile.mkdtemp(prefix=f"pulse-{args.execution}-{args.mode}-", dir=args.evidence.resolve()))
    # Use the accepted RMT transport convention for every active artifact,
    # including compiler objects, writable flash and captures, not only UART.
    evidence = pathlib.Path(tempfile.mkdtemp(prefix="pulse-native-", dir="/tmp"))
    result = dict(status="FAIL", execution=args.execution, mode=args.mode, feature=args.feature,
                  qualification="Authored fixture: only this run's exercised assertions qualify", unavailable=[])
    proc = qmp = qt = None
    uart = evidence / "uart.log"
    uart_dir = None
    started = time.monotonic()
    try:
        filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(evidence)], text=True).strip()
        require(filesystem in ("ext2/ext3", "ext4", "btrfs", "xfs", "overlayfs", "tmpfs"),
                f"Live build/capture must use native Linux disk, not {filesystem}")
        result["transport"] = dict(live_root=str(evidence), durable_root=str(durable),
                                   filesystem=filesystem, policy="Copy only closed artifacts; no active DrvFS build/flash/capture")
        if args.adc_provider_ready:
            ready = json.loads(args.adc_provider_ready.read_text())
            require(ready.get("status") == "READY", "ADC readiness evidence is not READY")
            result["adc_readiness"] = dict(path=str(args.adc_provider_ready.resolve()), sha256=digest(args.adc_provider_ready), evidence=ready)
        elif args.feature in ("all", "sdm"):
            result["unavailable"].append("ADC: no externally qualified READY provider supplied")
        if args.build:
            args.flash, args.sdkconfig = build(args, evidence)
        args.qemu = args.qemu.resolve(strict=True)
        args.flash = args.flash.resolve(strict=True)
        if args.execution == "firmware":
            configuration(args)
            result["sdkconfig_sha256"] = digest(args.sdkconfig)
            pin_path = args.flash.parent / "build-pin.json"
            if pin_path.exists():
                pin = json.loads(pin_path.read_text())
                require(pin.get("idf_commit") == IDF_COMMIT and pin.get("flash_sha256") == digest(args.flash)
                        and pin.get("sdkconfig_sha256") == digest(args.sdkconfig),
                        "Supplied firmware does not match pinned build provenance")
                result["firmware_build_pin"] = pin
            else:
                result["unavailable"].append("Supplied flash has no build-pin.json; binary hash and sdkconfig are recorded, source provenance is unqualified")
        disk = evidence / "flash-writable.raw"
        shutil.copyfile(args.flash, disk)
        disk.chmod(disk.stat().st_mode | 0o200)
        result["inputs"] = dict(qemu=str(args.qemu), qemu_sha256=digest(args.qemu), flash=str(args.flash), flash_sha256=digest(args.flash), writable_flash_copy=str(disk))
        p = graph(args.mode)
        store(evidence / "project.json", p)
        # Keep the actively written capture on Linux's native filesystem.
        # DrvFS/9p can return ENODATA while QEMU writes a Windows-hosted file.
        # Copy the exact bytes to permanent evidence only after QEMU closes it.
        uart_dir = tempfile.TemporaryDirectory(prefix="pulse-uart-")
        uart_live = pathlib.Path(uart_dir.name) / "uart.log"
        with tempfile.TemporaryDirectory(prefix="pulse-transport-") as tmp, (evidence / "qmp.log").open("w") as qmplog, (evidence / "qtest.log").open("w") as qtlog, (evidence / "stdout.log").open("wb") as output, (evidence / "stderr.log").open("wb") as errors:
            qmp_path, qt_path = (pathlib.Path(tmp) / x for x in ("qmp.sock", "qtest.sock"))
            command = [str(args.qemu), "-machine", "esp32s3", "-nographic", "-S", "-monitor", "none", "-serial", f"file:{uart_live}", "-drive", f"file={disk},if=mtd,format=raw", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
            if args.execution == "qtest":
                command += ["-accel", "qtest", "-qtest", f"unix:{qt_path},server=on,wait=off"]
            else:
                # Match the accepted ordinary RMT functional timebase.
                # Host CPU/solver latency must not become peripheral elapsed time.
                command += ["-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off"]
                result["native_clock"] = dict(mode="icount", shift=0, align=False, sleep=False,
                                             tcg_thread="single", qualification="functional time, not silicon CPU-cycle accuracy")
            result["command"] = command
            store(evidence / "command.json", command)
            proc = subprocess.Popen(command, stdout=output, stderr=errors)
            deadline = time.monotonic() + args.watchdog_seconds
            while not qmp_path.exists() or (args.execution == "qtest" and not qt_path.exists()):
                require(proc.poll() is None, "QEMU exited before native transports opened")
                if time.monotonic() >= deadline:
                    raise TimeoutError("QEMU transport startup watchdog")
                time.sleep(.02)
            qmp = Qmp(qmp_path, deadline, qmplog)
            qmp.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(p)))
            result["initial_snapshot"] = qmp.snapshot()
            if args.feature in ("all", "sdm"):
                result["sdm_profile"] = {
                    prop: qmp.call("qom-get", dict(path="/machine/soc/sdm", property=prop))
                    for prop in ("waveform-profile", "waveform-assumptions")}
                require(result["sdm_profile"]["waveform-profile"] == "s3-sdm-second-order-transfer-v1",
                        "Actual SDM waveform profile differs from approved functional fixture profile")
            if args.execution == "qtest":
                qt = Qtest(qt_path, deadline, qtlog)
                # QTest dummy CPUs require cont for virtual timer dispatch.
                qmp.call("cont")
                result["pulse_assertions"] = run_qtest(qt, qmp, args, evidence)
                if args.feature in ("all", "sdm"):
                    result["unavailable"].append("SDM exact silicon pulse sequence/startup: second-order reference widths not qualified")
                    if args.adc_provider_ready:
                        result["unavailable"].append("ADC conversions are exercised by firmware sdm mode, not this qtest run")
            else:
                qmp.call("cont")
                while True:
                    text = uart_live.read_text(errors="replace") if uart_live.exists() else ""
                    if re.search(r"^PULSE_DONE wiring=\S+ failures=\d+ time_us=\d+\r?\n", text, re.M):
                        break
                    require("ESP_ERROR_CHECK failed:" not in text and "Guru Meditation Error:" not in text,
                            "Ordinary SDK firmware aborted; actual UART capture is retained")
                    require(proc.poll() is None, "QEMU exited before real firmware completion")
                    if time.monotonic() >= deadline:
                        raise TimeoutError("Ordinary IDF firmware did not complete; no result fabricated")
                    time.sleep(.05)
                qmp.call("stop")
                result["firmware_assertions"] = validate_uart(text, args)
                result["unavailable"].extend(result["firmware_assertions"]["unavailable"])
            result["final_snapshot"] = qmp.snapshot()
            result["status"] = "PASS_WITH_EXPLICIT_UNAVAILABLE_STAGES" if result["unavailable"] else "PASS"
            if qt:
                qt.close()
                qt = None
            qmp.close()
            qmp = None
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        result["traceback"] = traceback.format_exc()
    finally:
        for transport in (qt, qmp):
            if transport:
                try:
                    transport.close()
                except OSError:
                    pass
        if proc:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            result["qemu_returncode_after_cleanup"] = proc.returncode
        if uart_dir:
            if uart_live.exists():
                shutil.copyfile(uart_live, uart)
            uart_dir.cleanup()
        result["elapsed_host_seconds"] = time.monotonic() - started
        fixture = ROOT / "tests/firmware/pulse_native"
        source_paths = [pathlib.Path(__file__).resolve(), HERE / "fixtures.json",
                        fixture / "CMakeLists.txt", fixture / "sdkconfig.defaults",
                        fixture / "main/CMakeLists.txt", fixture / "main/Kconfig.projbuild",
                        fixture / "main/pulse_native.c"]
        result["source_hashes"] = {str(p.relative_to(ROOT)): digest(p) for p in source_paths}
        result["evidence_hashes"] = {str(p.relative_to(evidence)): digest(p) for p in evidence.iterdir() if p.is_file()}
        store(evidence / "result.json", result)
        try:
            shutil.copytree(evidence, durable, dirs_exist_ok=True)
        except Exception as exc:
            result["status"] = "FAIL"
            result.setdefault("error", f"Closed artifact copy failed: {type(exc).__name__}: {exc}")
            result["retained_live_directory"] = str(evidence)
            store(durable / "result.json", result)
        else:
            result["artifact_mapping"] = {str(evidence): str(durable)}
            store(durable / "result.json", result)
            shutil.rmtree(evidence)
    print(json.dumps(dict(status=result["status"], evidence=str(durable), error=result.get("error"), unavailable=result["unavailable"])))
    return 1 if result["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
