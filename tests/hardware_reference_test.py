"""Malformed/partial evidence cannot pass; byte differences retain both captures."""
import copy
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TEST_TMP = ROOT / "tests/firmware/simd_reference/build/host-tests"
TEST_TMP.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("hardware_reference", ROOT / "tools/hardware-reference.py")
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)


def records():
    state = {key: "00" * size for key, size in {"q": 128, "qacc": 40, "aux": 36, "ar_deltas": 8}.items()}
    return [
        {"schema": 1, "type": "begin", "seq": 0, "fixture": "simd_reference", "seed": 7,
         "expected_records": 1, "source_sha256": "11" * 32, "elf_digest": "22" * 32},
        {"schema": 1, "type": "vector", "seq": 1, "case": "edge", "operation": "mv.qr",
         "vector_seed": 7, "input_q": "00" * 128, "input_qacc": "00" * 40,
         "before": copy.deepcopy(state), "after": copy.deepcopy(state)},
        {"schema": 1, "type": "end", "seq": 2, "records": 1, "complete": True}]


def raw(rows):
    return b"ROM boot and normal firmware diagnostics\n" + b"".join(
        ref.PREFIX + json.dumps(row, separators=(",", ":")).encode() + b"\n" for row in rows)


def manifest():
    return {"schema_version": 1, "fixture": "simd_reference", "source_sha256": "11" * 32,
            "expected_records": 1, "artifacts": {"flash_sha256": "33" * 32, "elf_sha256": "22" * 32}, "expected_cases": ["edge"]}


