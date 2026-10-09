#!/usr/bin/env python3
"""Freeze ordinary pinned IDF6.1 or Arduino3.3.12/IDF5.5.5 boot artifacts.

The sole live circuit mutation releases GPIO10 through a real resistor to VDD
while UART1 has 513 queued bytes and physical CTS is held high by GPIO13.
No guest registers, internal loopback, UART console RX, or echo devices are used.
Run under WSL/Linux: QMP uses a short-path UNIX socket.
"""
import argparse
import errno
import hashlib
import json
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
ARDUINO_IDF_COMMIT = "b774170ff46c393eeb5e495ea37936038d3f4f4f"
ARDUINO_COMMIT = "94afccf35fb1e401facddbcf9e13bcf7c76a31d8"
BASE_COMMIT = "40edccac415693c5130f91c01d84176ae6008566"
MODES = ("connected", "absent", "disconnected", "wrong", "uhci", "arduino")
# Native filesystem for artifacts a running QEMU is still writing.
LIVE_ROOT = pathlib.Path.home() / ".cache/esp32s3vm"
TRANSFERS = {
    "duplex9600": (9600, 8, 1), "duplex115200": (115200, 8, 7),
    "duplex921600": (921600, 8, 13), "width5_even_stop1": (115200, 5, 17),
    "width6_odd_stop15": (115200, 6, 19), "width7_even_stop2": (115200, 7, 23),
    "width8_odd_stop2": (115200, 8, 31), "normal_restore": (115200, 8, 37),
    "routing_reconnect": (115200, 8, 41), "driver_reset_reconnect": (115200, 8, 47),
    "after_error_recovery": (115200, 8, 73),
}
UHCI_TRANSFERS = {
    "idle_full_duplex513_concurrent_uart0": (5, 0, 0),
    "idle_multibuffer_reset513": (13, 0, 1),
    "length_eof_reset513": (23, 1, 0),
    "break_eof_reset513": (37, 2, 0),
}
ARDUINO_TRANSFERS = {
    "duplex9600": (9600, 1, 255, 8), "duplex115200": (115200, 7, 255, 8),
    "duplex921600": (921600, 13, 255, 8), "width7_even_stop2": (115200, 23, 127, 7),
    "width8_odd_stop2": (115200, 31, 255, 8),
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def configuration(path, mode):
    values = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", path.read_text(), re.MULTILINE))
    selected = [key.removeprefix("CONFIG_UART_NATIVE_PROFILE_").lower()
                for key, value in values.items()
                if key.startswith("CONFIG_UART_NATIVE_PROFILE_") and value == "y"]
    require(selected == [mode], f"Graph mode {mode} differs from sdkconfig selection {selected}")
    require(values.get("CONFIG_IDF_TARGET") == '"esp32s3"', "Fixture target must be esp32s3")
    require(values.get("CONFIG_ESP_CONSOLE_UART_DEFAULT") == "y", "Console must remain default UART0")
    require(values.get("CONFIG_FREERTOS_HZ") == "1000", "Fixture tick rate must be 1000Hz")
    if mode == "arduino":
        require(values.get("CONFIG_AUTOSTART_ARDUINO") == "y" and values.get("CONFIG_ARDUINO_VARIANT") == '"esp32s3"',
                "Arduino image requires ordinary autostart setup/loop and esp32s3 variant")


def freeze(args):
    args.output.mkdir(parents=True, exist_ok=False)
    actual = subprocess.check_output(["git", "-C", str(args.idf), "rev-parse", "HEAD"], text=True).strip()
    expected_idf = ARDUINO_IDF_COMMIT if args.mode == "arduino" else IDF_COMMIT
    require(actual == expected_idf, f"IDF revision {actual} differs from locked profile {expected_idf}")
    sdk = json.loads(args.sdk_manifest.read_text())
    expected_profile = "idf-5.5.5" if args.mode == "arduino" else "idf-6.1"
    require(sdk["profile"] == expected_profile and sdk["commit"] == actual
            and pathlib.Path(sdk["source"]).resolve() == args.idf.resolve(),
            "Canonical SDK manifest differs from selected profile/source")
    source_manifest = json.loads(args.source_manifest.read_text())
    snapshot_root = args.source_manifest.resolve().parent
    identity = source_manifest["identity"]
    require(identity["mode"] == args.mode and identity["sdk_profile"] == expected_profile
            and identity["sdk_commit"] == actual
            and source_manifest["sdk_manifest_sha256"] == digest(args.sdk_manifest),
            "Native firmware source snapshot differs from canonical SDK/profile identity")
    for relative, sha in source_manifest["files"].items():
        require(digest(snapshot_root / relative) == sha, f"Actual compiled source snapshot changed: {relative}")
    require(all(path.resolve().parent == snapshot_root / "build" for path in (args.flash, args.elf, args.sdkconfig)),
            "Boot artifacts must belong to the declared native source snapshot build directory")
    arduino_revision = None
    if args.mode == "arduino":
        require(args.arduino_source is not None, "Arduino freeze requires --arduino-source")
        arduino_revision = subprocess.check_output(
            ["git", "-C", str(args.arduino_source), "rev-parse", "HEAD"], text=True).strip()
        require(arduino_revision == ARDUINO_COMMIT, "Arduino source revision differs from pinned3.3.12")
    configuration(args.sdkconfig, args.mode)
    require(args.flash.stat().st_size == 4 * 1024 * 1024, "Freeze requires full ordinary merged 4MiB boot flash")
    files = {}
    for key, source in (("flash.bin", args.flash), ("firmware.elf", args.elf),
                        ("sdkconfig", args.sdkconfig), ("runtime-source.json", args.runtime_source),
                        ("sdk-manifest.json", args.sdk_manifest), ("source-manifest.json", args.source_manifest)):
        shutil.copyfile(source, args.output / key)
        files[key] = digest(args.output / key)
    for relative, sha in source_manifest["files"].items():
        key = f"compiled/{relative}"
        destination = args.output / key
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(snapshot_root / relative, destination)
        require(digest(destination) == sha, f"Compiled source changed while freezing: {relative}")
        files[key] = sha
    if args.mode == "arduino":
        component = args.arduino_source.resolve()
        source_tree = {str(path.relative_to(component)): digest(path)
                       for path in sorted(component.rglob("*"))
                       if path.is_file() and ".git" not in path.relative_to(component).parts}
        dump(args.output / "arduino-source-tree.json",
             dict(commit=arduino_revision, source=str(component), files=source_tree))
        files["arduino-source-tree.json"] = digest(args.output / "arduino-source-tree.json")
    source_paths = [pathlib.Path(__file__).resolve(), ROOT / "runtime-lock.json"]
    source_paths.extend(sorted((ROOT / "tests/firmware/uart_native").glob("sdkconfig*.defaults")))
    source_paths.extend([ROOT / "tests/firmware/uart_native/CMakeLists.txt",
                         ROOT / "tests/firmware/uart_native/main/CMakeLists.txt",
                         ROOT / "tests/firmware/uart_native/main/Kconfig.projbuild",
                         ROOT / "tests/firmware/uart_native/main/uart_native.c",
                         ROOT / "tests/firmware/uart_native/main/uart_uhci_native.c"])
    source_paths.extend(ROOT / f"qemu-extensions/prototypes/uart/firmware/project-{mode}.json" for mode in MODES)
    source_paths.append(ROOT / "qemu-extensions/prototypes/uart/firmware/project-connected-resume.json")
    source_paths.extend([ROOT / "qemu-extensions/prototypes/uart/firmware/build-idf-fixture.sh",
                         ROOT / "tests/firmware/uart_native/README.md"])
    source_paths.append(ROOT / "qemu-extensions/prototypes/uart/run-rom-download.py")
    source_paths.append(ROOT / "qemu-extensions/prototypes/uart/firmware/prepare-fixture-source.py")
    source_paths.extend([ROOT / "tests/firmware/uart_native/arduino/CMakeLists.txt",
                         ROOT / "tests/firmware/uart_native/arduino/sdkconfig.defaults",
                         ROOT / "tests/firmware/uart_native/arduino/main/CMakeLists.txt",
                         ROOT / "tests/firmware/uart_native/arduino/main/Kconfig.projbuild",
                         ROOT / "tests/firmware/uart_native/arduino/main/uart_native_arduino.cpp"])
    for source in source_paths:
        key = str(pathlib.Path("sources") / source.relative_to(ROOT))
        destination = args.output / key
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        files[key] = digest(destination)
    graph_key = f"sources/qemu-extensions/prototypes/uart/firmware/project-{args.mode}.json"
    dump(args.output / "freeze.json", dict(schema_version=1, mode=args.mode, idf_commit=actual,
         base_qemu_commit=BASE_COMMIT, qemu_path=str(args.qemu.resolve()),
         arduino_commit=arduino_revision,
         sdk_profile=expected_profile, firmware_source_sha256=source_manifest["source_sha256"],
         qemu_sha256=digest(args.qemu), project=graph_key,
         resumed_project="sources/qemu-extensions/prototypes/uart/firmware/project-connected-resume.json",
         files=files, qualification="Frozen inputs only; no build/runtime success implied"))
    print(json.dumps(dict(status="FROZEN", manifest=str(args.output / "freeze.json"))))
    return 0


class Qmp:
    def __init__(self, path, transcript, deadline):
        self.deadline, self.transcript, self.sequence = deadline, transcript, 0
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)
        require("QMP" in self.receive(), "QMP greeting missing")
        self.call("qmp_capabilities")

    def receive(self):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, "Host diagnostic watchdog expired during QMP")
        self.sock.settimeout(min(10, remaining))
        line = self.stream.readline()
        require(bool(line), "QMP transport closed")
        value = json.loads(line)
        self.transcript.append(dict(direction="receive", message=value))
        return value

    def call(self, command, arguments=None):
        self.sequence += 1
        value = dict(execute=command, arguments=arguments or {}, id=self.sequence)
        self.transcript.append(dict(direction="send", message=value))
        self.stream.write((json.dumps(value) + "\n").encode())
        while True:
            response = self.receive()
            if "event" in response:
                continue
            require(response.get("id") == self.sequence, "Unmatched QMP response")
            require("error" not in response, f"QMP {command} failed: {response}")
            return response["return"]

    def apply(self, graph):
        self.call("qom-set", dict(path="/machine/soc/electrical", property="project-json", value=json.dumps(graph)))

    def snapshot(self):
        return json.loads(self.call("qom-get", dict(path="/machine/soc/electrical", property="snapshot-json")))

    def close(self):
        self.stream.close()
        self.sock.close()


