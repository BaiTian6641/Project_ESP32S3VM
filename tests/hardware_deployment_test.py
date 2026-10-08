"""Recovery control tests use mocks and never open a serial port."""
import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("hardware_deployment", Path(__file__).resolve().parents[1] / "tools/run-hardware-reference.py")
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)


class RecoveryTests(unittest.TestCase):
    @staticmethod
    def programmed(calls, name, mark_mutation):
        mark_mutation()
        calls.append("flash:" + name)

    def test_success_restores_once_after_all_captures(self):
        calls, record = [], {"captures": {}}
        deployment.execute_with_restore(["simd", "wifi"], lambda name, mark: self.programmed(calls, name, mark),
            lambda name: calls.append("capture:" + name), lambda: calls.append("restore"),
            lambda: calls.append("verify"), record)
        self.assertEqual(calls, ["flash:simd", "capture:simd", "flash:wifi", "capture:wifi", "restore", "verify"])
        self.assertEqual(record["restoration"], "verified")

    def test_failed_partial_flash_still_restores(self):
        calls, record = [], {"captures": {}}
        def failed(name, mark_mutation):
            mark_mutation()
            calls.append("partial-flash")
            raise RuntimeError("write interrupted")
        with self.assertRaisesRegex(RuntimeError, "write interrupted"):
            deployment.execute_with_restore(["simd"], failed, lambda name: calls.append("capture"),
                lambda: calls.append("restore"), lambda: calls.append("verify"), record)
        self.assertEqual(calls, ["partial-flash", "restore", "verify"])
        self.assertEqual(record["restoration"], "verified")

    def test_cancelled_capture_still_restores(self):
        calls, record = [], {"captures": {}}
        def cancelled(name):
            raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            deployment.execute_with_restore(["simd"], lambda name, mark: self.programmed(calls, name, mark), cancelled,
                lambda: calls.append("restore"), lambda: calls.append("verify"), record)
        self.assertEqual(calls, ["flash:simd", "restore", "verify"])

    def test_restore_error_remains_an_active_failure(self):
        record = {"captures": {}}
        def failed_restore():
            raise RuntimeError("restore needs recovery")
        with self.assertRaisesRegex(RuntimeError, "restore needs recovery"):
            deployment.execute_with_restore(["simd"], lambda name, mark: mark(), lambda name: None,
                failed_restore, lambda: self.fail("verification cannot run"), record)
        self.assertEqual(record["restoration"], "attempting")

    def test_restore_waiver_leaves_last_fixture_after_all_captures(self):
        calls, record = [], {"captures": {}}
        deployment.execute_with_restore(
            ["simd", "wifi", "ble"],
            lambda name, mark: self.programmed(calls, name, mark),
            lambda name: calls.append("capture:" + name),
            lambda: self.fail("Restoration was explicitly waived"),
            lambda: self.fail("No restored image exists to verify"),
            record, no_restore=True)
        self.assertEqual(record["mutation_attempted"], ["simd", "wifi", "ble"])
        self.assertEqual(record["restoration"], "waived")
        self.assertEqual(calls[-2:], ["flash:ble", "capture:ble"])

    def test_restore_waiver_never_suppresses_capture_failure_or_cancellation(self):
        for failure in (RuntimeError("capture failed"), KeyboardInterrupt()):
            with self.subTest(failure=type(failure).__name__):
                record = {"captures": {}}
                def failed_capture(name):
                    raise failure
                with self.assertRaises(type(failure)) as raised:
                    deployment.execute_with_restore(
                        ["simd"], lambda name, mark: mark(), failed_capture,
                        lambda: self.fail("Restoration was explicitly waived"),
                        lambda: self.fail("No restoration to verify"),
                        record, no_restore=True)
                self.assertIs(raised.exception, failure)
                self.assertEqual(record["restoration"], "waived")
                self.assertEqual(record["mutation_attempted"], ["simd"])
                self.assertEqual(record["captures"], {})

    def test_restore_waiver_preserves_preflight_no_mutation_boundary(self):
        record = {"captures": {}}
        def rejected_flash(name, mark):
            raise RuntimeError("current flash differs")
        with self.assertRaisesRegex(RuntimeError, "current flash differs"):
            deployment.execute_with_restore(
                ["simd"], rejected_flash, lambda name: self.fail("No write occurred"),
                lambda: self.fail("A stale backup must not be restored"),
                lambda: self.fail("No restoration to verify"),
                record, no_restore=True)
        self.assertEqual(record["restoration"], "not-needed")
        self.assertNotIn("mutation_attempted", record)