class ReferenceTests(unittest.TestCase):
    def test_valid_batch_ignores_rom_but_keeps_records(self):
        result = ref.parse_log(raw(records()), manifest())
        self.assertTrue(result["successful"])
        self.assertEqual(len(result["records"]), 3)

    def test_partial_missing_reordered_duplicate_and_count_are_not_complete(self):
        cases = [records()[:-1], records()[1:], records() + records(), records()]
        cases[-1][1]["seq"] = 9
        wrong_count = records()
        wrong_count[0]["expected_records"] = 2
        cases.append(wrong_count)
        for rows in cases:
            with self.subTest(rows=rows):
                self.assertFalse(ref.parse_log(raw(rows))["complete"])

    def test_strict_json_state_and_boolean_integer_rejection(self):
        self.assertFalse(ref.parse_log(ref.PREFIX + b'{"schema":1,"schema":1}\n')["complete"])
        self.assertFalse(ref.parse_log(ref.PREFIX + b'{"schema":NaN}\n')["complete"])
        for field in ["schema", "seq", "expected_records"]:
            bad = records()
            bad[0][field] = True
            self.assertFalse(ref.parse_log(raw(bad))["complete"])
        bad = records()
        bad[1]["after"]["q"] = "00"
        self.assertFalse(ref.parse_log(raw(bad))["complete"])
        bad = records()
        bad[2]["records"] = True
        self.assertFalse(ref.parse_log(raw(bad))["complete"])

    def test_manifest_identity_and_catalogue_must_match(self):
        for field, value in [("source_sha256", "ff" * 32), ("fixture", "radio_init"),
                             ("expected_cases", ["other"]), ("mode", "ble"),
                             ("artifacts", {"elf_sha256": "ff" * 32})]:
            wrong = manifest()
            wrong[field] = value
            self.assertFalse(ref.parse_log(raw(records()), wrong)["complete"])

    def test_radio_failure_can_be_complete_but_is_not_successful(self):
        head = records()[0]
        head.update(fixture="radio_init", expected_records=2)
        events = [{"schema": 1, "type": "event", "seq": 1, "stage": "esp_wifi_init",
                   "phase": "start", "attempted": True, "err": 0, "status": "enter"},
                  {"schema": 1, "type": "event", "seq": 2, "stage": "esp_wifi_init",
                   "phase": "result", "attempted": True, "err": 1, "status": "error"}]
        end = records()[2]
        end.update(seq=3, records=2)
        result = ref.parse_log(raw([head, *events, end]))
        self.assertTrue(result["complete"])
        self.assertFalse(result["successful"])
        self.assertEqual(result["last_stage"], "esp_wifi_init")
        events[1]["stage"] = "wrong_stage"
        self.assertFalse(ref.parse_log(raw([head, *events, end]))["complete"])

    def test_serial_open_is_explicit_and_lines_set_before_open(self):
        actions = []
        class FakeSerial:
            def __init__(self, **options):
                self.options = options
            def __setattr__(self, key, value):
                if key in {"dtr", "rts", "port"}:
                    actions.append((key, value))
                object.__setattr__(self, key, value)
            def open(self):
                actions.append(("open", self.port))
                self.assert_options = self.options
            def read(self, count):
                return raw(records())[:count]
            def close(self):
                actions.append(("close", None))
        captured, reason = ref.capture_serial("EXPLICIT_TEST_PORT", 115200, 1, 8192, serial_factory=FakeSerial)
        self.assertEqual(reason, "complete")
        self.assertTrue(ref.parse_log(captured)["complete"])
        self.assertEqual(actions[:4], [("dtr", False), ("rts", False), ("port", "EXPLICIT_TEST_PORT"), ("open", "EXPLICIT_TEST_PORT")])
        self.assertEqual(actions[-1], ("close", None))

    def test_serial_cancel_and_byte_limit_preserve_partial_data(self):
        class FakeSerial:
            def __init__(self, **unused): pass
            def open(self): pass
            def close(self): self.closed = True
            def read(self, count): return b"x" * count
        captured, reason = ref.capture_serial("EXPLICIT_TEST_PORT", 115200, 1, 7, serial_factory=FakeSerial)
        self.assertEqual((len(captured), reason), (7, "byte-limit"))
        stop = threading.Event()
        stop.set()
        captured, reason = ref.capture_serial("EXPLICIT_TEST_PORT", 115200, 1, 7, serial_factory=FakeSerial, cancelled=stop)
        self.assertEqual((captured, reason), (b"", "cancelled"))

    def test_without_explicit_capture_and_port_cli_never_opens(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            mp = Path(tmp) / "manifest.json"
            mp.write_text(json.dumps(manifest()))
            with mock.patch.object(ref, "capture_serial") as serial:
                with self.assertRaises(SystemExit):
                    ref.main(["--capture", "--manifest", str(mp), "--output", str(Path(tmp) / "out")])
                serial.assert_not_called()

    def test_journal_durable_chunks_survive_serial_error_and_midstream_cancel(self):
        for mode in ["error", "cancel"]:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
                root = Path(tmp)
                frozen = json.dumps(manifest()).encode()
                journal = ref.SerialJournal(root / "capture", frozen, {"transport": "mock"}, 1, 8192)
                chunk = raw(records()[:-1])
                stop = threading.Event()
                reads = []
                class FakeSerial:
                    def __init__(self, **unused): pass
                    def open(self):
                        self.closed = False
                        # Frozen identity precedes even the mocked port open.
                        self.identity = json.loads((journal.path / "journal.json").read_bytes())
                        reads.append(self)
                    def read(self, count):
                        if not hasattr(self, "read_once"):
                            self.read_once = True
                            return chunk
                        # The file is already flushed/fsynced before another read.
                        assert (journal.path / "raw.partial.log").read_bytes() == chunk
                        if mode == "error":
                            raise OSError("mock unplug")
                        stop.set()
                        return b"tail after cancellation\n"
                    def close(self): self.closed = True
                captured, reason = ref.capture_serial("MOCK_ONLY", 115200, 1, 8192,
                    serial_factory=FakeSerial, cancelled=stop, on_bytes=journal.append)
                info = journal.finish(captured, reason)
                self.assertTrue(reads[0].closed)
                self.assertFalse(reads[0].identity["complete"])
                self.assertEqual((journal.path / "raw.partial.log").read_bytes(), captured)
                self.assertEqual(info["raw_sha256"], ref.hashlib.sha256(captured).hexdigest())
                self.assertFalse(info["complete"])
                self.assertFalse(info["successful"])
                self.assertIn("mock unplug" if mode == "error" else "cancelled", reason)
                result = ref.write_capture(root / "capture", captured, reason, None,
                                           {"acquisition_journal": info}, frozen)
                self.assertFalse(result["complete"])
                with self.assertRaises(FileNotFoundError):
                    ref.compare(journal.path, root / "capture", root / "must-not-qualify")

    def test_complete_serial_cli_freezes_identity_and_keeps_journal_incomplete(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            root = Path(tmp)
            mp = root / "manifest.json"
            frozen = json.dumps(manifest()).encode()
            mp.write_bytes(frozen)
            output = root / "capture"
            original_capture = ref.capture_serial
            class FakeSerial:
                def __init__(self, **unused): pass
                def open(self):
                    assert (root / "capture.journal/manifest.json").read_bytes() == frozen
                    changed = manifest()
                    changed["source_sha256"] = "ff" * 32
                    mp.write_text(json.dumps(changed))
                def read(self, count): return raw(records())[:count]
                def close(self): pass
            def mocked_capture(*args, **kwargs):
                return original_capture(*args, serial_factory=FakeSerial, **kwargs)
            with mock.patch.object(ref, "inventory", return_value=[]), \
                    mock.patch.object(ref, "capture_serial", side_effect=mocked_capture), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = ref.main(["--capture", "--port", "MOCK_ONLY", "--manifest", str(mp),
                                 "--output", str(output), "--seconds", "1"])
            self.assertEqual(code, 0)
            self.assertEqual((output / "manifest.json").read_bytes(), frozen)
            metadata = json.loads((output / "metadata.json").read_bytes())
            identity = json.loads((root / "capture.journal/journal.json").read_bytes())
            stopped = json.loads((root / "capture.journal/stop.json").read_bytes())
            self.assertTrue(metadata["complete"])
            self.assertFalse(identity["complete"])
            self.assertFalse(stopped["complete"])
            self.assertEqual(metadata["raw_sha256"], stopped["raw_sha256"])
            self.assertEqual(metadata["manifest_sha256"], identity["manifest_sha256"])
            self.assertEqual(metadata["context"]["acquisition_journal"], stopped)

    def test_journal_bounds_collision_and_integrity_fail_before_qualification(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            root = Path(tmp)
            frozen = json.dumps(manifest()).encode()
            journal = ref.SerialJournal(root / "bounded", frozen, {}, 1, 7)
            class OverreadingMock:
                def __init__(self, **unused): pass
                def open(self): pass
                def read(self, count): return b"x" * 100
                def close(self): pass
            captured, reason = ref.capture_serial("MOCK_ONLY", 115200, 1, 7,
                serial_factory=OverreadingMock, on_bytes=journal.append)
            self.assertEqual((captured, reason), (b"x" * 7, "byte-limit"))
            self.assertEqual(journal.finish(captured, reason)["bytes"], 7)
            with self.assertRaises(FileExistsError):
                ref.SerialJournal(root / "bounded", frozen, {}, 1, 7)
            mp = root / "manifest.json"
            mp.write_bytes(frozen)
            with mock.patch.object(ref, "capture_serial") as capture, \
                    mock.patch.object(ref, "inventory", return_value=[]):
                with self.assertRaises(FileExistsError):
                    ref.main(["--capture", "--port", "MOCK_ONLY", "--manifest", str(mp),
                              "--output", str(root / "bounded")])
                capture.assert_not_called()
            damaged = ref.SerialJournal(root / "damaged", frozen, {}, 1, 7)
            damaged.append(b"data")
            (damaged.path / "manifest.json").write_bytes(b"tampered")
            with self.assertRaisesRegex(OSError, "integrity mismatch"):
                damaged.finish(b"data", "cancelled")
            self.assertFalse((root / "damaged").exists())

    def test_journal_sync_failure_never_finalizes_even_a_complete_raw_batch(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            root = Path(tmp)
            mp = root / "manifest.json"
            mp.write_text(json.dumps(manifest()))
            original_capture, original_append = ref.capture_serial, ref.SerialJournal.append
            class FakeSerial:
                def __init__(self, **unused): pass
                def open(self): pass
                def read(self, count): return raw(records())[:count]
                def close(self): pass
            def failed_sync(stream, data):
                stream.write(data)
                raise OSError("mock journal disk sync failed")
            def failing_append(journal, chunk):
                with mock.patch.object(ref, "durable_write", side_effect=failed_sync):
                    return original_append(journal, chunk)
            def mocked_capture(*args, **kwargs):
                return original_capture(*args, serial_factory=FakeSerial, **kwargs)
            with mock.patch.object(ref, "inventory", return_value=[]), \
                    mock.patch.object(ref, "capture_serial", side_effect=mocked_capture), \
                    mock.patch.object(ref.SerialJournal, "append", failing_append), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(OSError, "mock journal disk sync failed"):
                    ref.main(["--capture", "--port", "MOCK_ONLY", "--manifest", str(mp),
                              "--output", str(root / "capture"), "--seconds", "1"])
            journal = root / "capture.journal"
            self.assertTrue(ref.parse_log((journal / "raw.partial.log").read_bytes())["complete"])
            self.assertFalse(json.loads((journal / "journal.json").read_bytes())["complete"])
            self.assertFalse((root / "capture").exists())
            self.assertFalse((journal / "stop.json").exists())

    def test_forced_subprocess_kill_leaves_durable_incomplete_journal(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            root = Path(tmp)
            mp = root / "manifest.json"
            frozen = json.dumps(manifest()).encode()
            mp.write_bytes(frozen)
            output = root / "capture"
            ready = root / "mock-blocking-read"
            chunk = raw(records()[:-1])
            # The child imports no serial backend: its only stream is this mock.
            code = '''
import importlib.util, pathlib, sys, time
spec = importlib.util.spec_from_file_location('reference', sys.argv[1])
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)
chunk = bytes.fromhex(sys.argv[5])
class FakeSerial:
    def __init__(self, **unused): self.first = True
    def open(self): pass
    def close(self): pass
    def read(self, count):
        if self.first:
            self.first = False
            return chunk
        pathlib.Path(sys.argv[4]).write_text('mock only')
        time.sleep(60)
        return b''
original = ref.capture_serial
ref.capture_serial = lambda *a, **k: original(*a, serial_factory=FakeSerial, **k)
ref.inventory = lambda: []
ref.main(['--capture', '--port', 'MOCK_ONLY', '--manifest', sys.argv[2],
          '--output', sys.argv[3], '--seconds', '60'])
'''
            process = subprocess.Popen([sys.executable, "-B", "-u", "-c", code,
                str(ROOT / "tools/hardware-reference.py"), str(mp), str(output), str(ready), chunk.hex()],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists(), "Mock child did not reach its second read")
                process.kill()  # Intentionally skips Python finally/final capture.
                process.communicate(timeout=5)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)
            journal = root / "capture.journal"
            self.assertEqual((journal / "raw.partial.log").read_bytes(), chunk)
            self.assertEqual((journal / "manifest.json").read_bytes(), frozen)
            identity = json.loads((journal / "journal.json").read_bytes())
            self.assertFalse(identity["complete"])
            self.assertFalse(identity["successful"])
            self.assertEqual(identity["manifest_sha256"], ref.hashlib.sha256(frozen).hexdigest())
            self.assertFalse(output.exists())
            self.assertFalse((journal / "stop.json").exists())

    def test_byte_exact_comparator_preserves_mismatch_and_integrity(self):
        with tempfile.TemporaryDirectory(dir=TEST_TMP) as tmp:
            root = Path(tmp)
            mp = root / "manifest.json"
            mp.write_text(json.dumps(manifest()))
            ref.write_capture(root / "reference", raw(records()), "test", mp, {"transport": "test"})
            ref.write_capture(root / "same", raw(records()), "test", mp, {"transport": "test"})
            equal = ref.compare(root / "reference", root / "same", root / "equal")
            self.assertTrue(equal["equal"])
            changed = records()
            changed[1]["after"]["qacc"] = "01" + "00" * 39
            ref.write_capture(root / "changed", raw(changed), "test", mp, {"transport": "test"})
            mismatch = ref.compare(root / "reference", root / "changed", root / "mismatch")
            self.assertFalse(mismatch["equal"])
            self.assertEqual(mismatch["differences"][0]["candidate"]["after"]["qacc"], changed[1]["after"]["qacc"])
            self.assertTrue((root / "mismatch/candidate/raw.log").is_file())
            with self.assertRaises(FileExistsError):
                ref.compare(root / "reference", root / "same", root / "equal")
            with (root / "same/raw.log").open("ab") as stream:
                stream.write(b"tampered\n")
            integrity = ref.compare(root / "reference", root / "same", root / "integrity")
            self.assertFalse(integrity["equal"])


if __name__ == "__main__":
    unittest.main()
