#!/usr/bin/env python3
"""Freeze, then exercise real ESP32-S3 ROM UART0 RAM-download commands in QEMU.

QEMU-only loopback TCP endpoints are created by this process. No hardware port,
flash command, register helper, RAM readback, stub execution, eFuse operation or
external electrical UART circuit is permitted. esptool5.4.0's ordinary ROM
protocol supplies SYNC, MEM_BEGIN, MEM_DATA and checked MEM_END(no-execute).
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
PINNED_ESPTOOL = "5.4.0"
PINNED_PYSERIAL = "3.5"
NATIVE_RUNNER = ROOT / "qemu-extensions/prototypes/uart/run-native-fixture.py"
BASE_COMMIT = "40edccac415693c5130f91c01d84176ae6008566"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def installed_tool():
    import esptool
    import serial
    from esptool.loader import StubFlasher
    from esptool.targets.esp32s3 import ESP32S3ROM
    require(esptool.__version__ == PINNED_ESPTOOL, "Select existing pinned esptool5.4.0 environment; no install/download is performed")
    require(serial.__version__ == PINNED_PYSERIAL, "Select existing pinned pyserial3.5 environment")
    package = pathlib.Path(esptool.__file__).resolve().parent
    sources = {str(path.relative_to(package)): digest(path) for path in sorted(package.rglob("*.py"))}
    return ESP32S3ROM, StubFlasher, package, sources


def freeze(args):
    boot_manifest = args.boot_frozen / "freeze.json"
    boot = json.loads(boot_manifest.read_text())
    require(boot["base_qemu_commit"] == BASE_COMMIT, "Ordinary boot image has a different QEMU base lock")
    for name, sha in boot["files"].items():
        require(digest(args.boot_frozen / name) == sha, f"Ordinary boot frozen artifact changed: {name}")
    rom, stub_metadata, package, sources = installed_tool()
    # The official installed stub's text segment provides a real, reproducible
    # inert payload and its documented IRAM load address. Never execute it.
    stub = stub_metadata(rom)
    require(len(stub.text) > 128, "Official installed S3 stub has no >FIFO text payload")
    require(any(start <= stub.text_start and stub.text_start + len(stub.text) <= end and role == "IRAM"
                for start, end, role in rom.MEMORY_MAP), "Official S3 stub text does not fit its declared IRAM map")
    args.output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.boot_frozen / "flash.bin", args.output / "flash.bin")
    shutil.copyfile(args.boot_frozen / "runtime-source.json", args.output / "runtime-source.json")
    shutil.copyfile(boot_manifest, args.output / "ordinary-boot-freeze.json")
    shutil.copyfile(pathlib.Path(__file__).resolve(), args.output / "run-rom-download.py")
    shutil.copyfile(NATIVE_RUNNER, args.output / "run-native-fixture.py")
    (args.output / "payload.bin").write_bytes(stub.text)
    dump(args.output / "payload.json", dict(kind="official-installed-esptool-S3-stub-text-inert",
         address=stub.text_start, official_entry=stub.entry, execute=False, size=len(stub.text),
         block_size=rom.ESP_RAM_BLOCK, payload_sha256=digest(args.output / "payload.bin")))
    # Capture the exact installed metadata/license used by the official loader.
    json_name = rom.STUB_CLASS.stub_json_name(rom)
    metadata_path = pathlib.Path(stub._get_json_path(json_name, rom.CHIP_NAME))
    shutil.copyfile(metadata_path, args.output / "official-stub.json")
    license_path = metadata_path.parent / "LICENSE-APACHE"
    if license_path.is_file():
        shutil.copyfile(license_path, args.output / "official-stub-LICENSE")
    files = {path.name: digest(path) for path in args.output.iterdir() if path.is_file()}
    dump(args.output / "freeze.json", dict(schema_version=1, base_qemu_commit=BASE_COMMIT,
         qemu_sha256=boot["qemu_sha256"], qemu_path=boot["qemu_path"],
         esptool_version=PINNED_ESPTOOL, pyserial_version=PINNED_PYSERIAL,
         esptool_source=str(package), esptool_source_sha256=sources,
         files=files, qualification="Frozen inputs only; ROM protocol outcome unqualified until run"))
    print(json.dumps(dict(status="FROZEN", manifest=str(args.output / "freeze.json"))))
    return 0


def qmp_class():
    spec = importlib.util.spec_from_file_location("uart_native_qmp", NATIVE_RUNNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.Qmp


def listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sock.listen(1)
    return sock


class Relay:
    """Record and forward only actual TCP UART0 bytes, never generate replies."""
    def __init__(self, qemu_socket, esptool_listener, evidence, deadline):
        self.qemu_socket, self.listener = qemu_socket, esptool_listener
        self.evidence, self.deadline = evidence, deadline
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.received = bytearray()
        self.banner_done = False
        self.banner_ready = threading.Event()
        self.events = []
        self.error = None
        self.thread = threading.Thread(target=self.forward, name="qemu-only-uart0-capture", daemon=True)
        self.thread.start()

    def forward(self):
        client = None
        try:
            self.listener.settimeout(max(.001, self.deadline - time.monotonic()))
            client, address = self.listener.accept()
            require(address[0] == "127.0.0.1", "Non-loopback esptool peer rejected")
            self.qemu_socket.settimeout(2)
            client.settimeout(2)
            with (self.evidence / "host-to-rom.bin").open("wb") as outbound, \
                 (self.evidence / "rom-to-host.bin").open("wb") as inbound:
                peers = (client, self.qemu_socket)
                while not self.stop.is_set() and time.monotonic() < self.deadline:
                    ready, _, _ = select.select(peers, [], [], .05)
                    for source in ready:
                        data = source.recv(65536)
                        if not data:
                            return
                        destination = self.qemu_socket if source is client else client
                        capture = outbound if source is client else inbound
                        offset = capture.tell()
                        capture.write(data)
                        capture.flush()
                        with self.lock:
                            self.events.append(dict(direction="host-to-rom" if source is client else "rom-to-host",
                                                    offset=offset, size=len(data), monotonic_ns=time.monotonic_ns()))
                            if source is self.qemu_socket:
                                self.received.extend(data)
                        # ROM banner bytes are captured as evidence but never
                        # forwarded: esptool's one-shot input flush races the
                        # relay's delivery latency, so stale banner bytes would
                        # otherwise surface as the first byte of the SYNC reply.
                        if source is self.qemu_socket and not self.banner_done:
                            marker = b"waiting for download\r\n"
                            idx = self.received.find(marker)
                            if idx < 0:
                                continue
                            # Forward only bytes past the marker's end; the
                            # completing chunk may carry the marker's tail.
                            self.banner_done = True
                            chunk_start = len(self.received) - len(data)
                            data = data[max(0, idx + len(marker) - chunk_start):]
                            self.banner_ready.set()
                            if not data:
                                continue
                        destination.sendall(data)
        except Exception as exc:
            if not self.stop.is_set():
                self.error = f"{type(exc).__name__}: {exc}"
        finally:
            if client is not None:
                client.close()

    def incoming(self):
        with self.lock:
            return bytes(self.received)

    def close(self):
        self.stop.set()
        self.thread.join(3)
        require(not self.thread.is_alive(), "UART0 capture relay did not stop")


def slip_frames(raw):
    """Strict evidence decoder; unknown escape/input is not made into an ACK."""
    frame = None
    escaped = False
    frames = []
    for byte in raw:
        if byte == 0xc0:
            require(not escaped, "Truncated SLIP escape in actual serial evidence")
            if frame:
                frames.append(bytes(frame))
            frame = bytearray()
        elif frame is not None:
            if escaped:
                require(byte in (0xdc, 0xdd), f"Unknown actual SLIP escape {byte:#x}")
                frame.append(0xc0 if byte == 0xdc else 0xdb)
                escaped = False
            elif byte == 0xdb:
                escaped = True
            else:
                frame.append(byte)
        else:
            raise RuntimeError(f"Non-SLIP byte {byte:#x} before actual protocol frame")
    require(not escaped and not frame, "Incomplete actual SLIP frame")
    return frames


def protocol_proof(evidence, payload, address, block_size, commands):
    sent = slip_frames((evidence / "host-to-rom.bin").read_bytes())
    incoming = (evidence / "rom-to-host.bin").read_bytes()
    marker = b"waiting for download\r\n"
    banner_end = incoming.find(marker)
    require(incoming.startswith(b"ESP-ROM:esp32s3-20210327\r\n") and banner_end >= 0,
            "Actual ESP32-S3 ROM download banner missing from raw evidence")
    # Only the complete startup banner is excluded; later panic text is not ACK.
    received = slip_frames(incoming[banner_end + len(marker):])
    requests = []
    responses = []
    for frame in sent:
        require(len(frame) >= 8, "Short actual ROM request")
        direction, op, size, checksum = struct.unpack("<BBHI", frame[:8])
        require(direction == 0 and len(frame) == size + 8 and op in (5, 6, 7, 8), "Unexpected command on actual QEMU-only ROM wire")
        requests.append(dict(opcode=op, checksum=checksum, data=frame[8:]))
    for frame in received:
        require(len(frame) >= 12, "Short actual ROM reply")
        direction, op, size, value = struct.unpack("<BBHI", frame[:8])
        require(direction == 1 and op in (5, 6, 7, 8) and size == 4 and len(frame) == 12,
                "Malformed actual ESP32-S3 ROM status reply")
        require(frame[8:10] == b"\x00\x00", f"Actual ROM reported failure status for opcode {op}")
        responses.append(dict(opcode=op, value=value, status_hex=frame[8:].hex()))
    blocks = (len(payload) + block_size - 1) // block_size
    # The normal esptool connection procedure can send several identical SYNC
    # packets while the real ROM acquires baud. Only that prefix may repeat.
    sync_count = 0
    while sync_count < len(requests) and requests[sync_count]["opcode"] == 8:
        sync_count += 1
    require(1 <= sync_count <= 5, "Unexpected normal ROM SYNC attempt count")
    for request in requests[:sync_count]:
        require(request["checksum"] == 0 and request["data"] == b"\x07\x07\x12\x20" + b"\x55" * 32,
                "Actual normal ROM SYNC payload differs")
    memory_requests = requests[sync_count:]
    require([request["opcode"] for request in memory_requests] == [5] + [7] * blocks + [6],
            "Actual sent command sequence differs from SYNC/MEM-only traversal")
    require(memory_requests[0]["checksum"] == 0 and memory_requests[-1]["checksum"] == 0,
            "Actual MEM_BEGIN/MEM_END checksums must be zero")
    require(memory_requests[0]["data"] == struct.pack("<IIII", len(payload), blocks, block_size, address),
            "Actual MEM_BEGIN address/size differs")
    for sequence, request in enumerate(memory_requests[1:-1]):
        data = payload[sequence * block_size:(sequence + 1) * block_size]
        require(request["data"] == struct.pack("<IIII", len(data), sequence, 0, 0) + data, f"Actual MEM_DATA{sequence} bytes differ")
        checksum = 0xef
        for byte in data:
            checksum ^= byte
        require(request["checksum"] == checksum, f"Actual MEM_DATA{sequence} checksum differs")
    require(memory_requests[-1]["data"] == struct.pack("<II", 1, 0), "MEM_END must have no-execute flag and zero entry")
    response_sync_count = 0
    while response_sync_count < len(responses) and responses[response_sync_count]["opcode"] == 8:
        response_sync_count += 1
    # Ordinary acquisition may receive replies to more than one retried SYNC.
    # Every received request produces eight replies; retain all complete groups.
    require(8 <= response_sync_count <= sync_count * 8 and response_sync_count % 8 == 0,
            "Missing/incomplete or unsolicited real ROM SYNC reply group")
    require([response["opcode"] for response in responses[response_sync_count:]] ==
            [5] + [7] * blocks + [6], "Missing/mismatched real ROM memory ACK traversal")
    require(all(response["value"] != 0 for response in responses[:response_sync_count]),
            "SYNC replies identify a stub, not the actual ROM")
    require(max(len(request["data"]) - 16 for request in memory_requests if request["opcode"] == 7) > 128,
            "ROM regression lacks a >FIFO actual MEM_DATA payload")
    synced = False
    for command in commands:
        if command.get("reply") is not None:
            synced |= command["opcode"] == 8
        else:
            require(not synced and command["opcode"] == 8 and command.get("error"),
                    "esptool did not observe every actual protocol reply after initial SYNC acquisition")
    require(synced, "esptool never acquired the real ROM")
    return dict(request_count=len(requests), response_count=len(responses), data_blocks=blocks,
                sync_requests=sync_count, sync_reply_groups=response_sync_count // 8,
                payload_bytes=len(payload), responses=responses,
                qualification="Real ROM checksum/status ACK traversal; no execution, flash write or RAM readback claim")


def run(args):
    frozen = json.loads((args.frozen / "freeze.json").read_text())
    require(frozen["base_qemu_commit"] == BASE_COMMIT, "ROM frozen base lock mismatch")
    for name, sha in frozen["files"].items():
        require(digest(args.frozen / name) == sha, f"ROM frozen input changed: {name}")
    require(digest(args.qemu) == frozen["qemu_sha256"], "ROM and ordinary fixture must use the same frozen native executable")
    require(digest(pathlib.Path(__file__).resolve()) == frozen["files"]["run-rom-download.py"], "ROM runner differs from its frozen source")
    require(digest(NATIVE_RUNNER) == frozen["files"]["run-native-fixture.py"], "Public QMP helper differs from its frozen source")
    rom_class, _, _, sources = installed_tool()
    require(sources == frozen["esptool_source_sha256"], "Installed pinned esptool source differs from frozen protocol tooling")
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix="rom-download-", dir=args.evidence.resolve()))
    shutil.copyfile(args.frozen / "flash.bin", evidence / "flash-working.bin")
    payload = (args.frozen / "payload.bin").read_bytes()
    metadata = json.loads((args.frozen / "payload.json").read_text())
    result = dict(status="FAIL", commands=[], qmp=[], snapshots=[], qemu_sha256=frozen["qemu_sha256"],
                  frozen_manifest_sha256=digest(args.frozen / "freeze.json"),
                  payload_sha256=hashlib.sha256(payload).hexdigest(), esptool_version=PINNED_ESPTOOL,
                  host_watchdog_seconds=args.watchdog_seconds,
                  host_command_timeout_seconds=args.command_timeout_seconds)
    process = qmp = relay = uart_socket = None
    qemu_listener = listener()
    tool_listener = listener()
    started = time.monotonic()
    deadline = started + args.watchdog_seconds

    class CapturedROM(rom_class):
        def command(self, op=None, data=b"", chk=0, wait_response=True, timeout=3):
            require(op in (None, 5, 6, 7, 8), "Only SYNC/MEM protocol is permitted; no register/flash/eFuse commands")
            entry = dict(opcode=op, data_size=len(data), data_sha256=hashlib.sha256(data).hexdigest(),
                         checksum=chk, esptool_timeout_seconds=timeout,
                         started_monotonic_ns=time.monotonic_ns())
            result["commands"].append(entry)
            remaining = deadline - time.monotonic()
            require(remaining > 0, "Host ROM diagnostic watchdog expired")
            # Hardware esptool deadlines are host time, not modeled UART time.
            # Preserve ordinary commands/retries and bound all waits uniformly.
            command_timeout = min(max(timeout, args.command_timeout_seconds), remaining)
            entry["effective_timeout_seconds"] = command_timeout
            try:
                value, response = super().command(op, data, chk, wait_response, command_timeout)
                entry["reply"] = dict(value=value, data_hex=response.hex())
                return value, response
            except Exception as exc:
                entry["error"] = f"{type(exc).__name__}: {exc}"
                raise
            finally:
                entry["finished_monotonic_ns"] = time.monotonic_ns()

    try:
        with tempfile.TemporaryDirectory(prefix="rom-qmp-") as transport:
            qmp_path = pathlib.Path(transport) / "qmp.sock"
            command = [str(args.qemu.resolve()), "-machine", "esp32s3", "-nographic", "-S", "-monitor", "none",
                       "-accel", "tcg,thread=single",
                       "-icount", "shift=0,align=off,sleep=off",
                       "-global", "driver=esp32s3.gpio,property=strap_mode,value=0",
                       "-chardev", f"socket,id=romuart0,host=127.0.0.1,port={qemu_listener.getsockname()[1]},server=off",
                       "-serial", "chardev:romuart0", "-drive", f"file={evidence / 'flash-working.bin'},if=mtd,format=raw",
                       "-qmp", f"unix:{qmp_path},server=on,wait=off"]
            dump(evidence / "command.json", command)
            with (evidence / "stdout.log").open("wb") as stdout, (evidence / "stderr.log").open("wb") as stderr:
                process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
                qemu_listener.settimeout(max(.001, deadline - time.monotonic()))
                uart_socket, address = qemu_listener.accept()
                require(address[0] == "127.0.0.1", "Non-loopback QEMU UART peer rejected")
                relay = Relay(uart_socket, tool_listener, evidence, deadline)
                while not qmp_path.exists():
                    require(process.poll() is None, "QEMU exited before ROM QMP; see stderr.log")
                    require(time.monotonic() < deadline, "Host watchdog expired before ROM QMP")
                    time.sleep(.01)
                qmp = qmp_class()(qmp_path, result["qmp"], deadline)
                # Fresh VM with no applied project: console/download UART0 is
                # independent of physical circuits. Empty text is the public
                # getter's no-project state, not a valid project to Apply.
                require(qmp.call("qom-get", dict(path="/machine/soc/electrical", property="project-json")) == "",
                        "ROM regression must start with no external electrical project")
                result["snapshots"].append(dict(phase="empty_external_graph_before_ROM", graph=qmp.snapshot()))
                with CapturedROM(f"socket://127.0.0.1:{tool_listener.getsockname()[1]}", baud=115200) as rom:
                    qmp.call("cont")
                    while not relay.banner_ready.is_set():
                        require(relay.error is None, f"UART0 relay failed: {relay.error}")
                        require(process.poll() is None, "QEMU exited before real ROM download banner")
                        require(time.monotonic() < deadline, "No real ROM download banner before host watchdog")
                        time.sleep(.01)
                    # Standard esptool SYNC acquisition, without resets or
                    # chip-detection register/security commands.
                    rom.connect(mode="no-reset", attempts=1, detecting=True, warnings=False)
                    require(not rom.sync_stub_detected and not rom.IS_STUB, "SYNC did not identify the actual ROM")
                    block_size = metadata["block_size"]
                    blocks = (len(payload) + block_size - 1) // block_size
                    rom.mem_begin(len(payload), blocks, block_size, metadata["address"])
                    for sequence in range(blocks):
                        rom.mem_block(payload[sequence * block_size:(sequence + 1) * block_size], sequence)
                    # Ordinary esptool check_command, exactly mem_finish(0)'s
                    # no-execute payload. Unlike mem_finish(), do not suppress
                    # a missing ROM MEM_END reply: acceptance requires its ACK.
                    rom.check_command("finish RAM download without execution", rom.ESP_CMDS["MEM_END"],
                                      data=struct.pack("<II", 1, 0), timeout=3)
                    qmp.call("stop")
                    result["snapshots"].append(dict(phase="ROM_ACK_completed_empty_external_graph", graph=qmp.snapshot()))
                relay.close()
                require(relay.error is None, f"UART0 relay failed: {relay.error}")
                result["serial_events"] = relay.events
                result["proof"] = protocol_proof(evidence, payload, metadata["address"], metadata["block_size"], result["commands"])
                require(digest(evidence / "flash-working.bin") == frozen["files"]["flash.bin"], "ROM memory-only regression changed flash")
                result["status"] = "PASS"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if relay is not None:
            try:
                relay.close()
            except Exception as exc:
                result["cleanup_error"] = f"{type(exc).__name__}: {exc}"
                result["status"] = "FAIL"
            result["serial_events"] = relay.events
            result["relay_error"] = relay.error
        if qmp is not None:
            qmp.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if uart_socket is not None:
            uart_socket.close()
        qemu_listener.close()
        tool_listener.close()
        result["host_elapsed_seconds"] = time.monotonic() - started
        result["evidence_sha256"] = {path.name: digest(path) for path in evidence.iterdir() if path.is_file()}
        dump(evidence / "result.json", result)
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), error=result.get("error"))))
    return 0 if result["status"] == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    freezing = sub.add_parser("freeze")
    freezing.add_argument("--boot-frozen", required=True, type=pathlib.Path)
    freezing.add_argument("--output", required=True, type=pathlib.Path)
    running = sub.add_parser("run")
    for name in ("qemu", "frozen", "evidence"):
        running.add_argument(f"--{name}", required=True, type=pathlib.Path)
    running.add_argument("--watchdog-seconds", type=float, default=900,
                         help="Overall host diagnostic deadline; never guest UART/ROM time")
    running.add_argument("--command-timeout-seconds", type=float, default=180,
                         help="Minimum host wait per ordinary esptool command, including SYNC replies")
    args = parser.parse_args()
    if args.action == "run":
        if not 1 <= args.watchdog_seconds <= 900:
            parser.error("--watchdog-seconds must be within1..900")
        if not 0 < args.command_timeout_seconds <= min(240, args.watchdog_seconds):
            parser.error("--command-timeout-seconds must be positive, <=240 and <=watchdog-seconds")
    return freeze(args) if args.action == "freeze" else run(args)


if __name__ == "__main__":
    raise SystemExit(main())
