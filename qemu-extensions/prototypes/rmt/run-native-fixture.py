#!/usr/bin/env python3
"""Build pinned IDF6.1 firmware; apply only public v3 graph; check exact UART.

Run in the activated WSL environment after the RMT provider is READY:
  bash tools/prepare-idf-wsl.sh idf-6.1
  source ~/.cache/esp32s3vm/idf/idf-6.1-fff9895c82d744c7237be8847347bdd1b07c6643/activate.sh
  python3 qemu-extensions/prototypes/rmt/run-native-fixture.py --build \
    --mode connected --qemu /path/to/pinned/qemu-system-xtensa \
    --evidence build-runtime-state/rmt-native-2026-10-07
Repeat with --mode disconnected. --flash and --sdkconfig can reuse a build;
its build-pin.json must accompany the image. No firmware patching, MMIO,
loopback option, QMP register manipulation, or RX-result normalization occurs.
All live capture files, QMP and each session's writable flash are native /tmp
files. Closed artifacts are copied to --evidence only after QEMU exits. The
returned evidence directory is durable; copy failure retains the native
directory and reports failure, never cached bytes or a successful fallback.
Exact UART payload checks qualify closed pulses; the idle trailer is retained
verbatim because the TRM does not specify its final half-duration encoding.
Guest ISR timestamps are not presented as exact electrical edge timestamps.
Electrical qualification additionally requires exact native RMT trace ns and
the actual GPIO-matrix RX subscription for every source transition. A missing
trace, disconnected high, incorrect vector, or unequal sync launch fails.
Builds require the activated canonical native Linux IDF6.1 SDK metadata,
source, tools, Python environment and build root. Fixture inputs are copied
to immutable content-addressed native snapshots; older repository images
and logs are never reset or deleted. No master/6.2 or DrvFS build fallback.
Native waveform qualification uses -accel tcg,thread=single and
-icount shift=0,align=off,sleep=off, retaining the machine's normal CPU count.
This deterministic virtual time is functional modeling, not CPU cycle accuracy.
The graph also powers real native WS2812 and NEC peers. NEC VDD starts at
ground; only after ordinary RX8 is armed does public graph Apply connect
VDD to 3.3 V. Explicit 45 kohm pull-ups keep RX8 idle high in both modes.
GPIO9 is an ordinary input wired to the same physical NEC VDD node. The
guest waits for that rail to read high before its bounded receive interval,
so icount time warp cannot race host graph Apply. It is not a private ACK.
Unpowered/HIZ NEC OUT is checked with independent physical voltage, not
operational digital logic referenced to its off 0V device rails. The explicit
pull must solve to 3.3V; powered logic and actual MCU RX remain separate checks.
NEC comes from the external source, never from a firmware TX stand-in.
WS2812 must actually latch the three GRB pixels through wired DIN4.
These named functional profiles are not physical metrology. A native sensor
model remains a separate registry prerequisite; no sensor response is faked.
The approved quantity grammar uses count for integer period_ms; the field
name, not an unsupported ms quantity unit, defines its millisecond meaning.
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time

IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
PREFIX = "dbe2a8a5ad4e07a6"
FIXTURE_WIRING = {"tx_gpio": 4, "rx_gpio": 5, "sync_tx_gpio": 6, "sync_rx_gpio": 7,
                  "external_nec_rx_gpio": 8, "external_nec_vdd_monitor_gpio": 9}
PHASES = ("external_nec", "apb_1mhz", "long_refill", "noise_filter", "disable_midstream",
          "disable_recovery", "carrier", "synchronization", "nec_ir", "xtal_2mhz", "apb_500khz",
          "ws2812_grb", "dma_long")
VECTORS = {"apb_1mhz": (8, 1000000), "long_refill": (120, 1000000),
           "disable_recovery": (8, 1000000), "synchronization": (8, 1000000),
           "xtal_2mhz": (8, 2000000), "apb_500khz": (8, 500000),
           "nec_ir": (33, 1000000), "ws2812_grb": (72, 20000000), "dma_long": (120, 1000000)}
RX_VECTORS = {"external_nec": (33, 1000000), **VECTORS}
NEC_BYTES = bytes.fromhex("34cba758")
WS2812_BYTES = bytes.fromhex("1234568001feff007f")


def phase_symbols(phase):
    count, _ = RX_VECTORS[phase]
    if phase in ("nec_ir", "external_nec"):
        mark, space = (0, 1) if phase == "external_nec" else (1, 0)
        symbols = [(mark, 9000, space, 4500)]
        symbols.extend((mark, 560, space, 1690 if (NEC_BYTES[bit // 8] >> (bit % 8)) & 1 else 560)
                       for bit in range(32))
        return symbols + [(mark, 560, space, 1000)]
    if phase == "ws2812_grb":
        symbols = [(1, 16, 0, 9) if (WS2812_BYTES[bit // 8] >> (7 - bit % 8)) & 1
                   else (1, 8, 0, 17) for bit in range(72)]
        return symbols
    return [(1, 31 + (i * 17) % 71, 0, 43 + (i * 13) % 83)
            for i in range(count)] + [(1, 37, 0, 11)]


def decode_content(phase, observed, duration_ns=None):
    """Decode measured input pulse intervals, not expected/source bytes."""
    periods = [(observed[2 * bit + 1][0] - observed[2 * bit][0],
                observed[2 * bit + 2][0] - observed[2 * bit + 1][0])
               for bit in range(len(observed) // 2 - 1)]
    if phase in ("nec_ir", "external_nec"):
        require(len(periods) == 33 and periods[0] == (9000000, 4500000), "Actual NEC input header mismatch")
        bits = []
        for high, low in periods[1:]:
            require(high == 560000 and low in (560000, 1690000), f"Actual NEC input bit mismatch: {high}/{low}")
            bits.append(int(low == 1690000))
        payload = bytes(sum(bits[byte * 8 + bit] << bit for bit in range(8)) for byte in range(4))
        require(payload == NEC_BYTES and payload[0] ^ payload[1] == 255 and payload[2] ^ payload[3] == 255,
                f"Actual NEC address/command mismatch: {payload.hex()}")
        return dict(protocol="NEC", bytes=payload.hex(), address=payload[0], command=payload[2],
                    observer="actual resolved GPIO matrix RX subscription ns")
    # The final WS bit's low continues into real reset, without a next rising
    # edge. TX stop bounds its nominal low; actual peer latch proves reset.
    require(len(observed) == 144 and duration_ns is not None, "Actual WS2812 input edge count/stop missing")
    periods.append((observed[-1][0] - observed[-2][0], duration_ns - observed[-1][0]))
    require(len(periods) == 72, "Actual WS2812 input did not contain three pixels")
    bits = []
    for high, low in periods:
        require((high, low) in ((400, 850), (800, 450)), f"Actual WS2812 input bit mismatch: {high}/{low}")
        bits.append(int(high == 800))
    payload = bytes(sum(bits[byte * 8 + bit] << (7 - bit) for bit in range(8)) for byte in range(9))
    require(payload == WS2812_BYTES, f"Actual WS2812 GRB bytes mismatch: {payload.hex()}")
    return dict(protocol="WS2812", order="GRB", bytes=payload.hex(),
                pixels=[dict(g=payload[i], r=payload[i + 1], b=payload[i + 2]) for i in range(0, 9, 3)],
                observer="actual resolved GPIO matrix RX subscription ns")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def project(mode, nec_powered=False):
    def terminal(cid, name, domain, direction="passive", gpio=None):
        value = dict(id=f"{cid}.{name}", name="NEC VDD rail monitor" if gpio == 9 else name,
                     role="gpio" if gpio is not None else name,
                     domain=domain, direction=direction)
        if gpio is not None:
            value["gpio"] = gpio
        return value

    def component(cid, kind, terminals, parameters=None, model=None):
        return dict(id=cid, name=cid, kind=kind, type=model or kind,
                    terminals=terminals, parameters=parameters or {})

    components = [
        component("U1", "mcu", [terminal("U1", "vdd", "power", "input"),
                  terminal("U1", "gnd", "ground", "input")] +
                  [terminal("U1", f"io{pad}", "digital", "inout", pad) for pad in (4, 5, 6, 7, 8, 9)]),
        component("G", "ground", [terminal("G", "ref", "ground")]),
        component("V", "voltage-source", [terminal("V", "p", "power", "output"),
                  terminal("V", "n", "ground", "input")], {"voltage": dict(value=3.3, unit="V")}),
        component("WS", "device", [terminal("WS", "vdd", "power", "input"),
                  terminal("WS", "gnd", "ground", "input"), terminal("WS", "din", "digital", "input")],
                  {"led_count": dict(value=3, unit="count")}, "ws2812-functional-3v3"),
        component("IR", "device", [terminal("IR", "vdd", "power", "input"),
                  terminal("IR", "gnd", "ground", "input"), terminal("IR", "out", "digital", "output")],
                  {"address": dict(value=0x34, unit="count"), "command": dict(value=0xa7, unit="count"),
                   "period_ms": dict(value=1000, unit="count")}, "nec-envelope-source"),
        component("PullIR", "resistor", [terminal("PullIR", "a", "passive"),
                  terminal("PullIR", "b", "passive")], {"resistance": dict(value=45000, unit="ohm")}),
        component("PullIRSource", "resistor", [terminal("PullIRSource", "a", "passive"),
                  terminal("PullIRSource", "b", "passive")], {"resistance": dict(value=45000, unit="ohm")}),
    ]
    endpoints = {"gnd": ["G.ref", "V.n", "U1.gnd", "WS.gnd", "IR.gnd"],
                 "vdd": ["V.p", "U1.vdd", "WS.vdd", "PullIR.b", "PullIRSource.b"],
                 "ir_rx": ["U1.io8", "PullIR.a"]}
    # Actual rail topology, never a provider flag or receive-data injection.
    endpoints["vdd" if nec_powered else "gnd"].extend(("IR.vdd", "U1.io9"))
    for index, (tx, rx) in enumerate(((4, 5), (6, 7))):
        cid = f"Pull{index}"
        components.append(component(cid, "resistor", [terminal(cid, "a", "passive"),
                          terminal(cid, "b", "passive")], {"resistance": dict(value=10000, unit="ohm")}))
        endpoints["gnd"].append(f"{cid}.b")
        endpoints[f"rx{index}"] = [f"U1.io{rx}", f"{cid}.a"]
        if mode == "connected":
            endpoints[f"rx{index}"].append(f"U1.io{tx}")
        else:
            endpoints[f"tx{index}"] = [f"U1.io{tx}"]
    if mode == "connected":
        endpoints["rx0"].append("WS.din")
        endpoints["ir_rx"].extend(("IR.out", "PullIRSource.a"))
    else:
        components.extend((
            component("PullWS", "resistor", [terminal("PullWS", "a", "passive"),
                      terminal("PullWS", "b", "passive")], {"resistance": dict(value=10000, unit="ohm")}),
        ))
        endpoints["ws_input"] = ["WS.din", "PullWS.a"]
        endpoints["gnd"].append("PullWS.b")
        endpoints["ir_source"] = ["IR.out", "PullIRSource.a"]
    return dict(version=3, id=f"rmt-native-{mode}", name=f"Ordinary native RMT {mode}",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
                firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
                components=components,
                nets=[dict(id=name, name=name, endpoints=ends) for name, ends in endpoints.items()],
                geometry=dict(components={}, nets={}))


class Qmp:
    def __init__(self, path, transcript, deadline):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(min(10, max(.001, deadline - time.monotonic())))
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)
        self.transcript, self.deadline, self.sequence = transcript, deadline, 0
        require("QMP" in self.receive(), "Invalid QMP greeting")
        self.call("qmp_capabilities")

    def receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Host watchdog during QMP")
        self.sock.settimeout(min(10, remaining))
        line = self.stream.readline()
        if not line:
            raise RuntimeError("QMP closed before response")
        response = json.loads(line)
        self.transcript.append(dict(direction="receive", message=response))
        return response

    def call(self, command, arguments=None):
        self.sequence += 1
        request = dict(execute=command, arguments=arguments or {}, id=self.sequence)
        self.transcript.append(dict(direction="send", message=request))
        self.stream.write((json.dumps(request) + "\n").encode())
        while True:
            response = self.receive()
            if "event" in response:
                continue
            require(response.get("id") == request["id"], f"Mismatched QMP response: {response}")
            if "error" in response:
                raise RuntimeError(response["error"])
            return response["return"]

    def snapshot(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))

    def peers(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/rmt-peers", property="capture-json")))

    def close(self):
        try:
            self.stream.close()
        finally:
            self.sock.close()


def configuration(path, mode):
    values = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", path.read_text(), re.MULTILINE))
    selected = [key for key, value in values.items() if key.startswith("CONFIG_RMT_NATIVE_PROFILE_") and value == "y"]
    require(selected == [f"CONFIG_RMT_NATIVE_PROFILE_{mode.upper()}"], f"Wrong firmware profile: {selected}")
    require(values.get("CONFIG_IDF_TARGET") == '"esp32s3"', "Firmware target is not ESP32S3")
    return values


def build(args, evidence):
    root = pathlib.Path(__file__).resolve().parents[3]
    fixture = root / "tests/firmware/rmt_native"
    required_env = ("IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH",
                    "ESP32S3_IDF_BUILD_ROOT", "ESP32S3_IDF_METADATA")
    missing = [name for name in required_env if not os.environ.get(name)]
    require(not missing, f"Activate the canonical idf-6.1 SDK first; missing {missing}")
    metadata_path = pathlib.Path(os.environ["ESP32S3_IDF_METADATA"]).resolve(strict=True)
    sdk = json.loads(metadata_path.read_text())
    require(sdk["schema_version"] == 1 and sdk["profile"] == "idf-6.1" and
            sdk["commit"] == IDF_COMMIT and sdk["target"] == "esp32s3" and
            sdk["repository"] == "https://github.com/espressif/esp-idf.git",
            "Activated SDK metadata is not canonical locked IDF6.1")
    idf = pathlib.Path(os.environ["IDF_PATH"]).resolve(strict=True)
    build_root = pathlib.Path(os.environ["ESP32S3_IDF_BUILD_ROOT"]).resolve(strict=True)
    tools = pathlib.Path(os.environ["IDF_TOOLS_PATH"]).resolve(strict=True)
    python_env = pathlib.Path(os.environ["IDF_PYTHON_ENV_PATH"]).resolve(strict=True)
    for key, path in (("source", idf), ("build_root", build_root), ("tools_path", tools),
                      ("python_environment", python_env)):
        require(path == pathlib.Path(sdk[key]).resolve(strict=True), f"Activated {key} differs from canonical SDK metadata")
        filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(path)], text=True).strip()
        require(filesystem in ("ext2/ext3", "ext4", "btrfs", "xfs", "overlayfs", "tmpfs"),
                f"Canonical SDK/build inputs must be native Linux disk, not {filesystem}: {path}")
    commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    origin = subprocess.check_output(["git", "-C", str(idf), "remote", "get-url", "origin"], text=True).strip()
    require(commit == IDF_COMMIT and origin == sdk["repository"], "Native SDK source identity differs from locked metadata")
    subprocess.run(["git", "-C", str(idf), "diff", "--quiet", "HEAD", "--ignore-submodules=all"], check=True)
    require(sha256(idf / "tools/tools.json") == sdk["tools_manifest_sha256"], "Native SDK tool manifest differs from prepared identity")
    # Preserve the venv interpreter path instead of resolving its symlink to
    # system Python and silently losing the activated release environment.
    python = python_env / "bin/python"
    require(python.is_file(), "Canonical IDF6.1 Python environment interpreter is missing")
    paths = [fixture / "CMakeLists.txt", fixture / "sdkconfig.defaults"]
    paths += sorted(path for path in (fixture / "main").rglob("*") if path.is_file())
    contents = {str(path.relative_to(fixture)): path.read_bytes() for path in paths}
    original_hashes = {name: hashlib.sha256(content).hexdigest() for name, content in contents.items()}
    contents["sdkconfig.defaults"] = contents["sdkconfig.defaults"].replace(
        b"CONFIG_RMT_NATIVE_PROFILE_CONNECTED=y",
        f"CONFIG_RMT_NATIVE_PROFILE_{args.mode.upper()}=y".encode())
    files = {name: hashlib.sha256(content).hexdigest() for name, content in contents.items()}
    source_hash = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    # Match the existing canonical build-boot-fixture.sh snapshot convention.
    project = build_root / "rmt_native" / source_hash / "source"
    source_manifest = project.parent / "source-manifest.json"
    if not source_manifest.exists():
        project.mkdir(parents=True, exist_ok=True)
        for name, content in contents.items():
            target = project / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                require(target.read_bytes() == content, f"Incomplete native snapshot changed: {target}")
            else:
                target.write_bytes(content)
        source_manifest.write_text(json.dumps(dict(original_source=str(fixture), source_sha256=source_hash,
                                                   source_files=files, original_source_files=original_hashes,
                                                   profile="idf-6.1", mode=args.mode, fixture_wiring=FIXTURE_WIRING), indent=2) + "\n")
    manifest = json.loads(source_manifest.read_text())
    require(manifest["source_sha256"] == source_hash and manifest["source_files"] == files,
            "Native source snapshot manifest differs; refusing to overwrite it")
    for name, expected in files.items():
        require(sha256(project / name) == expected, f"Native fixture snapshot changed: {name}")
    shutil.copyfile(metadata_path, evidence / "sdk-manifest.json")
    shutil.copyfile(source_manifest, evidence / "source-manifest.json")
    build_dir = project.parent / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    config = build_dir / "sdkconfig"
    if config.exists():
        configuration(config, args.mode)
    command = [str(python), str(idf / "tools/idf.py"), "-B", str(build_dir), "-DIDF_TARGET=esp32s3",
               f"-DSDKCONFIG={config}", f"-DSDKCONFIG_DEFAULTS={project / 'sdkconfig.defaults'}", "build"]
    with (evidence / "build.log").open("wb") as output:
        subprocess.run(command, cwd=project, stdout=output, stderr=subprocess.STDOUT, check=True)
    flash = build_dir / "rmt-native.merged.bin"
    merge = [str(python), "-m", "esptool", "--chip", "esp32s3", "merge-bin", "--fill-flash-size", "4MB",
             "-o", str(flash), "@flash_args"]
    with (evidence / "merge.log").open("wb") as output:
        subprocess.run(merge, cwd=build_dir, stdout=output, stderr=subprocess.STDOUT, check=True)
    description = json.loads((build_dir / "project_description.json").read_text())
    compiler = subprocess.check_output([description["c_compiler"], "--version"], text=True).splitlines()[0]
    pin = dict(idf_commit=commit, flash_sha256=sha256(flash), sdkconfig_sha256=sha256(config),
               elf_sha256=sha256(build_dir / "rmt_native.elf"), compiler=compiler,
               build_command=command, merge_command=merge, sources=files,
               fixture_wiring=FIXTURE_WIRING,
               sdk_metadata=sdk, sdk_metadata_sha256=sha256(metadata_path),
               source_snapshot=str(project), source_manifest=manifest,
               source_manifest_sha256=sha256(source_manifest))
    (build_dir / "build-pin.json").write_text(json.dumps(pin, indent=2) + "\n")
    return flash, config


def validate_uart(text, mode):
    require(re.search(rf"^RMT_NATIVE_BOOT profile={mode} idf=6.1 tx_gpio=4 rx_gpio=5 sync_tx_gpio=6 sync_rx_gpio=7$", text, re.M), "Missing exact boot declaration")
    require(re.search(r"^RMT_NATIVE_CONTENT phase=nec_ir protocol=NEC address=34 command=a7 bytes=34cba758$", text, re.M),
            "Missing exact NEC content declaration")
    require(re.search(r"^RMT_NATIVE_CONTENT phase=ws2812_grb protocol=WS2812 order=GRB pixels=3 bytes=1234568001feff007f$", text, re.M),
            "Missing exact WS2812 content declaration")
    require(re.search(r"^RMT_NATIVE_CONTENT phase=external_nec protocol=NEC address=34 command=a7 bytes=34cba758 origin=external_model$", text, re.M),
            "Missing ordinary RX external NEC declaration")
    require(len(re.findall(r"^RMT_NATIVE_EXTERNAL_READY rx_gpio=8 armed_us=\d+$", text, re.M)) == 1,
            "Missing/duplicate armed external RX marker")
    power = re.findall(r"^RMT_NATIVE_POWER phase=external_nec gpio=9 level=1 observed_us=(\d+)$", text, re.M)
    require(len(power) == 1, "Missing/duplicate actual NEC VDD GPIO9 power observation")
    if mode == "connected":
        require(re.search(r"^RMT_NATIVE_DECODE phase=external_nec address=34 command=a7 bytes=34cba758$", text, re.M),
                "External NEC driver capture did not decode exact address and command")
    done = re.findall(r"^RMT_NATIVE_DONE profile=(\w+) failures=(\d+)$", text, re.M)
    require(done == [(mode, "0")], f"Firmware failed or missing completion: {done}")
    checks = re.findall(r"^RMT_NATIVE_CHECK phase=(\w+) name=(\w+) result=(PASS|FAIL)$", text, re.M)
    require(checks and all(result == "PASS" for _, _, result in checks), "Firmware acceptance assertion failed")
    found = {(phase, name) for phase, name, _ in checks}
    for phase in PHASES:
        require(re.search(rf"^RMT_NATIVE_PHASE name={phase} ", text, re.M), f"Missing phase {phase}")
    expected = {("noise_filter", "glitch_no_completion"), ("disable_midstream", "loop_wait_timeout"),
                ("disable_midstream", "no_fabricated_tx_done"), ("synchronization", "first_withheld"),
                ("synchronization", "both_start_after_second"), ("cleanup", "delete_encoder"),
                ("external_nec", "external_no_tx"), ("external_nec", "power_monitor_input"),
                ("external_nec", "initial_power_off"), ("external_nec", "before_first_frame_empty")}
    for phase in VECTORS:
        expected.add((phase, "tx_single_done"))
        if phase != "synchronization":
            expected.add((phase, "tx_not_immediate"))
    for phase in RX_VECTORS:
        required_rx = ("rx_idle_completion", "rx_exact_count", "rx_exact_payload",
                       "rx_final_bit" if phase == "ws2812_grb" else "rx_guard_pulse")
        for name in (required_rx if mode == "connected" else ("disconnected_no_payload",)):
            expected.add((phase, name))
    if mode == "connected":
        expected |= {("long_refill", "rx_ring_partial_progress"), ("ws2812_grb", "rx_ring_partial_progress"),
                     ("noise_filter", "valid_exact_pulse"), ("disable_midstream", "loop_prefix_exact"),
                     ("disable_midstream", "stop_causes_idle"), ("carrier", "carrier_exact_raw_pulses")}
        expected.add(("external_nec", "external_nec_decode"))
    else:
        expected |= {(phase, "disconnected_no_payload") for phase in ("noise_filter", "disable_midstream", "carrier")}
    require(expected <= found, f"Missing required UART checks: {sorted(expected - found)}")
    symbols = {}
    for phase, lane, index, l0, d0, l1, d1 in re.findall(
            r"^RMT_NATIVE_SYMBOL phase=(\w+) lane=(\d+) index=(\d+) l0=(\d+) d0=(\d+) l1=(\d+) d1=(\d+)$", text, re.M):
        key = (phase, int(lane))
        values = symbols.setdefault(key, [])
        require(int(index) == len(values), f"Noncontiguous RX symbol index {key}/{index}")
        values.append(tuple(map(int, (l0, d0, l1, d1))))
    if mode == "disconnected":
        require(not symbols, "Disconnected graph produced fabricated RX payload")
    else:
        for phase, (count, _) in RX_VECTORS.items():
            for lane in (range(2) if phase == "synchronization" else range(1)):
                actual = symbols.get((phase, lane), [])
                expected_symbols = phase_symbols(phase)
                require(len(actual) == len(expected_symbols), f"Wrong RX count {phase}/{lane}: {len(actual)}")
                require(actual[:-1] == expected_symbols[:-1], f"Exact closed RX vector mismatch {phase}/{lane}")
                require(actual[-1][:3] == expected_symbols[-1][:3], f"Final pulse mismatch {phase}/{lane}")
        carrier = symbols.get(("carrier", 0), [])
        require(len(carrier) == 10 and carrier[:9] == [(1, 5, 0, 5)] * 9 and carrier[9][:3] == (1, 5, 0), "Raw native carrier vector mismatch")
    timestamps = []
    for match in re.finditer(r"^RMT_NATIVE_TIME phase=(\w+) lane=(\d+) submit_us=(-?\d+) tx_done_us=(-?\d+) rx_done_us=(-?\d+) tx_events=(\d+) rx_events=(\d+) partial_events=(\d+) tx_symbols=(\d+) rx_symbols=(\d+)$", text, re.M):
        phase, *fields = match.groups()
        lane, submit, tx, rx, tx_events, rx_events, partial, tx_count, rx_count = map(int, fields)
        timestamps.append(dict(phase=phase, lane=lane, submit_us=submit, tx_done_us=tx, rx_done_us=rx,
                               tx_events=tx_events, rx_events=rx_events, partial_events=partial,
                               tx_symbols=tx_count, rx_symbols=rx_count))
        if phase in VECTORS and phase != "synchronization":
            count, resolution = VECTORS[phase]
            ticks = sum(high + low for _, high, _, low in phase_symbols(phase))
            require(tx - submit >= ticks * 1000000 // resolution, f"Immediate/early TX DONE {phase}")
    require({entry["phase"] for entry in timestamps} == set(PHASES), "Missing UART callback timestamps")
    return dict(checks=[dict(phase=p, name=n, result=r) for p, n, r in checks], timestamps=timestamps,
                limitations=re.findall(r"^RMT_NATIVE_LIMIT .*$", text, re.M))


def trace_records(text):
    """Use model ns fields, never the trace backend's host timestamps."""
    records = []
    for match in re.finditer(r"esp32s3_rmt_(edge|input|rx_symbol|tx_start|tx_stop)\s+([^\r\n]+)", text):
        kind, body = match.groups()
        fields = dict(re.findall(r"(\w+)=(0x[0-9a-fA-F]+|-?\d+)", body))
        require("ch" in fields and "ns" in fields, f"Malformed native trace: {match.group(0)}")
        records.append(dict(kind=kind, **{key: int(value, 16 if value.startswith("0x") else 10)
                                        for key, value in fields.items()}))
    require(records, "Missing native model timestamp trace")
    return records