class FakeChip:
    CHIP_NAME = "ESP32-S3"
    def __init__(self, api, mac=(1, 2, 3, 4, 5, 6), encrypted=False):
        self.api, self.mac, self.encrypted = api, mac, encrypted
        self._port = object()
    def __enter__(self):
        return self
    def __exit__(self, *args):
        self.api.events.append(("close", self))
    def read_mac(self):
        return self.mac
    def get_chip_revision(self):
        return 2
    def get_crystal_freq(self):
        return 40
    def get_security_info(self):
        return {"parsed_flags": {"SECURE_BOOT_EN": False}, "flash_crypt_cnt": 1 if self.encrypted else 0}
    def flash_id(self):
        return 0x1840c8


class FakeAPI:
    def __init__(self):
        self.events, self.connections = [], []
        self.next_mac = (1, 2, 3, 4, 5, 6)
        self.after_verify = None
    def connect_esp(self, **kwargs):
        chip = FakeChip(self, self.next_mac)
        self.events.append(("connect", chip, kwargs))
        self.connections.append(chip)
        return chip
    def run_stub(self, chip):
        self.events.append(("stub", chip))
        return chip
    def attach_flash(self, chip):
        self.events.append(("attach", chip))
    def verify_flash(self, chip, files):
        self.events.append(("verify", chip, files))
        if self.after_verify:
            self.after_verify(chip)
    def write_flash(self, chip, files, **kwargs):
        if chip.WRITE_FLASH_ATTEMPTS != 1:
            raise AssertionError("Automatic unidentified port-reopen retries must be disabled")
        self.events.append(("write", chip, files, kwargs))
    def reset_chip(self, chip, mode):
        self.events.append(("reset", chip, mode))


def prepared_identity():
    return {"hardware_id_sha256": hashlib.sha256(b"01:02:03:04:05:06").hexdigest(),
            "board": {"flash_bytes": 16777216},
            "files": {"backup": {"sha256": hashlib.sha256(b"original").hexdigest()}}}


class IdentityAndQuiescenceTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parents[1] / "build-hardware-host/deployment-tests"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=test_root)
        self.assertTrue(Path(self.temporary.name).absolute().is_relative_to(test_root.absolute()))
        self.addCleanup(self.temporary.cleanup)
        self.api = FakeAPI()
        self.paths = {"backup": Path("original.bin"), "simd": Path("simd.bin"), "wifi": Path("wifi.bin")}
        with mock.patch.object(deployment, "freeze_flash_bytes",
                               return_value={"backup": b"original", "simd": b"simd", "wifi": b"wifi"}):
            self.operations = deployment.HardwareOperations(prepared_identity(), self.paths,
                                                            Path(self.temporary.name), self.api)

    def test_verify_and_first_write_use_one_identified_quiescent_connection(self):
        with mock.patch.object(deployment, "validate"):
            self.operations.flash("simd", lambda: None)
        events = self.api.events
        verify = next(item for item in events if item[0] == "verify")
        write = next(item for item in events if item[0] == "write")
        reset = next(item for item in events if item[0] == "reset")
        self.assertIs(verify[1], write[1])
        self.assertLess(events.index(verify), events.index(write))
        self.assertLess(events.index(write), events.index(reset))
        self.assertEqual(sum(item[0] == "reset" for item in events), 1)
        self.assertEqual(events[0][2]["port"], "COM5")
        self.assertEqual(events[0][2]["before"], "usb-reset")

    def test_reassignment_after_verification_is_rejected_before_programming(self):
        self.api.after_verify = lambda chip: setattr(chip, "mac", (9, 8, 7, 6, 5, 4))
        with mock.patch.object(deployment, "validate"), self.assertRaisesRegex(RuntimeError, "identity differs"):
            self.operations.flash("simd", lambda: None)
        self.assertFalse(any(item[0] == "write" for item in self.api.events))

    def test_each_later_write_rechecks_the_new_connection(self):
        with mock.patch.object(deployment, "validate"):
            self.operations.flash("simd", lambda: None)
            self.api.next_mac = (9, 8, 7, 6, 5, 4)
            with self.assertRaisesRegex(RuntimeError, "identity differs"):
                self.operations.flash("wifi", lambda: None)
        self.assertEqual(sum(item[0] == "write" for item in self.api.events), 1)

    def test_restore_rejects_different_board_without_writing(self):
        self.api.next_mac = (9, 8, 7, 6, 5, 4)
        with mock.patch.object(deployment, "sha", return_value="frozen"), self.assertRaisesRegex(RuntimeError, "identity differs"):
            self.operations.restore()
        self.assertFalse(any(item[0] == "write" for item in self.api.events))

    def test_restore_verifies_before_reset_on_same_connection(self):
        with mock.patch.object(deployment, "sha", return_value="frozen"):
            self.operations.restore()
        kinds = [item[0] for item in self.api.events]
        self.assertLess(kinds.index("write"), kinds.index("verify"))
        self.assertLess(kinds.index("verify"), kinds.index("reset"))
        write = next(item for item in self.api.events if item[0] == "write")
        verify = next(item for item in self.api.events if item[0] == "verify")
        self.assertIs(write[1], verify[1])

    def test_security_or_stub_connection_change_prevents_write(self):
        chip = FakeChip(self.api, encrypted=True)
        with self.assertRaisesRegex(RuntimeError, "security state"):
            deployment.check_live_identity(chip, prepared_identity(), security=True)
        other = FakeChip(self.api)
        with mock.patch.object(deployment, "validate"), mock.patch.object(self.api, "run_stub", return_value=other):
            with self.assertRaisesRegex(RuntimeError, "changed serial connection"):
                self.operations.flash("simd", lambda: None)
        self.assertFalse(any(item[0] == "write" for item in self.api.events))

    def test_original_verification_failure_does_not_restore_stale_backup(self):
        record = {"captures": {}}
        with mock.patch.object(deployment, "validate"), \
             mock.patch.object(self.api, "verify_flash", side_effect=RuntimeError("original flash drifted")), \
             mock.patch.object(self.operations, "restore") as restore:
            with self.assertRaisesRegex(RuntimeError, "original flash drifted"):
                deployment.execute_with_restore(["simd"], self.operations.flash,
                    lambda name: self.fail("capture before write"), restore, lambda: None, record)
        restore.assert_not_called()
        self.assertFalse(any(item[0] == "write" for item in self.api.events))
        self.assertEqual(record["restoration"], "not-needed")
        self.assertNotIn("mutation_attempted", record)

    def test_partial_real_operation_write_requires_identified_restore(self):
        record = {"captures": {}}
        actual_write = self.api.write_flash
        def interrupted_write(chip, files, **kwargs):
            actual_write(chip, files, **kwargs)
            if files[0][1] == b"simd":
                raise RuntimeError("write interrupted after erase")
        with mock.patch.object(deployment, "validate"), \
             mock.patch.object(deployment, "sha", return_value="frozen"), \
             mock.patch.object(self.api, "write_flash", side_effect=interrupted_write):
            with self.assertRaisesRegex(RuntimeError, "write interrupted after erase"):
                deployment.execute_with_restore(["simd"], self.operations.flash,
                    lambda name: self.fail("capture after failure"), self.operations.restore, lambda: None, record)
        writes = [item for item in self.api.events if item[0] == "write"]
        self.assertEqual(len(writes), 2)
        self.assertEqual(writes[1][2], [(0, b"original")])
        self.assertIsNot(writes[0][1], writes[1][1])
        self.assertEqual(record["mutation_attempted"], ["simd"])
        self.assertEqual(record["restoration"], "verified")

    def test_programming_uses_immutable_bytes_not_reopened_paths(self):
        with mock.patch.object(deployment, "validate"):
            self.operations.flash("simd", lambda: None)
        with mock.patch.object(deployment, "sha", side_effect=AssertionError("Do not reopen snapshot")):
            self.operations.restore()
        writes = [item for item in self.api.events if item[0] == "write"]
        self.assertEqual(writes[0][2], [(0, b"simd")])
        self.assertEqual(writes[1][2], [(0, b"original")])

    def test_freeze_rechecks_exact_file_bytes_and_size(self):
        image = Path(self.temporary.name) / "fixture.bin"
        original = b"\x12" * 4194304
        image.write_bytes(original)
        manifest = {"files": {"simd": {"sha256": hashlib.sha256(original).hexdigest()}}}
        frozen = deployment.freeze_flash_bytes(manifest, {"simd": image})
        image.write_bytes(b"\x13" * 4194304)
        self.assertEqual(frozen["simd"], original)
        with self.assertRaisesRegex(RuntimeError, "bytes differ"):
            deployment.freeze_flash_bytes(manifest, {"simd": image})
        image.write_bytes(original[:-1])
        with self.assertRaisesRegex(RuntimeError, "bytes differ"):
            deployment.freeze_flash_bytes(manifest, {"simd": image})


if __name__ == "__main__":
    unittest.main()
