"""Host-only unit seams; synthetic parser records are NOT runtime evidence."""
from collections import Counter
from fractions import Fraction
import unittest

import expected_vectors as vectors
import graph_vectors as graphs
import run_fixture as runner


def synthetic_transcript():
    """Construct parser inputs mathematically, never claim firmware observations."""
    lines = []
    for row in vectors.matrix():
        fields = dict(row, mask=f"{row['mask']:x}")
        lines.append("I2S_CASE " + " ".join(f"{key}={value}" for key, value in fields.items()))
        receivers = (1 - row["source"],) if row["mode"] == "pdm" else (0, 1)
        phases = (("sustained", 8),) if row["mode"] == "pdm" else (("sustained", 8), ("starved", 4), ("restart", 4))
        for phase, rounds in phases:
            for receiver in receivers:
                capture = vectors.expected_capture(row, receiver, 17, rounds)
                lines.append(f"I2S_OBS case={row['case']} phase={phase} controller={receiver} offset=17 expected={capture['hash']} observed={capture['hash']} bytes={capture['bytes']} frames={capture['frames']} match={rounds}/{rounds} txeof={rounds} rxeof={rounds} rxovf={int(phase == 'starved')}")
            cadence = vectors.cadence(row)
            elapsed_us = cadence["frame_us"] * 3 * vectors.FRAMES
            lines.append(f"I2S_CADENCE case={row['case']} phase={phase} frame_hz={cadence['frame_hz']} interval_ns={cadence['interval_ns']} eof_frames={vectors.FRAMES} intervals=3 elapsed_us={int(elapsed_us)}")
        if row["mode"] != "pdm":
            for receiver in (0, 1):
                lines.append(f"I2S_DISABLED case={row['case']} controller={receiver} read=259 write=259 frozen=1")
    for controller in (0, 1):
        for direction in ("tx", "rx"):
            for fmt in ("raw", "pcm"):
                status = 262 if controller == 1 and fmt == "pcm" else 0
                lines.append(f"I2S_CAP controller={controller} direction={direction} format={fmt} expected={status} observed={status}")
            for bits in (24, 32):
                lines.append(f"I2S_TDM_LIMIT controller={controller} direction={direction} bits={bits} slots=8 expected=258 observed=258")
    lines.append("I2S_NATIVE_DONE cases=124 failures=0")
    return "\n".join(lines) + "\n"


