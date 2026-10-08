"""Qualification comparisons reject incomplete captures and label state correctly."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPARATOR = ROOT / "qemu-extensions/prototypes/simd-pie/compare-captures.py"
spec = importlib.util.spec_from_file_location("capture_helpers", ROOT / "tests/hardware_reference_test.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


class SimdComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=helpers.TEST_TMP)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.rows = helpers.records()
        second = copy.deepcopy(self.rows[1])
        second.update(seq=2, case="second")
        self.rows.insert(2, second)
        self.rows[0]["expected_records"] = 2
        self.rows[-1].update(seq=3, records=2)
        self.manifest = helpers.manifest()
        self.manifest.update(expected_records=2, expected_cases=["edge", "second"])
        self.manifest_path = self.root / "fixture.json"
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def capture(self, name, rows):
        path = self.root / name
        helpers.ref.write_capture(path, helpers.raw(rows), "complete",
                                  self.manifest_path, {"transport": "local-test"})
        return path / "records.json"

    def compare(self, reference, candidate, baseline=None):
        output = self.root / (candidate.parent.name + "-comparison.json")
        command = [sys.executable, str(COMPARATOR), "--hw", str(reference),
                   "--qemu", str(candidate), "--output", str(output)]
        if baseline:
            command += ["--baseline", str(baseline)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        return result, output

    def test_incomplete_or_changed_capture_cannot_qualify_matching_vectors(self):
        for fault in ("missing-end", "unsuccessful", "duplicate-vector", "raw-change"):
            with self.subTest(fault=fault):
                reference = self.capture("reference-" + fault, self.rows)
                candidate = self.capture("candidate-" + fault, self.rows)
                data = json.loads(candidate.read_text(encoding="utf-8"))
                if fault == "missing-end":
                    data["records"].pop()
                elif fault == "unsuccessful":
                    data["successful"] = False
                elif fault == "duplicate-vector":
                    data["records"].insert(2, copy.deepcopy(data["records"][1]))
                else:
                    log = candidate.parent / "raw.log"
                    log.write_bytes(log.read_bytes() + b"unrecorded appended bytes\n")
                candidate.write_text(json.dumps(data), encoding="utf-8")
                result, output = self.compare(reference, candidate)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(output.exists(), "Invalid capture must not produce a qualification report")

    def test_difference_roles_preserve_reference_and_candidate_values(self):
        candidate_rows = copy.deepcopy(self.rows)
        candidate_rows[1]["after"]["q"] = "ff" + "00" * 127
        reference = self.capture("reference", self.rows)
        candidate = self.capture("candidate", candidate_rows)
        result, output = self.compare(reference, candidate)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(output.read_text(encoding="utf-8"))
        lane = report["vectors"][0]["diffs"][0]["lanes"][0]
        self.assertEqual(lane, {"byte": 0, "reference": "00", "candidate": "ff"})

    def test_remaining_baseline_difference_is_not_a_new_regression(self):
        baseline_rows = copy.deepcopy(self.rows)
        baseline_rows[1]["after"]["q"] = "ff" + "00" * 127
        candidate_rows = copy.deepcopy(baseline_rows)
        candidate_rows[2]["after"]["q"] = "ee" + "00" * 127
        reference = self.capture("reference", self.rows)
        baseline = self.capture("baseline", baseline_rows)
        candidate = self.capture("candidate", candidate_rows)
        result, output = self.compare(reference, candidate, baseline)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["mismatched_vectors"], 2)
        self.assertEqual(report["baseline_comparison"]["regressions_vs_hardware"], [2])


if __name__ == "__main__":
    unittest.main()