def validate_model_trace(text, mode):
    records = trace_records(text)
    starts = {ch: [record for record in records if record["kind"] == "tx_start" and record["ch"] == ch]
              for ch in (0, 1, 3)}
    phases = ("apb_1mhz", "long_refill", "noise_filter_glitch", "noise_filter_valid",
              "disable_midstream", "disable_recovery", "carrier", "synchronization",
              "nec_ir", "xtal_2mhz", "apb_500khz", "ws2812_grb")
    require(len(starts[0]) == len(phases) and len(starts[1]) == 1 and len(starts[3]) == 1,
            f"Wrong native transaction starts: ch0={len(starts[0])}, ch1={len(starts[1])}, ch3={len(starts[3])}")
    runs = []
    if mode == "disconnected":
        require(not any(record["kind"] == "rx_symbol" for record in records), "Disconnected RX fabricated RAM symbols")
    for channel, labels in ((0, phases), (1, ("synchronization",)), (3, ("dma_long",))):
        for index, phase in enumerate(labels):
            start = starts[channel][index]["ns"]
            next_start = starts[channel][index + 1]["ns"] if index + 1 < len(starts[channel]) else None
            stops = [record["ns"] for record in records if record["kind"] == "tx_stop" and
                     record["ch"] == channel and record["ns"] >= start and
                     (next_start is None or record["ns"] < next_start)]
            require(len(stops) == 1, f"Missing/duplicate actual stop {phase}/{channel}: {stops}")
            stop = stops[0]
            # CONF_UPDATE/idle publication can repeat the same driven level.
            # Keep every real transition and its native timestamp unchanged.
            transitions, previous = [], 0
            for record in records:
                if record["kind"] != "edge" or record["ch"] != channel or not start <= record["ns"] <= stop:
                    continue
                require(record.get("enabled") == 1, f"Unexpected released source during {phase}/{channel}")
                if record["level"] != previous:
                    transitions.append((record["ns"] - start, record["level"]))
                    previous = record["level"]
            if phase in VECTORS:
                count, resolution = VECTORS[phase]
                tick_ns = 1000000000 // resolution
                expected, ticks = [], 0
                for _, high, _, low in phase_symbols(phase):
                    expected.extend(((ticks * tick_ns, 1), ((ticks + high) * tick_ns, 0)))
                    ticks += high + low
                duration = ticks * tick_ns
            elif phase == "carrier":
                expected = [(edge * 5000, 1 if edge % 2 == 0 else 0) for edge in range(20)]
                duration = 150000
            elif phase.startswith("noise_filter_"):
                high, low = (2, 1) if phase.endswith("glitch") else (20, 10)
                expected = [(0, 1), (high * 1000, 0)]
                duration = (high + low) * 1000
            else:
                require(stop - start >= 2000000, "Infinite loop stopped before native midstream timeout")
                expected = [(edge * 200000, 1 if edge % 2 == 0 else 0)
                            for edge in range((stop - start + 199999) // 200000)]
                if expected and expected[-1][1] == 1:
                    expected.append((stop - start, 0))
                duration = stop - start
            require(transitions == expected, f"Exact native source waveform mismatch {phase}/{channel}: actual={transitions} expected={expected}")
            require(stop - start == duration, f"Wrong exact native stop duration {phase}/{channel}: {stop - start} != {duration}")
            observed, previous = [], 0
            if mode == "connected":
                for record in records:
                    if record["kind"] != "input" or record["ch"] != channel or not start <= record["ns"] <= stop:
                        continue
                    require(record.get("valid") == 1, f"Invalid actual graph input during {phase}/{channel}")
                    if record["level"] != previous:
                        observed.append((record["ns"] - start, record["level"]))
                        previous = record["level"]
                require(observed == transitions, f"Actual graph input waveform differs from source {phase}/{channel}: {observed}")
            else:
                require(not any(record["kind"] == "input" and record["ch"] == channel and
                                start <= record["ns"] <= stop and record.get("valid") == 1 and record.get("level") == 1
                                for record in records), f"Disconnected TX waveform reached RX {phase}/{channel}")
            content = decode_content(phase, observed, stop - start) if mode == "connected" and phase in ("nec_ir", "ws2812_grb") else None
            rx_symbols = [record for record in records if record["kind"] == "rx_symbol" and
                          record["ch"] == channel and record["ns"] >= start and
                          (next_start is None or record["ns"] < next_start)]
            if mode == "connected" and phase == "long_refill":
                require(len(rx_symbols) == 121, f"Long RX RAM trace count is {len(rx_symbols)}, not 121")
                require([record["index"] for record in rx_symbols] == [symbol % 48 for symbol in range(121)],
                        "Long RX did not advance and wrap the actual 48-symbol hardware ring")
            runs.append(dict(phase=phase, channel=channel, start_ns=start, stop_ns=stop,
                             transitions=[dict(relative_ns=ns, level=level) for ns, level in transitions],
                             actual_input_transitions=[dict(relative_ns=ns, level=level) for ns, level in observed],
                             decoded_content=content, rx_ram_symbols=rx_symbols))
    synchronized = [run for run in runs if run["phase"] == "synchronization"]
    require(synchronized[0]["start_ns"] == synchronized[1]["start_ns"], "Native sync channels did not launch at exactly the same model ns")
    require(synchronized[0]["stop_ns"] == synchronized[1]["stop_ns"], "Native sync channels did not finish identical vectors together")
    return dict(status="PASS", transactions=runs, actual_input_events=sum(record["kind"] == "input" for record in records),
                qualification="exact source model ns plus actual GPIO/v3 graph RX subscription at every transition")


def decimal_ns(value, label):
    require(isinstance(value, str) and re.fullmatch(r"\d+", value), f"{label} must be a decimal string")
    return int(value)


def peer_map(snapshot):
    require(snapshot["abi"] == 1 and snapshot["healthy"] is True, "Native RMT peer service is not healthy ABI1")
    decimal_ns(snapshot["sample_ns"], "peer sample_ns")
    peers = {peer["component_id"]: peer for peer in snapshot["peers"]}
    require(len(snapshot["peers"]) == 2 and set(peers) == {"WS", "IR"}, "Native service did not resolve the two actual graph peers")
    require(peers["WS"]["model"] == "ws2812-functional-3v3" and peers["IR"]["model"] == "nec-envelope-source",
            "Native peer model identity mismatch")
    for cid, peer in peers.items():
        require(peer["power_known"] is True, f"Unknown actual peer rails: {cid}")
        for field in ("power_epoch", "power_on_ns", "frame_count"):
            decimal_ns(peer[field], f"{cid}.{field}")
    ir = peers["IR"]
    voltage = ir["out_voltage_v"]
    if ir["out_voltage_valid"] is True:
        require(isinstance(voltage, (int, float)) and not isinstance(voltage, bool) and math.isfinite(voltage),
                "Actual NEC physical OUT voltage is not a finite number")
    else:
        require(ir["out_voltage_valid"] is False and voltage is None,
                "Unknown/floating NEC OUT must report invalid physical voltage and null, not guessed zero")
    return peers


def validate_peer_capture(snapshots, model_assertions, trace_text, mode):
    require([snapshot["phase"] for snapshot in snapshots] ==
            ["graph-applied-before-boot", "external-nec-powered-after-rx-armed", "firmware-completed"],
            "Missing actual peer/power snapshots")
    initial, powered, completed = [peer_map(snapshot["peers"]) for snapshot in snapshots]
    require(initial["WS"]["powered"] is True and initial["IR"]["powered"] is False,
            "Initial real WS/NEC rail topology is incorrect")
    require(initial["WS"]["frame_count"] == "0" and initial["WS"]["latch_valid"] is False and
            initial["WS"]["grb"] is None and initial["IR"]["frame_count"] == "0",
            "Native peers fabricated content before any actual waveform")
    require(initial["IR"]["drive_oe"] is False and initial["IR"]["out_valid"] is False and
            initial["IR"]["out_level"] is None, "OFF NEC incorrectly claims operational digital logic from its 0V rails")
    require(initial["IR"]["out_voltage_valid"] is True and
            math.isclose(initial["IR"]["out_voltage_v"], 3.3, rel_tol=0, abs_tol=1e-6),
            "Actual OFF/HIZ NEC OUT does not solve to the explicitly pulled 3.3V node")
    for snapshot in (powered, completed):
        require(snapshot["IR"]["out_voltage_valid"] is True, "Powered NEC OUT lacks an actual solved physical voltage")
        require(snapshot["IR"]["powered"] is True and snapshot["WS"]["powered"] is True,
                "Explicit 3.3V rails did not power both actual peers")
        require(snapshot["IR"]["address"] == 0x34 and snapshot["IR"]["command"] == 0xa7 and
                snapshot["IR"]["period_ms"] == 1000, "Actual external NEC immutable dataset differs")
        require(snapshot["IR"]["drive_level"] is False and snapshot["IR"]["out_valid"] is True and
                isinstance(snapshot["IR"]["out_level"], bool), "Actual NEC open-drain OUT is not digitally resolved")
        require(snapshot["IR"]["out_level"] is not snapshot["IR"]["drive_oe"],
                "Actual finite open-drain/pull physical OUT disagrees with its driven-low/released branch")
        require(snapshot["WS"]["led_count"] == 3, "Actual WS peer does not contain three configured pixels")
    require(powered["IR"]["drive_oe"] is False and powered["IR"]["out_level"] is True,
            "NEC power-on fabricated a mark before its real 100ms power delay")
    require(math.isclose(powered["IR"]["out_voltage_v"], 3.3, rel_tol=0, abs_tol=1e-6),
            "Actual powered/released NEC OUT is not held at 3.3V by its explicit pull")
    power_on = decimal_ns(powered["IR"]["power_on_ns"], "NEC actual power_on_ns")
    require(decimal_ns(initial["IR"]["power_epoch"], "NEC initial epoch") <
            decimal_ns(powered["IR"]["power_epoch"], "NEC powered epoch"), "Actual NEC power transition lacks a new epoch")
    require(powered["IR"]["power_epoch"] == completed["IR"]["power_epoch"] and
            powered["IR"]["power_on_ns"] == completed["IR"]["power_on_ns"], "NEC power identity changed without another physical cycle")
    require(initial["WS"]["power_epoch"] == powered["WS"]["power_epoch"] == completed["WS"]["power_epoch"] and
            initial["WS"]["power_on_ns"] == powered["WS"]["power_on_ns"] == completed["WS"]["power_on_ns"],
            "NEC-only rail Apply fabricated a WS power cycle")
    require(decimal_ns(completed["IR"]["frame_count"], "NEC frame_count") >= 1, "Real powered NEC source never completed a frame")
    require(decimal_ns(completed["IR"]["frame_abort_count"], "NEC frame_abort_count") == 0,
            "External source aborted a native frame; exact waveform qualification failed")
    records = trace_records(trace_text)
    first_frame = power_on + 100000000
    duration, expected = 0, []
    for _, mark, _, space in phase_symbols("external_nec"):
        expected.extend(((duration, 0), (duration + mark * 1000, 1)))
        duration += (mark + space) * 1000
    # Last programmed TX low is irrelevant here: real NEC ends with release.
    frame_end = first_frame + expected[-1][0]
    actual, previous = [], 1
    for record in records:
        if record["kind"] != "input" or record["ch"] != 0 or not first_frame <= record["ns"] <= frame_end:
            continue
        require(record.get("valid") == 1, "Actual external NEC GPIO8 input became invalid")
        if record["level"] != previous:
            actual.append((record["ns"] - first_frame, record["level"]))
            previous = record["level"]
    decoded = None
    if mode == "connected":
        require(actual == expected, f"Actual external NEC input timestamps differ from powered source reference: {actual}")
        decoded = decode_content("external_nec", actual)
        first_tx = min(record["ns"] for record in records if record["kind"] == "tx_start")
        ram = [record for record in records if record["kind"] == "rx_symbol" and record["ch"] == 0 and
               first_frame <= record["ns"] < first_tx]
        require(len(ram) == 34 and [record["index"] for record in ram] == list(range(34)),
                "Ordinary external NEC RMT RX did not store the complete real frame")
        for record, (l0, d0, l1, d1) in zip(ram[:-1], phase_symbols("external_nec")[:-1]):
            require(record["value"] == d0 | (l0 << 15) | (d1 << 16) | (l1 << 31),
                    "Actual external NEC RMT RAM closed-pulse word mismatch")
        require(ram[-1]["value"] & 0xffff == 560 and ram[-1]["value"] >> 31 == 1,
                "Actual external NEC RMT RAM trailer mark mismatch")
        ws_run = next(run for run in model_assertions["transactions"] if run["phase"] == "ws2812_grb")
        ws = completed["WS"]
        require(ws["latch_valid"] is True and ws["grb"] == WS2812_BYTES.hex() and ws["frame_count"] == "1",
                "Actual wired WS peer did not latch exactly the three GRB pixels")
        last_latch = decimal_ns(ws["last_latch_ns"], "WS last_latch_ns")
        last_fall = ws_run["start_ns"] + ws_run["actual_input_transitions"][-1]["relative_ns"]
        dma_start = next(run["start_ns"] for run in model_assertions["transactions"] if run["phase"] == "dma_long")
        require(last_fall + 50000 <= last_latch < dma_start, "WS actual latch lacks the real >=50us reset before subsequent traffic")
    else:
        require(not actual, "Disconnected GPIO8 received the external NEC source waveform")
        ws = completed["WS"]
        require(ws["latch_valid"] is False and ws["grb"] is None and ws["frame_count"] == "0",
                "Disconnected WS DIN fabricated a latched payload")
    return dict(status="PASS", actual_peer_capture=completed, external_first_frame_ns=first_frame,
                external_input_transitions=[dict(relative_ns=ns, level=level) for ns, level in actual],
                external_nec_decoded=decoded, qualification="actual powered external models through the real v3 graph")


def capture_pre_cleanup_state(qmp, proc):
    if qmp is None or proc is None or proc.poll() is not None:
        return dict(available=False, reason="No live QMP-connected QEMU process")
    # This separate diagnostic deadline never extends guest observation.
    # Preserve pre-stop status, then read frozen public state/PCs before
    # cleanup; closed-capture oracle failures can occur only after exit.
    qmp.deadline = time.monotonic() + 10
    diagnostics = dict(available=True, diagnostic_budget_seconds=10)
    operations = (
        ("status_before_stop", lambda: qmp.call("query-status")),
        ("stop", lambda: qmp.call("stop")),
        ("electrical", qmp.snapshot),
        ("peers", qmp.peers),
        ("cpus", lambda: qmp.call("query-cpus-fast")),
        ("registers_all", lambda: qmp.call("human-monitor-command", {"command-line": "info registers -a"})),
    )
    for name, operation in operations:
        try:
            diagnostics[name] = operation()
        except Exception as exc:
            diagnostics[name] = dict(error=f"{type(exc).__name__}: {exc}")
    return diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qemu", required=True, type=pathlib.Path)
    parser.add_argument("--mode", required=True, choices=("connected", "disconnected"))
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--flash", type=pathlib.Path)
    parser.add_argument("--sdkconfig", type=pathlib.Path)
    parser.add_argument("--watchdog-seconds", type=float, default=90)
    args = parser.parse_args()
    if not 1 <= args.watchdog_seconds <= 3600:
        parser.error("--watchdog-seconds must be 1..3600 (host diagnostic only)")
    if not args.build and (args.flash is None or args.sdkconfig is None):
        parser.error("Use --build or provide both --flash and --sdkconfig")
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f"{args.mode}-{time.strftime('%Y%m%dT%H%M%S')}-", dir=args.evidence.resolve()))
    result = dict(status="FAIL", mode=args.mode, evidence=str(evidence), expected_frozen_combined_prefix=PREFIX,
                  qmp_transcript=[], snapshots=[], qualification="ordinary IDF closed-pulse/RX/IRQ assertions",
                  fixture_wiring=FIXTURE_WIRING,
                  native_clock=dict(mode="icount", shift=0, align=False, sleep=False,
                                    tcg_thread="single", qualification="deterministic functional time, not CPU cycle accuracy"),
                  limitations=["Idle terminal half-duration is printed verbatim; TRM does not specify its encoding",
                               "RC_FAST calibration, RX demodulation and finite TX loop counts not exercised by this fixture",
                               "Sensor-device protocol unavailable: no named native sensor model in the assigned graph registry",
                               "NEC/WS2812 content is a software pulse reference, not physical metrology"])
    proc = qmp = live = None
    sources = [pathlib.Path(__file__).resolve()]
    started = time.monotonic()
    try:
        live = pathlib.Path(tempfile.mkdtemp(prefix="rmt-native-", dir="/tmp"))
        filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(live)], text=True).strip()
        require(filesystem in ("ext2/ext3", "ext4", "btrfs", "xfs", "overlayfs", "tmpfs"),
                f"Live transport must be native Linux disk, not {filesystem}")
        result["transport"] = dict(live_root=str(live), durable_root=str(evidence), filesystem=filesystem,
                                   policy="Copy closed artifacts after QEMU exits; no live DrvFS captures")
        uart, stderr, stdout, trace = (live / name for name in ("uart.log", "stderr.log", "stdout.log", "model-trace.log"))
        if args.build:
            args.flash, args.sdkconfig = build(args, live)
        args.qemu = args.qemu.resolve(strict=True)
        args.flash = args.flash.resolve(strict=True)
        args.sdkconfig = args.sdkconfig.resolve(strict=True)
        configuration(args.sdkconfig, args.mode)
        pin_path = args.flash.parent / "build-pin.json"
        pin = json.loads(pin_path.read_text())
        require(pin["idf_commit"] == IDF_COMMIT, "Firmware build pin is not IDF6.1")
        require(pin["flash_sha256"] == sha256(args.flash) and pin["sdkconfig_sha256"] == sha256(args.sdkconfig), "Firmware/config hash differs from build pin")
        require(pin["sdk_metadata"]["profile"] == "idf-6.1" and pin["sdk_metadata"]["commit"] == IDF_COMMIT,
                "Firmware lacks canonical native SDK provenance")
        require(pin["source_manifest"]["mode"] == args.mode, "Native source snapshot profile differs from graph")
        result["firmware_pin"] = pin
        sources.extend((args.qemu, args.flash, args.sdkconfig, pin_path))
        flash = live / "flash.bin"
        shutil.copyfile(args.flash, flash)
        result["transport"]["flash_initial_sha256"] = sha256(flash)
        require(result["transport"]["flash_initial_sha256"] == pin["flash_sha256"],
                "Per-session writable flash differs from the pinned input")
        graph = project(args.mode)
        (live / "project.json").write_text(json.dumps(graph, indent=2) + "\n")
        qmp_path = live / "qmp.sock"
        command = [str(args.qemu), "-machine", "esp32s3", "-nographic", "-S", "-monitor", "none",
                   "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
                   "-serial", f"file:{uart}", "-drive", f"file={flash},if=mtd,format=raw,snapshot=off",
                   "-qmp", f"unix:{qmp_path},server=on,wait=off",
                   "-trace", f"enable=esp32s3_rmt_*,file={trace}"]
        result["command"] = command
        (live / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        deadline = time.monotonic() + args.watchdog_seconds
        with stdout.open("wb") as output, stderr.open("wb") as errors:
            proc = subprocess.Popen(command, cwd=live, env=dict(os.environ, TMPDIR=str(live)),
                                    stdout=output, stderr=errors)
            while not qmp_path.exists():
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited {proc.returncode} before QMP")
                if time.monotonic() >= deadline:
                    raise TimeoutError("QMP startup watchdog expired")
                time.sleep(.05)
            qmp = Qmp(qmp_path, result["qmp_transcript"], deadline)
            qmp.call("stop")
            qmp.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(graph)))
            result["snapshots"].append(dict(phase="graph-applied-before-boot", graph=qmp.snapshot(), peers=qmp.peers()))
            qmp.call("cont")
            nec_powered = False
            while True:
                text = uart.read_text(errors="replace") if uart.exists() else ""
                if not nec_powered and re.search(r"^RMT_NATIVE_EXTERNAL_READY rx_gpio=8 armed_us=\d+\n", text, re.M):
                    qmp.call("stop")
                    powered_graph = project(args.mode, nec_powered=True)
                    (live / "project-powered.json").write_text(json.dumps(powered_graph, indent=2) + "\n")
                    qmp.call("qom-set", dict(path="/machine/soc/electrical", property="project-json",
                                           value=json.dumps(powered_graph)))
                    result["snapshots"].append(dict(phase="external-nec-powered-after-rx-armed",
                                                    graph=qmp.snapshot(), peers=qmp.peers()))
                    nec_powered = True
                    qmp.call("cont")
                if re.search(r"^RMT_NATIVE_DONE [^\r\n]*\n", text, re.M):
                    break
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited {proc.returncode} before completion")
                if time.monotonic() >= deadline:
                    raise TimeoutError("Native firmware did not complete before host watchdog; no result fabricated")
                time.sleep(.05)
            qmp.call("stop")
            require(nec_powered, "External NEC was never physically powered after RX arm")
            result["snapshots"].append(dict(phase="firmware-completed", graph=qmp.snapshot(), peers=qmp.peers()))
            result["uart_assertions"] = validate_uart(text, args.mode)
            result["pre_cleanup_diagnostics"] = capture_pre_cleanup_state(qmp, proc)
            (live / "pre-cleanup-diagnostics.json").write_text(json.dumps(result["pre_cleanup_diagnostics"], indent=2) + "\n")
            result["status"] = "PASS"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        result["failure_diagnostics"] = capture_pre_cleanup_state(qmp, proc)
        if live is not None:
            (live / "failure-diagnostics.json").write_text(json.dumps(result["failure_diagnostics"], indent=2) + "\n")
    finally:
        if qmp is not None:
            try:
                qmp.close()
            except OSError as exc:
                result["status"] = "FAIL"
                result.setdefault("error", f"QMP cleanup: {exc}")
        if proc is not None:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            result["qemu_returncode_after_cleanup"] = proc.returncode
        # Every child capture is closed now. Read only the native files, never
        # attempt a live DrvFS read or substitute cached/transient bytes.
        if live is not None:
            try:
                trace_text = (live / "model-trace.log").read_text()
                result["model_assertions"] = validate_model_trace(trace_text, args.mode)
                result["peer_assertions"] = validate_peer_capture(result["snapshots"], result["model_assertions"],
                                                                 trace_text, args.mode)
            except Exception as exc:
                result["status"] = "FAIL"
                result["model_error"] = f"{type(exc).__name__}: {exc}"
                result.setdefault("error", result["model_error"])
            uart_path = live / "uart.log"
            text = uart_path.read_text(errors="replace") if uart_path.exists() else ""
            result["uart_lines"] = re.findall(r"^RMT_NATIVE_.*$", text, re.M)
        result["host_elapsed_seconds"] = time.monotonic() - started
        result["host_watchdog_seconds"] = args.watchdog_seconds
        copy_complete = True
        result["artifact_mapping"] = {}
        if live is not None:
            try:
                for path in sorted(live.iterdir()):
                    if path.is_file():
                        destination = evidence / path.name
                        shutil.copyfile(path, destination)
                        result["artifact_mapping"][str(path)] = str(destination)
                        sources.append(destination)
            except Exception as exc:
                copy_complete = False
                result["status"] = "FAIL"
                result["artifact_copy_error"] = f"{type(exc).__name__}: {exc}"
                result.setdefault("error", result["artifact_copy_error"])
                result["retained_live_directory"] = str(live)
        result["closed_artifacts_copied"] = copy_complete
        result["hashes"] = {}
        try:
            for path in sources:
                result["hashes"][str(path)] = sha256(path)
        except Exception as exc:
            copy_complete = False
            result["status"] = "FAIL"
            result["artifact_hash_error"] = f"{type(exc).__name__}: {exc}"
            result.setdefault("error", result["artifact_hash_error"])
            if live is not None:
                result["retained_live_directory"] = str(live)
        if live is not None:
            (live / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        try:
            if live is not None:
                shutil.copyfile(live / "result.json", evidence / "result.json")
            else:
                (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        except Exception as exc:
            copy_complete = False
            result["status"] = "FAIL"
            result["closed_artifacts_copied"] = False
            result["artifact_copy_error"] = f"{type(exc).__name__}: {exc}"
            result.setdefault("error", result["artifact_copy_error"])
            if live is not None:
                result["retained_live_directory"] = str(live)
                (live / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if live is not None and copy_complete:
            shutil.rmtree(live)
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), error=result.get("error"),
                          retained_live_directory=result.get("retained_live_directory"), limitations=result["limitations"])))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