def validate_arduino(text):
    # esp_get_idf_version() reports git describe, which is the short commit
    # for our detached canonical checkout, not necessarily a release tag.
    # Freeze/run still require the full SDK commit and source-tree manifest.
    sdk_description = rf"(?:v?5\.5\.5(?:[-+].*)?|{ARDUINO_IDF_COMMIT[:8]})"
    require(re.search(rf"^ARDUINO_UART_BOOT profile=arduino arduino=3\.3\.12 idf={sdk_description}$",
                      text, re.MULTILINE), "Pinned Arduino3.3.12/IDF5.5.5 boot identity missing")
    require(re.search(r"^ARDUINO_UART_DONE profile=arduino failures=0 result=PASS$", text, re.MULTILINE),
            "Ordinary HardwareSerial fixture did not complete with zero failures")
    require(not re.search(r"^ARDUINO_UART_CHECK .*result=FAIL$", text, re.MULTILINE), "HardwareSerial vector failed")
    checks = set(re.findall(r"^ARDUINO_UART_CHECK name=(\w+) result=PASS$", text, re.MULTILINE))
    expected = set(ARDUINO_TRANSFERS) | {"begin1", "begin2", "clock1", "clock2", "rx_buffer1", "rx_buffer2",
                "tx_buffer1", "tx_buffer2", "end1", "end2", "uart0_console_restore",
                "uart0_loop9600", "uart0_loop115200", "uart0_loop921600"}
    require(expected <= checks, f"Missing HardwareSerial vectors: {sorted(expected - checks)}")
    for name, (baud, salt, mask, bits) in ARDUINO_TRANSFERS.items():
        match = re.search(rf"^ARDUINO_UART_TRANSFER name={name} baud={baud} salt={salt} mask={mask} "
                          r"n1=513 n2=513 elapsed_us=(\d+) actual_baud1=(\d+) actual_baud2=(\d+)\n"
                          rf"ARDUINO_UART_CHECK name={name} result=PASS\n"
                          r"ARDUINO_UART_BYTES name=rx1 size=513 data=([0-9a-f]+)\n"
                          r"ARDUINO_UART_BYTES name=rx2 size=513 data=([0-9a-f]+)$", text, re.MULTILINE)
        require(match is not None, f"Missing actual HardwareSerial full-duplex evidence: {name}")
        incoming1 = bytes((i * 53 + salt + 91) & mask for i in range(513))
        incoming2 = bytes((i * 37 + salt + 3) & mask for i in range(513))
        require(match[4] == incoming1.hex() and match[5] == incoming2.hex(), f"HardwareSerial real peer bytes differ: {name}")
        require(int(match[1]) >= 512 * (bits + 2) * 1000000 // baud, f"HardwareSerial {name} lacks physical frame duration")
        require(all(baud * .98 <= int(match[index]) <= baud * 1.02 for index in (2, 3)),
                f"HardwareSerial {name} actual baud differs")
    for index, baud in enumerate((9600, 115200, 921600)):
        name = f"uart0_loop{baud}"
        match = re.search(rf"^ARDUINO_UART_LOOPBACK name={name} baud={baud} salt={index * 17} "
                          r"sent=513 received=513 elapsed_us=(\d+) actual_baud=(\d+)\n"
                          rf"ARDUINO_UART_CHECK name={name} result=PASS\n"
                          r"ARDUINO_UART_BYTES name=uart0_rx size=513 data=([0-9a-f]+)$", text, re.MULTILINE)
        require(match is not None, f"Missing actual UART0 HardwareSerial loopback evidence: {name}")
        expected_bytes = bytes((i * 61 + index * 17 + 43) & 255 for i in range(513))
        require(match[3] == expected_bytes.hex(), f"UART0 actual physical bytes differ: {name}")
        require(int(match[1]) >= 512 * 10 * 1000000 // baud and baud * .98 <= int(match[2]) <= baud * 1.02,
                f"UART0 HardwareSerial {name} physical duration or actual baud differs")
    require(f"ARDUINO_UART_PROFILE arduino_commit={ARDUINO_COMMIT} idf_commit={ARDUINO_IDF_COMMIT} idf61_qualification=not_claimed" in text,
            "Arduino fixture failed to distinguish its pinned IDF5.5.5 qualification")
    require("ARDUINO_UART_SCOPE external_uart0_tx=4 external_uart0_rx=5 console_tx=43 console_rx=44 console_rx_injection=never" in text,
            "Arduino physical UART0 routing/console separation evidence missing")


def validate_uhci(text):
    require(re.search(r"^UHCI_NATIVE_DONE failures=0 result=PASS$", text, re.MULTILINE),
            "Ordinary UHCI driver did not complete with zero failures")
    require(not re.search(r"^UHCI_NATIVE_CHECK .*result=FAIL$", text, re.MULTILINE), "UHCI vector failed")
    require("UHCI_NATIVE_BOOT controller=0 selected_uart=1 tx=17 rx=15 peer_uart=2 peer_tx=18 peer_rx=16" in text,
            "UHCI controller selection or physical peer pins missing")
    checks = set(re.findall(r"^UHCI_NATIVE_CHECK name=(\w+) result=PASS$", text, re.MULTILINE))
    expected = set(UHCI_TRANSFERS) | {"new_controller", "callbacks", "receive_arm", "transmit",
                "multi_buffer_transmit", "real_partial_dma_callbacks", "controller_delete_reset",
                "peer_driver_install", "peer_config", "peer_pins", "peer_driver_delete",
                "selected_uart_no_competing_driver", "concurrent_uart0_before_physical_tx_end"}
    require(expected <= checks, f"Missing ordinary UHCI vectors: {sorted(expected - checks)}")
    concurrent = re.search(r"^UHCI_NATIVE_CONCURRENT uart0_probe_begin_us=(\d+) uart0_probe_end_us=(\d+)$",
                           text, re.MULTILINE)
    require(concurrent is not None and int(concurrent[1]) < 512 * 10 * 1000000 // 115200
            and int(concurrent[2]) > int(concurrent[1]), "UART0 ordinary physical probe did not overlap queued UHCI wire transfer")
    for name, (salt, eof, multi) in UHCI_TRANSFERS.items():
        match = re.search(rf"^UHCI_NATIVE_TRANSFER name={name} salt={salt} eof_mode={eof} multi={multi} "
                          r"rx=513 peer_rx=513 tx_callback=513 partial=(\d+) elapsed_us=(\d+)\n"
                          rf"UHCI_NATIVE_CHECK name={name} result=PASS\n"
                          r"UHCI_NATIVE_CHECK name=real_partial_dma_callbacks result=PASS\n"
                          r"UHCI_NATIVE_BYTES name=dma_rx size=513 data=([0-9a-f]+)\n"
                          r"UHCI_NATIVE_BYTES name=peer_rx size=513 data=([0-9a-f]+)$", text, re.MULTILINE)
        require(match is not None, f"Missing actual ordinary UHCI byte evidence: {name}")
        peer = bytes((i * 29 + salt + 11) & 255 for i in range(513))
        incoming = bytes(((i * 47 + salt + 101) & 255) or (0x80 if eof == 2 else 0) for i in range(513))
        require(match[3] == incoming.hex() and match[4] == peer.hex(), f"UHCI physical full-duplex bytes differ: {name}")
        require(int(match[1]) > 0 and int(match[2]) >= 512 * 10 * 1000000 // 115200,
                f"UHCI {name} lacks descriptor partial events or physical UART framing duration")
    require("UHCI_NATIVE_UNAVAILABLE ordinary_api=packet_separator_configuration,escape_sequence_configuration "
            "public_driver=driver/uhci.h HAL=init_deinit_only" in text,
            "Missing exact ordinary UHCI separator/escape API availability report")


def validate(text, mode, host_stop):
    text = text.replace("\r", "")
    if mode == "arduino":
        validate_arduino(text)
        return
    require(re.search(rf"^UART_NATIVE_BOOT profile={mode} idf=6\.1$", text, re.MULTILINE), "Boot profile missing or mismatched")
    require(re.search(rf"^UART_NATIVE_DONE profile={mode} failures=0 result=PASS$", text, re.MULTILINE), "Ordinary fixture did not report zero-failure PASS")
    require(not re.search(r"^UART_NATIVE_CHECK .*result=FAIL$", text, re.MULTILINE), "Firmware reported a failed vector")
    checks = set(re.findall(r"^UART_NATIVE_CHECK name=(\w+) result=PASS$", text, re.MULTILINE))
    expected = {"uart0_install", "uart0_config", "uart0_physical_loopback513", "uart0_console_restore"}
    if mode != "uhci":
        expected |= {"driver_install", "param_config", "set_pin", "delete1"}
    if mode == "uhci":
        validate_uhci(text)
    elif mode == "connected":
        expected |= set(TRANSFERS) | {"threshold_irq", "threshold_tail_exact", "timeout_irq", "timeout_exact",
                    "disconnected_no_receive", "cts_tx_stalled", "cts_rx_stalled", "cts_resume_exact",
                    "host_stop_resume_physical_gate", "parity_error_irq", "width_frame_error_irq",
                    "stop_mismatch_observed", "break_irq", "fifo_overflow_irq", "ring_buffer_full_irq",
                    "at_irq", "at_position", "at_bytes", "at_guard_reject", "at_guard_accept", "at_gap_reject",
                    "autobaud_actual_edges", "rs485_half_receive", "rs485_app_receive", "rs485_no_collision",
                    "rs485_collision_observed", "delete2"}
        expected |= {"auto_rts_tx_stalled", "auto_rts_resume_exact"}
        expected |= {"irda_receive_mode", "irda_gpio_physical_pulses", "irda_receive_physical",
                     "irda_uart1_tx_route_restore", "irda_normal_restore"}
        require(re.search(r"^UART_NATIVE_BYTES name=irda_rx size=4 data=0055aaff$", text, re.MULTILINE),
                "Missing actual receive-only IrDA GPIO pulse byte evidence")
        require(host_stop, "Runner never performed QMP stop/circuit-gate release/resume")
        for name, (baud, bits, salt) in TRANSFERS.items():
            match = re.search(rf"^UART_NATIVE_TRANSFER name={name} baud={baud} bits={bits} parity=\d+ stops=\d+ n1=513 n2=513 elapsed_us=(\d+)\n"
                              r"UART_NATIVE_CHECK name=\w+ result=PASS\n"
                              r"UART_NATIVE_BYTES name=rx1 size=513 data=([0-9a-f]+)\n"
                              r"UART_NATIVE_BYTES name=rx2 size=513 data=([0-9a-f]+)$", text, re.MULTILINE)
            require(match is not None, f"Missing full duplex byte evidence for {name}")
            mask = (1 << bits) - 1
            left = bytes((i * 53 + salt + 91) & mask for i in range(513))
            right = bytes((i * 37 + salt + 3) & mask for i in range(513))
            require(match[2] == left.hex() and match[3] == right.hex(), f"Actual peer bytes differ for {name}")
            # At least start+data+one stop per byte, with one-symbol allowance
            # for the two enqueue calls. No instant byte-delivery surrogate.
            require(int(match[1]) >= (512 * (bits + 2) * 1000000 // baud), f"Transfer {name} completed faster than physical framing")
    else:
        expected |= {"negative_write1", "negative_done1", "negative_no_rx1"}
        expected |= ({"absent_peer_not_installed"} if mode == "absent" else
                     {"negative_no_rx2", "negative_write2", "negative_done2", "negative_no_reverse_rx1", "delete2"})
    require(expected <= checks, f"Missing passing vectors: {sorted(expected - checks)}")
    require("UART_NATIVE_UNAVAILABLE ordinary_api=irda_tx_direction,irda_wctl_control "
            "rs485_autobaud_AT=available irda_receive=available" in text,
            "Missing exact ordinary API availability report")


TRANSIENT_ERRNOS = frozenset((errno.ENODATA, errno.EAGAIN, errno.EBUSY, errno.ETXTBSY))


def read_guest_log(path, offset=0, deadline=None):
    """Read the guest log while QEMU appends to it.

    The evidence directory may sit on a drvfs mount, where a read racing the
    guest-side append can fail with ENODATA instead of returning a prefix. That
    is a host filesystem artifact, so retry briefly and let the watchdog own the
    real bound. The live file is kept on native disk (see LIVE_ROOT).
    """
    for attempt in range(200):   # ~10 s bound; callers pass their own deadline
        try:
            return path.read_text(errors="replace").replace("\r", "")[offset:] if path.exists() else ""
        except OSError as exc:
            if exc.errno not in TRANSIENT_ERRNOS or                     (deadline is not None and time.monotonic() >= deadline):
                raise
            time.sleep(.05)


def run(args):
    manifest_path = args.frozen / "freeze.json"
    frozen = json.loads(manifest_path.read_text())
    expected_idf = ARDUINO_IDF_COMMIT if frozen["mode"] == "arduino" else IDF_COMMIT
    require(frozen["idf_commit"] == expected_idf and frozen["base_qemu_commit"] == BASE_COMMIT, "Frozen lock mismatch")
    if frozen["mode"] == "arduino":
        require(frozen.get("arduino_commit") == ARDUINO_COMMIT, "Frozen Arduino source lock mismatch")
    for name, sha in frozen["files"].items():
        require(digest(args.frozen / name) == sha, f"Frozen artifact changed: {name}")
    require(digest(args.qemu) == frozen["qemu_sha256"], "QEMU differs from frozen native executable")
    script_key = "sources/qemu-extensions/prototypes/uart/run-native-fixture.py"
    require(digest(pathlib.Path(__file__).resolve()) == frozen["files"][script_key],
            "Ordinary fixture runner differs from its frozen source")
    mode = frozen["mode"]
    configuration(args.frozen / "sdkconfig", mode)
    graph = json.loads((args.frozen / frozen["project"]).read_text())
    resumed = json.loads((args.frozen / frozen["resumed_project"]).read_text())
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f"{mode}-", dir=args.evidence.resolve()))
    # QEMU writes the logs while the run is in flight and the evidence directory
    # may be on drvfs, whose concurrent-write coherency is unusable inside a
    # polling loop. Keep the live files on native disk and publish byte copies
    # into the evidence directory when the session closes.
    LIVE_ROOT.mkdir(parents=True, exist_ok=True)
    live = pathlib.Path(tempfile.mkdtemp(prefix=f"{mode}-live-", dir=LIVE_ROOT))
    uart, stderr, stdout = (live / key for key in ("uart.log", "stderr.log", "stdout.log"))
    result = dict(status="FAIL", mode=mode, frozen_manifest_sha256=digest(manifest_path),
                  qmp=[], snapshots=[], host_stop=False, host_watchdog_seconds=args.watchdog_seconds)
    process = qmp = None
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="uart-qmp-") as transport:
            path = pathlib.Path(transport) / "qmp.sock"
            command = [str(args.qemu.resolve()), "-machine", "esp32s3", "-nographic", "-S", "-monitor", "none",
                       "-accel", "tcg,thread=single",
                       "-icount", "shift=0,align=off,sleep=off",
                       "-serial", f"file:{uart}", "-drive", f"file={(args.frozen / 'flash.bin').resolve()},if=mtd,format=raw,snapshot=on",
                       "-qmp", f"unix:{path},server=on,wait=off"]
            dump(evidence / "command.json", command)
            dump(evidence / "project.json", graph)
            deadline = started + args.watchdog_seconds
            with stdout.open("wb") as output, stderr.open("wb") as errors:
                process = subprocess.Popen(command, stdout=output, stderr=errors)
                while not path.exists():
                    require(process.poll() is None, "QEMU exited before QMP; see stderr.log")
                    require(time.monotonic() < deadline, "Host watchdog expired before QMP")
                    time.sleep(.01)
                qmp = Qmp(path, result["qmp"], deadline)
                qmp.apply(graph)
                result["snapshots"].append(dict(phase="before_boot", graph=qmp.snapshot()))
                qmp.call("cont")
                while True:
                    text = read_guest_log(uart)
                    if mode == "connected" and not result["host_stop"] and "UART_NATIVE_HOST_STOP_READY" in text:
                        qmp.call("stop")
                        result["snapshots"].append(dict(phase="cts_queued_vm_stopped", graph=qmp.snapshot()))
                        qmp.apply(resumed)
                        dump(evidence / "project-resumed.json", resumed)
                        result["snapshots"].append(dict(phase="physical_gate_released_vm_stopped", graph=qmp.snapshot()))
                        qmp.call("cont")
                        result["host_stop"] = True
                    # UHCI has an inner report, then more UART0 checks and
                    # the aggregate UART_NATIVE_DONE; wait for the latter.
                    completion = "ARDUINO_UART_DONE" if mode == "arduino" else "UART_NATIVE_DONE"
                    # Match only a complete line: the bare token followed by
                    # anything matches a partially flushed print and kills
                    # QEMU mid-line, so validation then never sees the result.
                    if re.search(rf"^{completion} (?:profile=\S+ )?failures=\d+ result=(?:PASS|FAIL)$",
                                 text, re.MULTILINE):
                        break
                    require(process.poll() is None, "QEMU exited before ordinary firmware completion; see stderr.log")
                    require(time.monotonic() < deadline, "Host watchdog expired: firmware outcome remains unqualified")
                    time.sleep(.01)
                qmp.call("stop")
                result["snapshots"].append(dict(phase="completed", graph=qmp.snapshot()))
                validate(text, mode, result["host_stop"])
                result["status"] = "PASS"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if qmp is not None:
            qmp.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for name in ("uart.log", "stderr.log", "stdout.log"):
            source = live / name
            target = evidence / name
            if source.exists() and not target.exists():
                target.write_bytes(source.read_bytes())
        result["elapsed_seconds"] = time.monotonic() - started
        result["evidence_sha256"] = {path.name: digest(path) for path in evidence.iterdir() if path.is_file()}
        dump(evidence / "result.json", result)
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), error=result.get("error"))))
    return 0 if result["status"] == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    freezing = sub.add_parser("freeze", help="Freeze built boot image and inputs before any launch")
    for name in ("qemu", "flash", "elf", "sdkconfig", "idf", "runtime-source", "sdk-manifest", "source-manifest", "output"):
        freezing.add_argument(f"--{name}", required=True, type=pathlib.Path)
    freezing.add_argument("--mode", required=True, choices=MODES)
    freezing.add_argument("--arduino-source", type=pathlib.Path,
                          help="Existing pinned Arduino component; required only for --mode arduino")
    running = sub.add_parser("run", help="Launch frozen image and physical v3 graph only")
    for name in ("qemu", "frozen", "evidence"):
        running.add_argument(f"--{name}", required=True, type=pathlib.Path)
    running.add_argument("--watchdog-seconds", type=float, default=900,
                         help="Host diagnostic deadline; electrical bit-by-bit qualification is much slower than guest time")
    args = parser.parse_args()
    if args.action == "run" and not 1 <= args.watchdog_seconds <= 900:
        parser.error("--watchdog-seconds must be within 1..900")
    return freeze(args) if args.action == "freeze" else run(args)


if __name__ == "__main__":
    raise SystemExit(main())
