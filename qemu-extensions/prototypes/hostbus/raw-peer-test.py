#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fault/reconnect tests of the real QEMU prototype using raw v1 frames.

This complements the actual Qt transport interop runner; it is not a fake-QEMU
test. Guest time is advanced only by the supplied fixed-icount QEMU binary.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

spec = importlib.util.spec_from_file_location("hostbus_probe_test", Path(__file__).with_name("probe-test.py"))
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
require = shared.require
Qmp = shared.Qmp


class RawPeer:
    def __init__(self, directory):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(4)
        self.port = self.listener.getsockname()[1]
        self.socket = None
        self.sequence = 0
        self.directory = directory
        self.number = 0
        self.valid_context = None

    def connect(self):
        if self.socket is not None:
            self.disconnect()
        self.socket, _ = self.listener.accept()
        self.socket.settimeout(3)
        self.number += 1
        self.sent = open(self.directory / f"raw-{self.number}-host-to-qemu.bin", "wb")
        self.received = open(self.directory / f"raw-{self.number}-qemu-to-host.bin", "wb")

    def send_bytes(self, body):
        wire = len(body).to_bytes(4, "big") + body
        self.sent.write(wire)
        self.sent.flush()
        self.socket.sendall(wire)

    def send(self, envelope):
        self.send_bytes(json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode())

    def read(self):
        def exact(length):
            result = bytearray()
            while len(result) < length:
                block = self.socket.recv(length - len(result))
                require(block, "QEMU raw channel closed unexpectedly")
                result.extend(block)
                self.received.write(block)
                self.received.flush()
            return bytes(result)
        length = int.from_bytes(exact(4), "big")
        require(0 < length <= 1024 * 1024, "QEMU emitted an invalid frame length")
        return json.loads(exact(length))

    def envelope(self, kind, status="ok", ordinal="", virtual_ns="0", data=None):
        self.sequence += 1
        return {"version": {"major": 1, "minor": 0}, "kind": kind, "status": status,
                "session_id": self.session, "connection_id": self.connection,
                "reset_epoch": str(self.epoch), "topology_generation": str(self.topology),
                "sequence": str(self.sequence), "request_id": ordinal,
                "virtual_time_ns": str(virtual_ns), "data": data or {}}

    def hello_record(self, session, epoch, topology, connection=None):
        self.session, self.epoch, self.topology = session, epoch, topology
        self.connection = connection or str(uuid.uuid4())
        self.sequence = 0
        return self.envelope("hello", data={
            "capabilities": ["bus.gpio.v1", "control.reset-generation", "payload.base64"],
            "required_capabilities": ["control.reset-generation", "payload.base64"],
            "limits": {"frame_bytes": 1024 * 1024, "bus_payload_bytes": 64 * 1024, "in_flight": 1}})

    def negotiate(self, qmp, session, epoch=0, topology=0, connection=None):
        self.connect()
        self.send(self.hello_record(session, epoch, topology, connection))
        ack = self.read()
        require(ack["kind"] == "hello_ack" and ack["connection_id"] == self.connection, "Bad real QEMU hello ack")
        qmp.wait_phase({"ready"})
        self.valid_context = (session, epoch, topology)
        return self.connection

    def bad_hello(self, qmp, session, epoch, topology, connection=None,
                  mutate=lambda record: record, recovery_context=None):
        self.connect()
        rejected = mutate(self.hello_record(session, epoch, topology, connection))
        self.send(rejected)
        # The socket byte stream orders this known-valid hello after the invalid
        # candidate. Its ACK is an observed processing boundary, independent of
        # an inherited QOM error value. If the invalid hello was accepted, its
        # ACK arrives first and this assertion fails.
        require(recovery_context or self.valid_context, "A valid recovery context is required")
        good_session, good_epoch, good_topology = recovery_context or self.valid_context
        accepted = self.hello_record(good_session, good_epoch, good_topology)
        self.send(accepted)
        ack = self.read()
        require(ack["kind"] == "hello_ack" and ack["connection_id"] == accepted["connection_id"]
                and ack["session_id"] == good_session and ack["reset_epoch"] == str(good_epoch)
                and ack["topology_generation"] == str(good_topology),
                f"Invalid hello produced an ACK before the recovery boundary: {ack}")
        qmp.log("wire-rejection-boundary", {"candidate": rejected, "recovery_ack": ack})
        qmp.wait_phase({"ready"})
        require(qmp.get("last-error") == "", "Valid recovery hello failed to clear its own error")
        require(qmp.get("resume-blocked") is False, "Invalid idle hello created a barrier")
        self.valid_context = (good_session, good_epoch, good_topology)
        self.disconnect()

    def arm(self, qmp):
        qmp.control("arm:1000000")
        armed = int(qmp.get("armed-ns"))
        qmp.command("cont")
        request = self.read()
        require(request["kind"] == "request" and int(request["virtual_time_ns"]) == armed,
                "Real raw request missed scheduled dependency")
        shared.assert_stopped(qmp, armed, 0.01)
        return request

    def response(self, request, text="", ordinal=None):
        data = {key: request["data"][key] for key in ("bus", "controller_id", "endpoint_ids", "net_ids")}
        data.update({"phase_statuses": [{"index": 0, "status": "ack"}], "modeled_latency_ns": "0",
                     "accepted_length": 0, "payload_encoding": "base64", "payload": "Wg==",
                     "error_message": text})
        return self.envelope("response", ordinal=ordinal or request["request_id"],
                             virtual_ns=request["virtual_time_ns"], data=data)

    def disconnect(self):
        if self.socket is not None:
            endpoint = self.socket
            self.socket = None
            try:
                endpoint.shutdown(socket.SHUT_RDWR)
            except OSError:
                # Remote reset/cancel/limit closure is expected in this suite.
                pass
            finally:
                endpoint.close()
                self.sent.close()
                self.received.close()

    def close(self):
        try:
            self.disconnect()
        except OSError:
            pass
        self.listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    directory = (options.output or Path(tempfile.mkdtemp(prefix="esp32s3vm-hostbus-raw-"))).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(options.flash, directory / "flash.bin")
    peer = RawPeer(directory)
    process = qmp = None
    checks = []
    errors = open(directory / "qemu.stderr", "w")
    with tempfile.TemporaryDirectory(prefix="s3hbr-") as sockets:
        args = [options.qemu, "-M", "esp32s3", "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
                "-accel", "tcg,thread=single", "-icount", "shift=0,align=off,sleep=off",
                "-display", "none", "-monitor", "none", "-serial", f"file:{directory / 'uart.log'}", "-S",
                "-qmp", f"unix:{sockets}/qmp,server=on,wait=off", "-L", options.data_dir,
                "-drive", f"file={directory / 'flash.bin'},if=mtd,format=raw",
                "-chardev", f"socket,id=hostbus,host=127.0.0.1,port={peer.port},server=off,reconnect-ms=100",
                "-object", "esp32s3-hostbus-probe,id=probe,chardev=hostbus,watchdog-ms=5000"]
        (directory / "command.json").write_text(json.dumps(args, indent=2) + "\n")
        try:
            process = subprocess.Popen(args, stdout=errors, stderr=errors)
            qmp = Qmp(Path(sockets) / "qmp", process, directory)
            session = str(uuid.uuid4())
            old_nonce = peer.negotiate(qmp, session, 3, 7)
            peer.disconnect()
            peer.bad_hello(qmp, session, 2, 7)
            checks.append("same-session epoch rollback rejected")
            peer.bad_hello(qmp, session, 3, 6)
            checks.append("same-session topology rollback rejected")
            peer.negotiate(qmp, session, 3, 7)
            peer.disconnect()
            peer.bad_hello(qmp, session, 3, 7, old_nonce)
            checks.append("older nonlast connection UUID reuse rejected")
            peer.negotiate(qmp, session, 3, 7)
            peer.epoch, peer.topology = 4, 8
            peer.send(peer.envelope("reset", data={"reason": "raw floor regression"}))
            # No request is pending: a correctly framed response is discarded.
            # Its cumulative counter is an ordered processing boundary after
            # the preceding reset on this same socket.
            previous_discarded = int(qmp.get("late-replies"))
            peer.send(peer.envelope("response", ordinal="1", data={}))
            end = time.monotonic() + 3
            while int(qmp.get("late-replies")) == previous_discarded and time.monotonic() < end:
                time.sleep(0.01)
            require(int(qmp.get("late-replies")) == previous_discarded + 1,
                    "Host reset processing boundary was not observed")
            peer.valid_context = (session, 4, 8)
            peer.disconnect()
            peer.bad_hello(qmp, session, 3, 8)
            peer.bad_hello(qmp, session, 4, 7)
            checks.append("accepted host reset preserves both generation floors")
            peer.negotiate(qmp, session, 4, 8)
            peer.disconnect()
            session = str(uuid.uuid4())
            peer.negotiate(qmp, session, 0, 0)
            checks.append("different session permits new generation baseline")
            peer.disconnect()

            for label, body in (
                ("single quotes", b"{'version':{'major':1,'minor':0}}"),
                ("nonstandard apostrophe escape", b'{"data":"\\\'"}'),
                ("depth 65", b"[" * 65 + b"0" + b"]" * 65),
                ("duplicate key", b'{"version":1,"version":2}'),
            ):
                peer.negotiate(qmp, session)
                require(qmp.get("last-error") == "", "Malformed test inherited a preexisting error")
                peer.send_bytes(body)
                qmp.wait_phase({"error"})
                reason = qmp.get("last-error")
                require(("duplicate" in reason.lower()) if label == "duplicate key" else "strict" in reason,
                        f"Malformed {label} did not reach strict parser guard: {reason}")
                peer.disconnect()
                checks.append(label + " rejected")
            for field in ("session_id", "connection_id", "sequence", "reset_epoch", "topology_generation", "virtual_time_ns"):
                def mutate(record, name=field):
                    record[name] += "\n"
                    return record
                peer.bad_hello(qmp, session, 0, 0, mutate=mutate)
                checks.append(field + " final-LF rejected")

            for text, accepted in (("€" * 1024, True), ("😀" * 512, True), ("😀" * 513, False)):
                peer.negotiate(qmp, session)
                request = peer.arm(qmp)
                peer.send(peer.response(request, text=text))
                qmp.wait_phase({"resolved" if accepted else "blocked-error"})
                shared.assert_stopped(qmp, int(request["virtual_time_ns"]), 0.01)
                qmp.control("cancel")
                peer.disconnect()
                checks.append(f"UTF16 error limit: {len(text)} code points accepted={accepted}")

            peer.negotiate(qmp, session)
            stale_telemetry_base = int(qmp.get("late-replies"))
            for repeat in range(2):
                request = peer.arm(qmp)
                for _ in range(255):
                    peer.send(peer.response(request, ordinal=str(int(request["request_id"]) + 1)))
                peer.send(peer.response(request))
                qmp.wait_phase({"resolved"})
                require(int(qmp.get("late-replies")) == stale_telemetry_base + (repeat + 1) * 255,
                        "Cumulative discard telemetry unexpectedly reset")
                qmp.control("release")
                qmp.command("cont")
                qmp.wait_phase({"completed"})
                qmp.command("stop")
            checks.append("255 stale responses + valid response resets consecutive discard twice")
            request = peer.arm(qmp)
            for _ in range(256):
                peer.send(peer.response(request, ordinal=str(int(request["request_id"]) + 1)))
            qmp.wait_phase({"blocked-error"})
            require("limit exceeded" in qmp.get("last-error"), "256th stale record did not fail closed")
            shared.assert_stopped(qmp, int(request["virtual_time_ns"]), 0.01)
            checks.append("256 consecutive stale responses fail stopped")
            qmp.control("cancel")
            peer.disconnect()

            peer.negotiate(qmp, session)
            qmp.command("qom-set", {"path": "/objects/probe", "property": "watchdog-ms", "value": "200"})
            peer.socket.sendall(b"\x00\x00")
            qmp.wait_phase({"error"})
            require("Incomplete host frame" in qmp.get("last-error"), "Incomplete prefix lacked a bounded watchdog")
            peer.disconnect()
            checks.append("incomplete frame prefix watchdog")

            session = str(uuid.uuid4())
            peer.negotiate(qmp, session, 2**64 - 1, 2**64 - 1)
            qmp.command("system_reset")
            peer.disconnect()
            peer.bad_hello(qmp, session, 2**64 - 1, 2**64 - 1,
                           recovery_context=(str(uuid.uuid4()), 0, 0))
            session = str(uuid.uuid4())
            peer.negotiate(qmp, session)
            checks.append("MAX epoch system reset requires a different session")

            # Review tag773C regressions: a response for the fixed one-phase
            # request must carry exactly one phase result, and a mismatched
            # endpoint/controller/net context must fail the barrier instead
            # of resolving it.
            for label, mutate_data in (
                ("empty phase result count", lambda data: data.update({"phase_statuses": []})),
                ("wrong controller context", lambda data: data.update({"controller_id": "wrong"})),
                ("wrong endpoint context", lambda data: data.update({"endpoint_ids": ["wrong"]})),
                ("wrong net context", lambda data: data.update({"net_ids": ["wrong"]})),
            ):
                peer.negotiate(qmp, session)
                require(qmp.get("last-error") == "", "Next review case inherited an error")
                request = peer.arm(qmp)
                data = {key: request["data"][key] for key in ("bus", "controller_id", "endpoint_ids", "net_ids")}
                data.update({"phase_statuses": [{"index": 0, "status": "ack"}], "modeled_latency_ns": "0",
                             "accepted_length": 0, "payload_encoding": "base64", "payload": "Wg==",
                             "error_message": ""})
                mutate_data(data)
                peer.send(peer.envelope("response", ordinal=request["request_id"],
                                        virtual_ns=request["virtual_time_ns"], data=data))
                qmp.wait_phase({"blocked-error"})
                reason = qmp.get("last-error")
                require("phase result count" in reason if label.startswith("empty")
                        else "context" in reason,
                        f"{label} was not rejected with its own reason: {reason}")
                shared.assert_stopped(qmp, int(request["virtual_time_ns"]), 0.01)
                qmp.control("release", expect_error=True)
                shared.assert_stopped(qmp, int(request["virtual_time_ns"]), 0.01)
                qmp.control("cancel")
                peer.disconnect()
                checks.append(label + " rejected before resolution")

            peer.negotiate(qmp, session)
            peer.socket.sendall(struct.pack(">I", 0x00F00000))
            qmp.wait_phase({"error"})
            require("Oversized" in qmp.get("last-error"),
                    f"Oversize length prefix lacked its bounded rejection: {qmp.get('last-error')}")
            peer.disconnect()
            checks.append("oversize length prefix rejected")

            peer.negotiate(qmp, session)
            request = peer.arm(qmp)
            full = json.dumps(peer.response(request), ensure_ascii=False,
                              separators=(",", ":")).encode()
            # Declare the full body length but hold back the final byte; the
            # assembly watchdog must bound the wait and fail the barrier.
            wire = len(full).to_bytes(4, "big") + full[:-1]
            peer.sent.write(wire)
            peer.sent.flush()
            peer.socket.sendall(wire)
            qmp.wait_phase({"blocked-error"})
            # With a pending request the host watchdog (armed at request
            # send) bounds the wait no later than the assembly watchdog;
            # either path must fail the barrier closed.
            require("watchdog" in qmp.get("last-error"),
                    f"Truncated reply body was not bounded by a watchdog: {qmp.get('last-error')}")
            shared.assert_stopped(qmp, int(request["virtual_time_ns"]), 0.01)
            qmp.control("cancel")
            peer.disconnect()
            checks.append("truncated reply body rejected by watchdog")

            # Reset while the dependency is armed but its deadline has not
            # fired: the stale timer must be cancelled, step flags stay
            # clean, and nothing may auto-resume the machine.
            peer.negotiate(qmp, session)
            qmp.command("stop")
            qmp.control("arm:10000000")
            stale_armed = int(qmp.get("armed-ns"))
            require(qmp.get("resume-blocked") is False, "Arm must not own the barrier")
            previous_events = len(qmp.events)
            qmp.command("system_reset")
            require(any(event["event"] == "RESET" for event in qmp.events[previous_events:]),
                    "RESET event missing after armed reset")
            frozen = int(qmp.get("virtual-ns"))
            require(qmp.get("armed-ns") == "-1", "Reset kept the armed deadline")
            require(qmp.get("resume-blocked") is False, "Reset kept barrier ownership")
            require(qmp.get("cpu-step-flags") == "0:0,1:0", "Reset disturbed CPU step flags")
            # A reset from paused lands in RUN_STATE_PRELAUNCH in this tree
            # (system/runstate.c re-runs resume_all_vcpus then set PRELAUNCH);
            # either way the machine must not be running.
            status = qmp.command("query-status")
            require(status["status"] in ("paused", "prelaunch") and status["running"] is False,
                    f"Reset auto-resumed the machine: {status}")
            end = time.monotonic() + 0.2
            while time.monotonic() < end:
                later = qmp.command("query-status")
                require(later["status"] in ("paused", "prelaunch") and later["running"] is False,
                        f"Machine resumed while parked after reset: {later}")
                require(int(qmp.get("virtual-ns")) == frozen, "Clock moved while parked after reset")
                time.sleep(0.05)
            checks.append("reset while armed clears deadline and ownership while paused")

            peer.disconnect()
            peer.negotiate(qmp, session, 1)
            qmp.command("cont")
            stale_frame = None
            try:
                peer.socket.settimeout(4)
                stale_frame = peer.read()
            except (socket.timeout, OSError):
                stale_frame = None
            require(stale_frame is None,
                    f"Stale armed timer produced a request after reset: {str(stale_frame)[:160]}")
            end = time.monotonic() + 10
            while time.monotonic() < end:
                if int(qmp.get("virtual-ns")) > stale_armed:
                    break
                time.sleep(0.02)
            require(int(qmp.get("virtual-ns")) > stale_armed, "Guest clock stalled after armed reset")
            require(qmp.get("phase") in ("disconnected", "handshaking", "ready"),
                    f"Unexpected phase after the stale deadline passed: {qmp.get('phase')}")
            qmp.command("stop")
            checks.append("stale armed timer did not fire after reset")

            request = peer.arm(qmp)
            peer.send(peer.response(request))
            qmp.wait_phase({"resolved"})
            qmp.control("release")
            qmp.command("cont")
            qmp.wait_phase({"completed"})
            qmp.command("stop")
            checks.append("post-armed-reset barrier completes via release+cont")

            result = {"passed": True, "checks": checks, "artifacts": str(directory),
                      "native_peripheral_support": False, "scope": "actual QEMU framed probe fault tests"}
            (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result), flush=True)
            return 0
        except Exception as error:
            result = {"passed": False, "completed_checks": checks, "error": str(error), "artifacts": str(directory)}
            (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result), flush=True)
            return 1
        finally:
            if qmp:
                try:
                    qmp.command("quit")
                except (AssertionError, OSError):
                    pass
                qmp.close()
            if process:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            peer.close()
            errors.close()


if __name__ == "__main__":
    sys.exit(main())
