#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real Qt-peer/QMP integration probe; never substitutes for a QEMU binary.

Uses a copied flash image, local sockets, and an isolated artifact directory.
Requires the separately built GPL prototype in the actual supplied QEMU binary.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Peer:
    def __init__(self, executable, directory, delay, latency, drop=False):
        self.records = []
        self.condition = threading.Condition()
        self.output = open(directory / "peer.jsonl", "w", encoding="utf-8")
        self.errors = open(directory / "peer.stderr", "w", encoding="utf-8")
        args = [executable, "--delay-ms", str(delay), "--latency-ns", str(latency)]
        if drop:
            args.append("--drop-replies")
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.errors,
                                        text=True, bufsize=1)
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            self.output.write(line)
            self.output.flush()
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                record = {"event": "invalid-output", "raw": line}
            with self.condition:
                self.records.append(record)
                self.condition.notify_all()
        with self.condition:
            self.condition.notify_all()

    def wait(self, event, after=0, timeout=5, predicate=lambda record: True):
        end = time.monotonic() + timeout
        with self.condition:
            while time.monotonic() < end:
                for record in self.records[after:]:
                    if record.get("event") == event and predicate(record):
                        return record
                self.condition.wait(min(0.1, end - time.monotonic()))
        raise AssertionError(f"Peer did not emit {event}: {self.records}")

    def command(self, record):
        self.process.stdin.write(json.dumps(record) + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.thread.join(timeout=5)
        self.output.close()
        self.errors.close()


class Qmp:
    def __init__(self, address, process, directory):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        end = time.monotonic() + 10
        while time.monotonic() < end:
            try:
                self.socket.connect(str(address))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                require(process.poll() is None, "QEMU exited before opening QMP")
                time.sleep(0.02)
        else:
            raise AssertionError("QMP socket startup deadline exceeded")
        self.socket.settimeout(3)
        self.buffer = bytearray()
        self.events = []
        self.serial = 0
        self.output = open(directory / "qmp.jsonl", "w", encoding="utf-8")
        self.log("receive", self.read())
        self.command("qmp_capabilities")

    def log(self, direction, message):
        self.output.write(json.dumps({"host_monotonic_ns": time.monotonic_ns(),
                                      "direction": direction, "message": message}) + "\n")
        self.output.flush()

    def read(self):
        while b"\n" not in self.buffer:
            incoming = self.socket.recv(65536)
            require(incoming, "QMP disconnected")
            self.buffer.extend(incoming)
            require(len(self.buffer) <= 4 * 1024 * 1024, "QMP line limit exceeded")
        end = self.buffer.index(b"\n")
        line = bytes(self.buffer[:end])
        del self.buffer[:end + 1]
        return json.loads(line)

    def command(self, name, arguments=None, expect_error=False):
        self.serial += 1
        record = {"execute": name, "id": self.serial}
        if arguments is not None:
            record["arguments"] = arguments
        self.log("send", record)
        self.socket.sendall(json.dumps(record).encode() + b"\r\n")
        end = time.monotonic() + 4
        while time.monotonic() < end:
            reply = self.read()
            self.log("receive", reply)
            if "event" in reply:
                self.events.append(reply)
            elif reply.get("id") == self.serial:
                if expect_error:
                    require("error" in reply, f"Expected QMP rejection: {record} -> {reply}")
                    return reply["error"]
                require("error" not in reply, f"QMP rejected {record}: {reply}")
                return reply["return"]
        raise AssertionError(f"QMP deadline exceeded: {name}")

    def get(self, property_name):
        return self.command("qom-get", {"path": "/objects/probe", "property": property_name})

    def control(self, value, expect_error=False):
        return self.command("qom-set", {"path": "/objects/probe", "property": "control",
                                        "value": value}, expect_error)

    def wait_phase(self, phases, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            phase = self.get("phase")
            if phase in phases:
                return phase
            time.sleep(0.01)
        raise AssertionError(f"Probe phase deadline: expected {phases}, got {phase}, "
                             f"error={self.get('last-error')}")

    def close(self):
        self.socket.close()
        self.output.close()


class WireProxy:
    """Transparent local capture, preserving exact bytes in both directions."""
    def __init__(self, peer_port, directory):
        self.peer_port = peer_port
        self.directory = directory
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.done = threading.Event()
        self.sockets = []
        self.threads = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        connection = 0
        while not self.done.is_set():
            try:
                qemu, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            connection += 1
            try:
                host = socket.create_connection(("127.0.0.1", self.peer_port), timeout=3)
            except OSError:
                qemu.close()
                continue
            qemu.settimeout(0.2)
            host.settimeout(0.2)
            self.sockets.extend([qemu, host])
            for source, target, direction in ((qemu, host, "qemu-to-host"),
                                               (host, qemu, "host-to-qemu")):
                thread = threading.Thread(target=self.forward,
                                          args=(source, target, connection, direction), daemon=True)
                self.threads.append(thread)
                thread.start()

    def forward(self, source, target, connection, direction):
        count = 0
        with open(self.directory / f"wire-{connection}-{direction}.bin", "wb") as raw:
            while not self.done.is_set():
                try:
                    block = source.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not block:
                    break
                try:
                    count += len(block)
                    require(count <= 16 * 1024 * 1024, "Wire capture limit exceeded")
                    raw.write(block)
                    raw.flush()
                    target.sendall(block)
                except (OSError, AssertionError):
                    break
        for endpoint in (source, target):
            try:
                endpoint.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def close(self):
        self.done.set()
        self.listener.close()
        for endpoint in self.sockets:
            endpoint.close()
        self.thread.join(timeout=2)
        for thread in self.threads:
            thread.join(timeout=2)


def reserve_port():
    with socket.socket() as temporary:
        temporary.bind(("127.0.0.1", 0))
        return temporary.getsockname()[1]


def verify_captured_frames(directory):
    records = []
    for path in sorted(directory.glob("wire-*.bin")):
        raw = path.read_bytes()
        offset = 0
        while offset < len(raw):
            require(len(raw) - offset >= 4, f"Truncated wire prefix in {path}")
            length = int.from_bytes(raw[offset:offset + 4], "big")
            require(0 < length <= 1024 * 1024, f"Invalid wire frame length in {path}")
            offset += 4
            require(len(raw) - offset >= length, f"Truncated wire body in {path}")
            frame = raw[offset:offset + length]
            offset += length
            envelope = json.loads(frame)
            records.append({"wire_file": path.name, "byte_count": length,
                            "body_sha256": hashlib.sha256(frame).hexdigest(),
                            "envelope": envelope})
    require(records, "No actual bus frames captured")
    (directory / "wire-envelopes.jsonl").write_text(
        "".join(json.dumps(record) + "\n" for record in records))
    return len(records)


def assert_stopped(qmp, expected, duration=0.05):
    end = time.monotonic() + duration
    while time.monotonic() < end:
        require(qmp.command("query-status")["status"] == "paused", "VM resumed unexpectedly")
        require(int(qmp.get("virtual-ns")) == expected, "Host wait advanced the guest clock")
        time.sleep(0.01)


def assert_start_guard(qmp, expected, gdb_port=None, raw_step=False):
    before = len(qmp.events)
    qmp.command("cont")  # Existing void vm_start may ACK without starting.
    assert_stopped(qmp, expected)
    require(not any(event["event"] == "RESUME" for event in qmp.events[before:]),
            "Blocked cont emitted RESUME")
    if gdb_port is not None:
        with socket.create_connection(("127.0.0.1", gdb_port), timeout=2) as debugger:
            debugger.settimeout(1)
            received = bytearray()

            def exchange(payload):
                packet = b"$" + payload + b"#" + f"{sum(payload) % 256:02x}".encode()
                qmp.log("gdb-send", payload.decode())
                debugger.sendall(packet)
                end = time.monotonic() + 2
                while time.monotonic() < end:
                    if b"$" not in received or b"#" not in received[received.index(b"$"):]:
                        block = debugger.recv(4096)
                        require(block, "GDB disconnected while checking preflight")
                        received.extend(block)
                        continue
                    start = received.index(b"$")
                    final = received.index(b"#", start)
                    if len(received) < final + 3:
                        received.extend(debugger.recv(4096))
                        continue
                    result = bytes(received[start + 1:final])
                    checksum = int(received[final + 1:final + 3], 16)
                    del received[:final + 3]
                    require(sum(result) % 256 == checksum, "Invalid GDB response checksum")
                    debugger.sendall(b"+")
                    qmp.log("gdb-receive", result.decode())
                    if result.startswith(b"T"):
                        continue
                    return result
                raise AssertionError("GDB preflight response deadline exceeded")

            step_flags = qmp.get("cpu-step-flags")
            registers = qmp.command("human-monitor-command", {"command-line": "info registers"})
            for payload in (b"c", b"c40000000", b"C05", b"s", b"s40000000",
                            b"vCont;c", b"vCont;s", b"vCont;S05", b"bs", b"bc", b"S05"):
                response = exchange(payload)
                # Pinned QEMU has no plain 'S' dispatcher; its empty response
                # explicitly means unsupported, whereas vCont;S is guarded.
                require(response == (b"" if payload == b"S05" else b"E16"),
                        f"GDB command was not rejected before mutation: {payload} -> {response}")
                require(qmp.get("cpu-step-flags") == step_flags, "Rejected GDB command changed step flags")
                require(qmp.command("human-monitor-command", {"command-line": "info registers"}) == registers,
                        "Rejected GDB command changed CPU PC/registers")
                assert_stopped(qmp, expected)
        require(not any(event["event"] == "RESUME" for event in qmp.events[before:]),
            "Blocked GDB continue/step emitted RESUME")


def read_uart(uart, attempts=40, pause=0.05):
    last = None
    for _ in range(attempts):
        try:
            return uart.read_text(errors="replace")
        except OSError as error:
            # A Linux writer on /mnt/c (DrvFS/9p) can transiently fail a
            # concurrent read with ENODATA/EAGAIN/EIO even though the file
            # exists and grows. Retry briefly, then preserve the filesystem
            # error and full cause; a live DrvFS read failure must never
            # silently become missing/empty firmware evidence.
            if error.errno not in (errno.EAGAIN, errno.ENODATA, errno.EIO):
                raise
            last = error
            time.sleep(pause)
    raise OSError(last.errno,
                  f"Live UART artifact read failed after {attempts} attempts: {last.strerror}",
                  str(uart)) from last


def core_heartbeats(text):
    evidence = {"0": [], "1": []}
    for line in text.splitlines(keepends=True):
        if not line.startswith("ESP32S3VM_CORE_EXEC ") or not line.endswith("\n"):
            continue  # A live final line can still be incomplete.
        match = re.fullmatch(r"ESP32S3VM_CORE_EXEC assigned=([01]) core=([01]) iteration=([1-9][0-9]*) time_us=([0-9]+)",
                             line.removesuffix("\n").removesuffix("\r"))
        require(match, f"Malformed completed core heartbeat: {line!r}")
        assigned, actual, iteration, time_us = match.groups()
        require(assigned == actual, f"Pinned guest task ran on the wrong core: {line!r}")
        previous = evidence[actual]
        sample = {"iteration": int(iteration), "time_us": int(time_us)}
        if previous:
            require(sample["iteration"] > previous[-1]["iteration"], "Core heartbeat iteration did not progress")
            require(sample["time_us"] > previous[-1]["time_us"], "Core heartbeat virtual time did not progress")
        previous.append(sample)
    return evidence


def cores_executed(evidence):
    return all([sample["iteration"] for sample in evidence[str(core)][:3]] == [1, 2, 3]
               for core in (0, 1))


def boot(qmp, uart, marker, timeout, require_core_heartbeats=False):
    require(qmp.wait_phase({"ready"}) == "ready", "Host handshake not ready")
    cpus = qmp.command("query-cpus-fast")
    require(len(cpus) == 2, f"Expected two ESP32-S3 CPUs: {cpus}")
    qmp.command("cont")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        text = read_uart(uart)
        evidence = core_heartbeats(text)
        if marker in text and (not require_core_heartbeats or cores_executed(evidence)):
            qmp.command("stop")
            require(qmp.command("query-status")["status"] == "paused", "Boot stop not acknowledged")
            qmp.log("guest-core-execution", {"required": require_core_heartbeats,
                    "established": cores_executed(evidence), "markers": evidence})
            return cpus
        time.sleep(0.05)
    raise AssertionError(f"Normal firmware boot marker absent after {timeout}s: "
                         f"required_core_heartbeats={require_core_heartbeats}: {read_uart(uart)[-4000:]}")


def wait_chardev_disconnect(qmp, timeout=3):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        record = next((entry for entry in qmp.command("query-chardev") if entry["label"] == "hostbus"), None)
        require(record is not None, "Hostbus chardev disappeared before disconnect proof")
        if record["filename"].startswith("disconnected:"):
            qmp.log("hostbus-disconnect-observed", record)
            return record
        time.sleep(0.01)
    raise AssertionError("QEMU did not observe hostbus socket closure")


def arm(qmp, peer):
    start = len(peer.records)
    qmp.control("arm:1000000")
    armed = int(qmp.get("armed-ns"))
    qmp.command("cont")
    try:
        request = peer.wait("request", after=start)
    except AssertionError:
        # Preserve the live probe/runstate evidence before failing: without
        # it an arm-timeout diagnosis would be guesswork.
        diagnosis = {}
        for prop in ("phase", "resume-blocked", "virtual-ns", "armed-ns",
                     "stopped-ns", "delivered-ns", "last-error"):
            try:
                diagnosis[prop] = qmp.get(prop)
            except (AssertionError, OSError) as error:
                diagnosis[prop] = f"unavailable: {error}"
        try:
            diagnosis["vm_status"] = qmp.command("query-status")["status"]
        except (AssertionError, OSError) as error:
            diagnosis["vm_status"] = f"unavailable: {error}"
        raise AssertionError(f"Framed request missing after arm+cont: {diagnosis}") from None
    require(int(request["virtual_ns"]) == armed, "Framed request missed the armed deadline")
    require(int(qmp.get("stopped-ns")) == armed, "Global stop missed its virtual deadline")
    require(qmp.get("resume-blocked") is True, "Dependency did not own resume blocker")
    return armed, start, request


def archive_uart(uart, destination):
    """Archive only after QEMU exits; retain native raw bytes on copy failure."""
    try:
        with uart.open("rb") as source, destination.open("xb") as target:
            shutil.copyfileobj(source, target)
            target.flush()
            os.fsync(target.fileno())
    except OSError as error:
        raise OSError(f"Durable UART archive failed; raw retained at {uart}: {error}") from error
    uart.unlink()


def scenario(options, root, name, delay, latency=1000000, drop=False):
    directory = root / name
    directory.mkdir()
    shutil.copyfile(options.flash, directory / "flash.bin")
    # Keep Unix socket path below the platform limit even for long output paths.
    with tempfile.TemporaryDirectory(prefix="s3hb-") as sockets:
        peer = Peer(options.peer, directory, delay, latency, drop)
        listening = peer.wait("listening")
        proxy = WireProxy(listening["port"], directory)
        gdb_port = reserve_port()
        # Keep per-byte console writes off DrvFS. This independent native file
        # survives socket-tempdir cleanup if the durable archive fails.
        uart_fd, uart_name = tempfile.mkstemp(prefix="s3hb-uart-", suffix=".log",
                                            dir=Path(sockets).parent)
        os.close(uart_fd)
        uart = Path(uart_name)
        args = [options.qemu, "-M", "esp32s3", "-global",
                "driver=esp32s3.gpio,property=strap_mode,value=0x04",
                "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
                "-display", "none", "-monitor", "none", "-serial", f"file:{uart}",
                "-S", "-qmp", f"unix:{sockets}/qmp,server=on,wait=off",
                "-gdb", f"tcp:127.0.0.1:{gdb_port}", "-L", options.data_dir,
                "-drive", f"file={directory / 'flash.bin'},if=mtd,format=raw",
                "-chardev", f"socket,id=hostbus,host=127.0.0.1,port={proxy.port},"
                            "server=off,reconnect-ms=200",
                "-object", f"esp32s3-hostbus-probe,id=probe,chardev=hostbus,watchdog-ms={100 if drop else 5000}"]
        args.extend(options.extra_qemu_arg)
        (directory / "command.json").write_text(json.dumps(args, indent=2) + "\n")
        stderr = open(directory / "qemu.stderr", "w")
        process = subprocess.Popen(args, stdout=stderr, stderr=stderr)
        qmp = None
        try:
            qmp = Qmp(Path(sockets) / "qmp", process, directory)
            peer.wait("ready")
            cpus = boot(qmp, uart, options.boot_marker, options.boot_timeout, options.require_core_heartbeats)
            initial_core_evidence = core_heartbeats(read_uart(uart))
            deadline, start, request = arm(qmp, peer)
            if name.startswith("delay-") or name == "gdb-raw-step":
                assert_start_guard(qmp, deadline, gdb_port, raw_step=True)
                peer.wait("response", after=start, predicate=lambda r: r.get("accepted") is True)
                qmp.wait_phase({"resolved"})
                assert_stopped(qmp, deadline)
                qmp.command("object-del", {"id": "probe"}, expect_error=True)
                qmp.control("release")
                assert_stopped(qmp, deadline)
                require(qmp.get("resume-blocked") is False, "Explicit release did not clear gate")
                qmp.command("cont")
                qmp.wait_phase({"completed"})
                delivered = int(qmp.get("delivered-ns"))
                require(delivered == deadline + latency, "Modeled completion did not fire at its virtual time")
                qmp.command("stop")
                result = {"stopped_ns": deadline, "delivered_ns": delivered,
                          "modeled_latency_ns": latency, "host_delay_ms": delay}
            elif name == "watchdog":
                qmp.wait_phase({"blocked-error"})
                require("Host watchdog expired" in qmp.get("last-error"), "Watchdog cause lost")
                assert_start_guard(qmp, deadline)
                qmp.control("release", expect_error=True)
                assert_stopped(qmp, deadline, 0.15)
                result = {"stopped_ns": deadline, "error": qmp.get("last-error")}
            elif name == "disconnect":
                peer.process.terminate()
                peer.process.wait(timeout=5)
                qmp.wait_phase({"blocked-error"})
                assert_start_guard(qmp, deadline)
                qmp.control("release", expect_error=True)
                result = {"stopped_ns": deadline, "error": qmp.get("last-error")}
            elif name == "reset":
                previous_connection = request["connection_id"]
                qmp.command("system_reset")
                peer.command({"op": "generation", "epoch": "1", "topology": "0",
                              "virtual_ns": qmp.get("virtual-ns")})
                peer.wait("control-ack", after=start, predicate=lambda r: r.get("accepted") is True)
                peer.wait("ready", after=start,
                          predicate=lambda r: r.get("connection_id") != previous_connection)
                qmp.wait_phase({"ready"})
                new_deadline, new_start, new_request = arm(qmp, peer)
                require(new_request["epoch"] == "1", "New request used old reset epoch")
                peer.wait("response", after=start,
                          predicate=lambda r: r.get("accepted") is False)
                # The old delayed model callback cannot resolve the new request.
                require(qmp.get("phase") == "blocked", "Old callback resolved the new barrier")
                assert_stopped(qmp, new_deadline, 0.01)
                peer.wait("response", after=new_start,
                          predicate=lambda r: r.get("accepted") is True)
                qmp.wait_phase({"resolved"})
                assert_stopped(qmp, new_deadline)
                result = {"old_stopped_ns": deadline, "new_stopped_ns": new_deadline,
                          "new_reset_epoch": "1", "old_callback_rejected": True}
            elif name == "released-reset":
                peer.wait("response", after=start, predicate=lambda r: r.get("accepted") is True)
                qmp.wait_phase({"resolved"})
                qmp.control("release")
                require(qmp.get("phase") == "completion-scheduled", "Future completion not pending")
                cancelled_completion = deadline + latency
                peer.process.terminate()
                peer.process.wait(timeout=5)
                # Observe actual QEMU backend closure before reset. A fixed
                # sleep could otherwise miss the released/disconnected stage.
                wait_chardev_disconnect(qmp)
                require(qmp.get("phase") == "completion-scheduled", "Disconnect lost pending completion stage")
                qmp.command("system_reset")
                require(qmp.get("delivered-ns") == "-1", "Reset retained old completion evidence")
                require(qmp.get("phase") in ("disconnected", "error"), "Reset retained completion state")
                require(qmp.get("resume-blocked") is False, "Reset retained barrier ownership")
                qmp.command("cont")
                end = time.monotonic() + options.boot_timeout
                while time.monotonic() < end:
                    if int(qmp.get("virtual-ns")) > cancelled_completion:
                        break
                    time.sleep(0.02)
                else:
                    raise AssertionError("Guest did not advance past cancelled completion deadline")
                qmp.command("stop")
                require(qmp.get("delivered-ns") == "-1", "Old completion fired after reset")
                result = {"cancelled_completion_ns": cancelled_completion,
                          "post_reset_virtual_ns": int(qmp.get("virtual-ns")),
                          "old_completion_cancelled": True}
            else:
                raise AssertionError(f"Unknown scenario {name}")
            result.update({"scenario": name, "passed": True, "cpus": cpus,
                           "core_execution": {"required": options.require_core_heartbeats,
                            "established": cores_executed(initial_core_evidence),
                            "evidence_source": "initial guest UART heartbeats before any reset",
                            "markers": initial_core_evidence}})
            (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            return result
        except Exception as error:
            detail = traceback.format_exc()
            (directory / "failure.traceback.txt").write_text(detail)
            (directory / "result.json").write_text(json.dumps({"scenario": name, "passed": False,
                                                              "error": str(error), "traceback": detail}, indent=2) + "\n")
            raise
        finally:
            try:
                if qmp is not None:
                    try:
                        qmp.command("quit")
                    except (OSError, AssertionError):
                        pass
                    qmp.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            finally:
                stderr.close()
                try:
                    if process.poll() is None:
                        raise OSError(f"QEMU still running; raw UART retained at {uart}")
                    archive_uart(uart, directory / "uart.log")
                finally:
                    try:
                        peer.close()
                    finally:
                        proxy.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--peer", required=True)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--boot-marker", default="ESP32S3VM_BOOT_OK cores=2 flash=4194304")
    parser.add_argument("--boot-timeout", type=float, default=90,
                        help="Wall-clock budget for the boot marker to become visible; /mnt/c DrvFS readers can serve appended UART data tens of seconds late")
    parser.add_argument("--require-core-heartbeats", action="store_true",
                        help="Require iterations 1/2/3 on both pinned guest cores; use the normal IDF dualcore helper image")
    parser.add_argument("--extra-qemu-arg", action="append", default=[])
    parser.add_argument("--scenario", action="append", choices=["delay-fast", "delay-slow", "watchdog", "disconnect", "reset", "released-reset", "gdb-raw-step"])
    options = parser.parse_args()
    root = options.output or Path(tempfile.mkdtemp(prefix="esp32s3vm-hostbus-"))
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    options.flash = options.flash.resolve()
    require(options.flash.stat().st_size == 4 * 1024 * 1024, "Supply a complete 4 MiB flash image")
    selected = options.scenario or ["delay-fast", "delay-slow", "watchdog", "disconnect", "reset", "released-reset", "gdb-raw-step"]
    manifest = {"base_commit": "40edccac415693c5130f91c01d84176ae6008566",
                "qemu_sha256": digest(options.qemu), "peer_sha256": digest(options.peer),
                "flash_sha256": digest(options.flash), "scope": "QOM dependency probe only",
                "native_peripheral_support": False, "scenarios": selected}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    try:
        for name in selected:
            result = scenario(options, root, name,
                              delay=40 if name == "delay-fast" else 500 if name == "reset" else 300,
                              latency=1000000000 if name == "released-reset" else 1000000,
                              drop=name in ("watchdog", "disconnect"))
            result["captured_frames"] = verify_captured_frames(root / name)
            (root / name / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            print(json.dumps(result), flush=True)
    except Exception as error:
        detail = traceback.format_exc()
        (root / "failure.traceback.txt").write_text(detail)
        print(json.dumps({"passed": False, "error": str(error), "artifacts": str(root), "traceback": detail}), flush=True)
        return 1
    print(json.dumps({"passed": True, "scenario_count": len(results), "artifacts": str(root)}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