class VectorTests(unittest.TestCase):
    def test_matrix_exact_coverage(self):
        rows = vectors.matrix()
        self.assertEqual(len(rows), 124)
        self.assertEqual([row["case"] for row in rows], list(range(124)))
        self.assertEqual(Counter(row["mode"] for row in rows), {"std": 48, "tdm": 72, "pdm": 4})
        signatures = {tuple(sorted({key: value for key, value in row.items() if key != "case"}.items())) for row in rows}
        self.assertEqual(len(signatures), len(rows))
        for row in rows:
            if row["mode"] == "tdm":
                self.assertEqual(row["mask"], row["mask"] & (0xff if row["bits"] <= 16 else 0x0f))
                self.assertLessEqual(row["bits"] * (8 if row["bits"] <= 16 else 4), 128)
        self.assertEqual({(row["source"], row["master"]) for row in rows if row["mode"] == "pdm"}, {(0, 0), (0, 1), (1, 0), (1, 1)})

    def test_packed_sample_bytes_and_order(self):
        for controller in (0, 1):
            for bits in vectors.WIDTHS:
                for mask in (1, 3, 0x81, 0x55, 0xff, 0x09, 0x05, 0x0f):
                    actual = vectors.vector(controller, bits, mask)
                    expected = bytearray()
                    for frame in range(96):
                        for slot in range(8):
                            if mask & (1 << slot):
                                value = ((73 * frame + 29 * slot + 101 * controller + 17) ^ (0x9e3779b9 * (1 + frame + slot))) & ((1 << bits) - 1)
                                expected.extend((value >> shift) & 255 for shift in range(0, bits, 8))
                    self.assertEqual(actual, bytes(expected))
                    self.assertEqual(len(actual), 96 * mask.bit_count() * bits // 8)

    def test_fnv_known_vectors(self):
        self.assertEqual(vectors.fnv1a(b""), "811c9dc5")
        self.assertEqual(vectors.fnv1a(b"a"), "e40c292c")
        self.assertEqual(vectors.fnv1a(b"foobar"), "bf9cf968")

    def test_capture_source_and_frame_rotation(self):
        for row in vectors.matrix():
            receiver = 1 - row.get("source", 0)
            source = row.get("source", 1 - receiver)
            payload = vectors.vector(source, row["bits"], row["mask"])
            stride = row["mask"].bit_count() * row["bits"] // 8
            for phase in (0, 1, 95):
                wanted = payload[phase * stride:] + payload[:phase * stride]
                actual = vectors.expected_capture(row, receiver, phase, 2)
                self.assertEqual(actual, dict(hash=vectors.fnv1a(wanted * 2), bytes=len(payload) * 2, frames=192))
        for phase in (-1, 96):
            with self.assertRaises(ValueError):
                vectors.expected_capture(vectors.matrix()[0], 0, phase, 1)

    def test_rational_cadence(self):
        for row in vectors.matrix():
            ref = vectors.cadence(row)
            pdm = row["mode"] == "pdm"
            self.assertEqual(ref["frame_hz"], "62500/1" if pdm else "12500/1")
            self.assertEqual(ref["interval_ns"], "16000/1" if pdm else "80000/1")
            self.assertEqual(ref["frame_us"], Fraction(16 if pdm else 80))
            slots = (8 if row["bits"] <= 16 else 4) if row["mode"] == "tdm" else 2
            self.assertEqual(ref["bclk_hz"], f"{2000000 if pdm else 12500 * row['bits'] * slots}/1")


class GraphTests(unittest.TestCase):
    def test_connected_and_disconnected_topology(self):
        for scenario in ("connected", "disconnected-data"):
            document = graphs.project(scenario)
            self.assertEqual(document["version"], 3)
            ids = [component["id"] for component in document["components"]]
            self.assertEqual(len(ids), len(set(ids)))
            terminals = {terminal["id"] for component in document["components"] for terminal in component["terminals"]}
            endpoints = [endpoint for net in document["nets"] for endpoint in net["endpoints"]]
            self.assertEqual(len(endpoints), len(set(endpoints)))
            self.assertTrue(set(endpoints) <= terminals)
            nets = {net["id"]: set(net["endpoints"]) for net in document["nets"]}
            self.assertEqual(nets["bclk"], {"U1.io18", "U1.io4", "Idlebclk.a"})
            self.assertEqual(nets["ws"], {"U1.io19", "U1.io5", "Idlews.a"})
            self.assertTrue({"V.n", "G.ref", "U1.gnd", "Idlebclk.b", "Idlews.b"} <= nets["gnd"])
            self.assertEqual(nets["vdd"], {"V.p", "U1.vdd"})
            if scenario == "connected":
                self.assertEqual(nets["data20"], {"U1.io20", "U1.io7"})
                self.assertEqual(nets["data6"], {"U1.io6", "U1.io21"})
            else:
                for source, destination in ((20, 7), (6, 21)):
                    self.assertEqual(nets[f"tx{source}"], {f"U1.io{source}"})
                    self.assertEqual(nets[f"rx{destination}"], {f"U1.io{destination}", f"Pull{destination}.a"})
                    self.assertIn(f"Pull{destination}.b", nets["gnd"])

    def test_unqualified_and_unknown_scenarios_rejected(self):
        with self.assertRaisesRegex(ValueError, "UNQUALIFIED"):
            graphs.project("external-peer")
        with self.assertRaisesRegex(ValueError, "unknown scenario"):
            graphs.project("unknown")


class ParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.transcript = synthetic_transcript()

    def test_records_ignore_unrelated_lines(self):
        self.assertEqual(runner.records("noise key=3\nI2S_OBS case=0 observed=deadbeef\n", "I2S_OBS"), [{"case": "0", "observed": "deadbeef"}])
        self.assertEqual(runner.records("I2S_OBSERVED case=0", "I2S_OBS"), [])

    def test_synthetic_complete_contract(self):
        self.assertEqual(runner.validate(self.transcript), dict(cases=124, exact_sample_records=724, cadence_records=364, capability_records=8, frame_limit_records=8, disable_records=240))

    def test_missing_or_duplicate_sample_rejected(self):
        lines = self.transcript.splitlines()
        observation = next(line for line in lines if line.startswith("I2S_OBS "))
        for text in (self.transcript.replace(observation + "\n", "", 1), self.transcript + observation + "\n"):
            with self.assertRaises(AssertionError):
                runner.validate(text)

    def test_mutated_contracts_rejected(self):
        mutations = (("observed=", "observed=deadbeef", "observed sample hash mismatch"),
                     ("rxovf=1", "rxovf=0", "no genuine starvation overflow"),
                     ("elapsed_us=23040", "elapsed_us=1", "DMA EOF cadence mismatch"),
                     ("read=259", "read=0", "disable/restart contract failed"),
                     ("format=pcm expected=262 observed=262", "format=pcm expected=262 observed=0", "controller capability mismatch"),
                     ("slots=8 expected=258 observed=258", "slots=8 expected=258 observed=0", "oversized TDM frame"),
                     ("failures=0", "failures=1", "firmware reported failures"))
        for old, new, message in mutations:
            with self.subTest(mutation=old):
                self.assertIn(old, self.transcript)
                with self.assertRaisesRegex(AssertionError, message):
                    runner.validate(self.transcript.replace(old, new, 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
