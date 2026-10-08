#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests helper failure detection only; never counts as QEMU execution proof."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("hostbus_raw_test", Path(__file__).resolve().parents[1] / "raw-peer-test.py")
raw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(raw)


class ClosedEndpoint:
    closed = False

    def shutdown(self, how):
        raise OSError("Remote endpoint already closed")

    def close(self):
        self.closed = True


class QmpView:
    def __init__(self):
        self.ack_observed = False
        self.phase = "error"  # Deliberately inherited from an unrelated attempt.

    def wait_phase(self, phases):
        if not self.ack_observed:
            raise AssertionError("An inherited phase was used before observing a wire boundary")
        self.phase = "ready"

    def get(self, name):
        return False if name == "resume-blocked" else ""

    def log(self, direction, message):
        pass


class CapturedPeer(raw.RawPeer):
    def __init__(self, directory, qmp, candidate_accepted):
        super().__init__(directory)
        self.valid_context = ("11111111-1111-4111-8111-111111111111", 3, 7)
        self.qmp = qmp
        self.candidate_accepted = candidate_accepted
        self.frames = []

    def connect(self):
        pass

    def send(self, record):
        self.frames.append(record)

    def read(self):
        record = self.frames[0 if self.candidate_accepted else 1].copy()
        record["kind"] = "hello_ack"
        self.qmp.ack_observed = True
        return record

    def disconnect(self):
        pass


class HarnessTests(unittest.TestCase):
    def test_three_monotonic_pinned_core_heartbeats_are_required(self):
        text = "".join(f"ESP32S3VM_CORE_EXEC assigned={core} core={core} iteration={iteration} time_us={iteration * 20000}\n"
                       for iteration in (1, 2, 3) for core in (0, 1))
        evidence = raw.shared.core_heartbeats(text)
        self.assertTrue(raw.shared.cores_executed(evidence))
        self.assertFalse(raw.shared.cores_executed(raw.shared.core_heartbeats("")))

    def test_reported_chip_core_count_alone_is_not_execution_evidence(self):
        evidence = raw.shared.core_heartbeats("ESP32S3VM_BOOT_OK cores=2 flash=4194304\n")
        self.assertFalse(raw.shared.cores_executed(evidence))

    def test_wrong_task_affinity_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, "wrong core"):
            raw.shared.core_heartbeats("ESP32S3VM_CORE_EXEC assigned=0 core=1 iteration=1 time_us=20000\n")

    def test_stalled_iteration_and_time_are_rejected(self):
        first = "ESP32S3VM_CORE_EXEC assigned=0 core=0 iteration=1 time_us=20000\n"
        with self.assertRaisesRegex(AssertionError, "iteration did not progress"):
            raw.shared.core_heartbeats(first + first)
        with self.assertRaisesRegex(AssertionError, "virtual time did not progress"):
            raw.shared.core_heartbeats(first + "ESP32S3VM_CORE_EXEC assigned=0 core=0 iteration=2 time_us=20000\n")

    def test_incomplete_live_line_does_not_count_as_a_heartbeat(self):
        evidence = raw.shared.core_heartbeats("ESP32S3VM_CORE_EXEC assigned=0 core=0 iteration=3 time_us=60000")
        self.assertEqual(evidence["0"], [])
        self.assertFalse(raw.shared.cores_executed(evidence))

    def test_remote_shutdown_error_still_closes_socket_and_files(self):
        with tempfile.TemporaryDirectory() as directory:
            peer = raw.RawPeer(Path(directory))
            endpoint = ClosedEndpoint()
            peer.socket = endpoint
            peer.sent, peer.received = io.BytesIO(), io.BytesIO()
            peer.disconnect()
            self.assertIsNone(peer.socket)
            self.assertTrue(endpoint.closed)
            self.assertTrue(peer.sent.closed)
            self.assertTrue(peer.received.closed)
            peer.close()

    def test_inherited_error_cannot_hide_an_accepted_bad_hello(self):
        with tempfile.TemporaryDirectory() as directory:
            view = QmpView()
            peer = CapturedPeer(Path(directory), view, candidate_accepted=True)
            with self.assertRaisesRegex(AssertionError, "Invalid hello produced an ACK"):
                peer.bad_hello(view, peer.valid_context[0], 2, 7)
            peer.close()

    def test_good_ack_is_required_after_the_invalid_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            view = QmpView()
            peer = CapturedPeer(Path(directory), view, candidate_accepted=False)
            peer.bad_hello(view, peer.valid_context[0], 2, 7)
            self.assertEqual(len(peer.frames), 2)
            self.assertEqual(peer.frames[0]["reset_epoch"], "2")
            self.assertEqual(peer.frames[1]["reset_epoch"], "3")
            self.assertNotEqual(peer.frames[0]["connection_id"], peer.frames[1]["connection_id"])
            self.assertEqual(view.phase, "ready")
            peer.close()


if __name__ == "__main__":
    unittest.main()
