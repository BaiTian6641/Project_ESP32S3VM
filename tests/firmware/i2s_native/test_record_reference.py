"""Mathematical/synthetic host tests ONLY: never actual capture qualification.

These tests author bytes in temporary files and test reference arithmetic and
rejection seams. Passing them is NOT evidence that QEMU, GPIO, DMA, firmware,
external peers, or converters transported any real samples. Run separately
from an actual record_reference.py comparison against a closed native capture.
"""
from contextlib import redirect_stdout
from dataclasses import replace
from fractions import Fraction
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import expected_vectors as vectors
import record_reference as reference


def synthetic_events(row, controller=0, frames=96, phase=17, base_ns=1000000,
                     sequence=0, first_frame=0, dropped=()):
    """Test-only mathematical wire completions, independent affine sample math."""
    source = row.get("source", 1 - controller)
    slots = (8 if row["bits"] <= 16 else 4) if row["mode"] == "tdm" else 2
    active = [slot for slot in range(slots) if row["mask"] & (1 << slot)]
    period = Fraction(10 ** 9 * (16 if row["mode"] == "pdm" else 1), row["rate"])
    events = []
    for frame in range(frames):
        vector_frame = (phase + frame) % 96
        for slot in active:
            value = ((73 * vector_frame + 29 * slot + 101 * source + 17) ^
                     (0x9e3779b9 * (1 + vector_frame + slot))) & ((1 << row["bits"]) - 1)
            if row["mode"] == "pdm":
                when = base_ns + (frame + 1) * period
            else:
                when = base_ns + frame * period + (slot + 1) * period / slots
                if row["format"] == "philips":
                    when += period / (slots * row["bits"])
            encoded_frame = first_frame + frame + int(row["format"] == "philips" and slot == slots - 1)
            index = len(events)
            events.append(reference.RxEvent(int(when), sequence + index, encoded_frame,
                                            value, slot, row["bits"], 5 if index in dropped else 3))
    return events


def binary_capture(events, controller=0):
    # The test codec deliberately does not use the reader's Struct objects.
    return struct.pack("<8sII", b"S3I2SRX1", controller, 1) + b"".join(
        struct.pack("<QQQIHBB", event.timestamp_ns, event.sequence, event.frame,
                    event.sample, event.slot, event.width, event.flags) for event in events)


class CaptureTests(unittest.TestCase):
    def parse(self, raw):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.rx.bin"
            path.write_bytes(raw)
            return reference.read_capture(path)

    def capture(self, events, controller=0):
        return self.parse(binary_capture(events, controller))

    def std(self, frames=96, **kwargs):
        return synthetic_events(vectors.matrix()[1], frames=frames, **kwargs)

    def test_binary_layout_and_provenance(self):
        events = self.std()
        raw = binary_capture(events)
        self.assertEqual(raw[:16], b"S3I2SRX1\x00\x00\x00\x00\x01\x00\x00\x00")
        self.assertEqual(len(raw), 16 + 32 * len(events))
        got = self.parse(raw)
        self.assertEqual(got.controller, 0)
        self.assertEqual(got.events, tuple(events))
        self.assertEqual(got.sha256, hashlib.sha256(raw).hexdigest())
        with self.assertRaises(Exception):
            got.events[0].sample = 0

    def test_bad_header_and_empty_stream(self):
        raw = binary_capture(self.std())
        for broken in (b"", raw[:15], b"badmagic" + raw[8:],
                       raw[:8] + struct.pack("<II", 2, 1) + raw[16:],
                       raw[:8] + struct.pack("<II", 0, 2) + raw[16:], raw[:16]):
            with self.subTest(header=broken[:16]):
                with self.assertRaises(reference.RecordError):
                    self.parse(broken)

    def test_truncated_event_or_trailing_garbage(self):
        raw = binary_capture(self.std())
        for broken in (raw[:-1], raw + b"x", raw[:16 + 31]):
            with self.assertRaisesRegex(reference.RecordError, "truncated 32-byte"):
                self.parse(broken)

    def test_immutable_sequence_duplicate_decrease_gap_and_nonzero_origin(self):
        events = self.std()
        for sequence in (events[0].sequence, events[2].sequence + 1):
            broken = events[:]
            broken[1] = replace(broken[1], sequence=sequence)
            with self.assertRaisesRegex(reference.RecordError, "sequence"):
                self.capture(broken)
        broken = events[:]
        broken[2] = replace(broken[2], sequence=0)
        with self.assertRaisesRegex(reference.RecordError, "sequence"):
            self.capture(broken)
        with self.assertRaisesRegex(reference.RecordError, "sequence zero"):
            self.capture([replace(event, sequence=event.sequence + 1) for event in events])

    def test_timestamp_regression_rejected_but_equal_allowed(self):
        events = self.std()
        events[1] = replace(events[1], timestamp_ns=events[0].timestamp_ns - 1)
        with self.assertRaisesRegex(reference.RecordError, "decreasing timestamp"):
            self.capture(events)
        events[1] = replace(events[1], timestamp_ns=events[0].timestamp_ns)
        self.capture(events)  # Layout-valid only: cadence qualification still fails.

    def test_width_sample_slot_and_unknown_validity_rejected(self):
        events = self.std()
        changes = [dict(width=0), dict(width=12), dict(width=64),
                   dict(sample=1 << events[0].width), dict(slot=8)]
        changes.extend(dict(flags=flags) for flags in (0, 1, 2, 4, 6, 7, 11, 255))
        for change in changes:
            broken = events[:]
            broken[0] = replace(broken[0], **change)
            with self.subTest(change=change):
                with self.assertRaises(reference.RecordError):
                    self.capture(broken)

    def test_each_expected_row_has_exact_finite_mathematical_match(self):
        for row in vectors.matrix():
            receiver = 1 - row.get("source", 1)
            events = synthetic_events(row, controller=receiver)
            got = reference.qualify_segment(self.capture(events, receiver),
                                            reference.Segment(row["case"], 0, len(events)))
            with self.subTest(case=row["case"]):
                self.assertEqual(got["vector_phase"], 17)
                self.assertEqual(got["complete_frames"], 96)
                self.assertEqual(got["actual_samples"], 96 * row["mask"].bit_count())
                self.assertEqual(got["accepted_samples"], len(events))
                self.assertEqual(got["dropped_samples"], 0)
                self.assertEqual(Fraction(got["cadence"]["frame_interval_ns"]),
                                 Fraction(16000 if row["mode"] == "pdm" else 80000))

    def test_exact_samples_are_not_qualified_by_any_hash_or_callback(self):
        events = self.std()
        events[-1] = replace(events[-1], sample=events[-1].sample ^ 1)
        with self.assertRaisesRegex(reference.RecordError, "sample mismatch"):
            reference.qualify_segment(self.capture(events), reference.Segment(1, 0, len(events), phase=17))
        with self.assertRaisesRegex(reference.RecordError, "no unique exact"):
            reference.qualify_segment(self.capture(events), reference.Segment(1, 0, len(events)))

    def test_geometry_rejects_missing_and_duplicate_samples_even_after_renumbering(self):
        events = self.std()
        for changed in (events[:3] + events[4:], events[:3] + [events[3]] + events[3:]):
            # Conceal a sequence gap: geometry must independently catch it.
            changed = [replace(event, sequence=index) for index, event in enumerate(changed)]
            with self.assertRaisesRegex(reference.RecordError, "missing/reordered"):
                reference.qualify_segment(self.capture(changed), reference.Segment(1, 0, len(changed)))

    def test_selected_width_slot_mask_and_destination_are_required(self):
        events = self.std()
        for change in (dict(width=16), dict(slot=7)):
            changed = events[:]
            changed[0] = replace(changed[0], **change)
            with self.assertRaises(reference.RecordError):
                reference.qualify_segment(self.capture(changed), reference.Segment(1, 0, len(changed)))
        row = vectors.matrix()[120]
        events = synthetic_events(row, controller=1)
        with self.assertRaisesRegex(reference.RecordError, "destination"):
            reference.qualify_segment(self.capture(events, 0), reference.Segment(120, 0, len(events)))

    def test_reordered_physical_slots_are_not_relabelled_as_valid_samples(self):
        row = vectors.matrix()[50]
        events = synthetic_events(row)
        events[2], events[3] = (
            replace(events[3], timestamp_ns=events[2].timestamp_ns, sequence=2),
            replace(events[2], timestamp_ns=events[3].timestamp_ns, sequence=3))
        with self.assertRaisesRegex(reference.RecordError, "missing/reordered"):
            reference.qualify_segment(self.capture(events), reference.Segment(50, 0, len(events)))

    def test_initial_partial_philips_tail_is_omitted_not_an_actual_drop(self):
        # Row 0 stereo Philips: slot 1 completion is over the physical boundary.
        events = synthetic_events(vectors.matrix()[0])
        self.assertEqual((events[0].frame, events[0].slot), (0, 0))
        self.assertEqual((events[1].frame, events[1].slot), (1, 1))
        got = reference.qualify_segment(self.capture(events), reference.Segment(0, 0, len(events)))
        self.assertEqual(got["dropped_samples"], 0)
        self.assertEqual(got["complete_frames"], 96)
        tail = replace(events[1], sequence=0, frame=0, timestamp_ns=events[0].timestamp_ns - 1)
        changed = [tail] + [replace(event, sequence=event.sequence + 1) for event in events]
        with self.assertRaisesRegex(reference.RecordError, "partial Philips tail"):
            reference.qualify_segment(self.capture(changed),
                                      reference.Segment(0, 0, len(changed), allow_initial_partial=True))

    def test_partial_range_allowances_are_explicit_and_do_not_skip_samples(self):
        row = vectors.matrix()[0]
        original = synthetic_events(row, frames=98)
        capture = self.capture(original)
        with self.assertRaisesRegex(reference.RecordError, "initial physical slots"):
            reference.qualify_segment(capture, reference.Segment(0, 1, len(original)))
        with self.assertRaisesRegex(reference.RecordError, "final physical slots"):
            reference.qualify_segment(capture, reference.Segment(0, 0, len(original) - 1))
        got = reference.qualify_segment(capture, reference.Segment(
            0, 1, len(original) - 1, allow_initial_partial=True, allow_final_partial=True))
        self.assertEqual(got["complete_frames"], 96)
        self.assertEqual(got["actual_samples"], len(original) - 2)
        self.assertEqual((got["omitted_initial_slots"], got["omitted_final_slots"]), (1, 1))
        wrong = original[:]
        wrong[1] = replace(wrong[1], sample=wrong[1].sample ^ 1)
        with self.assertRaises(reference.RecordError):
            reference.qualify_segment(self.capture(wrong), reference.Segment(
                0, 1, len(original) - 1, allow_initial_partial=True, allow_final_partial=True))

    def test_short_or_empty_samples_cannot_qualify(self):
        for frames in (1, 95):
            events = self.std(frames)
            with self.assertRaisesRegex(reference.RecordError, "insufficient complete"):
                reference.qualify_segment(self.capture(events), reference.Segment(1, 0, len(events)))
        events = self.std()
        with self.assertRaisesRegex(reference.RecordError, "complete independent vector"):
            reference.qualify_segment(self.capture(events), reference.Segment(1, 0, len(events), min_frames=1))

    def test_fifo_dropped_words_remain_exact_actual_samples(self):
        events = self.std(dropped=(10, 11, 20))
        capture = self.capture(events)
        with self.assertRaisesRegex(reference.RecordError, "unexpected actual FIFO"):
            reference.qualify_segment(capture, reference.Segment(1, 0, len(events)))
        got = reference.qualify_segment(capture, reference.Segment(
            1, 0, len(events), drops="require", expected_dropped=3))
        self.assertEqual((got["actual_samples"], got["accepted_samples"], got["dropped_samples"]),
                         (len(events), len(events) - 3, 3))
        with self.assertRaisesRegex(reference.RecordError, "drop count mismatch"):
            reference.qualify_segment(capture, reference.Segment(1, 0, len(events), drops="allow", expected_dropped=4))
        events[10] = replace(events[10], sample=events[10].sample ^ 1)
        with self.assertRaisesRegex(reference.RecordError, "sample mismatch"):
            reference.qualify_segment(self.capture(events), reference.Segment(1, 0, len(events), phase=17, drops="allow"))
        clean = self.std()
        with self.assertRaisesRegex(reference.RecordError, "no actual FIFO-dropped"):
            reference.qualify_segment(self.capture(clean), reference.Segment(1, 0, len(clean), drops="require"))
        overflow_only = [replace(event, flags=5) for event in clean]
        with self.assertRaisesRegex(reference.RecordError, "no accepted actual RX"):
            reference.qualify_segment(self.capture(overflow_only), reference.Segment(1, 0, len(clean), drops="require"))

    def test_exact_fractional_cadence_and_one_ns_quantization(self):
        # Nonfixture rate exercises arithmetic without pretending to qualify an
        # unlisted row. int(Fraction) models QEMU nanosecond timestamp rounding.
        row = dict(vectors.matrix()[50], rate=44100)
        events = synthetic_events(row)
        timing = reference.check_cadence(events, row)
        self.assertEqual(Fraction(timing["frame_interval_ns"]), Fraction(10 ** 9, 44100))
        ordinary = self.std()
        for direction in (-1, 1):
            quantized = [replace(event, timestamp_ns=event.timestamp_ns + direction * (index % 2))
                         for index, event in enumerate(ordinary)]
            reference.check_cadence(quantized, vectors.matrix()[1])
        broken = ordinary[:]
        broken[-1] = replace(broken[-1], timestamp_ns=broken[-1].timestamp_ns + 2)
        with self.assertRaisesRegex(reference.RecordError, "cadence mismatch"):
            reference.check_cadence(broken, vectors.matrix()[1])

    def test_per_interval_allowance_cannot_accumulate_drift(self):
        events = self.std()
        drifting = [replace(event, timestamp_ns=event.timestamp_ns + index // 2)
                    for index, event in enumerate(events)]
        with self.assertRaisesRegex(reference.RecordError, "cadence mismatch"):
            reference.check_cadence(drifting, vectors.matrix()[1])

    def test_sparse_tdm_uses_same_physical_slot_not_adjacent_selected_slots(self):
        row = vectors.matrix()[48]  # 8-bit sparse 0x81: slots 0 and 7, not 0/1.
        events = synthetic_events(row)
        got = reference.check_cadence(events, row)
        self.assertEqual(Fraction(got["slot_interval_ns"]), 10000)
        self.assertEqual(events[2].timestamp_ns - events[0].timestamp_ns, 80000)
        self.assertEqual(events[1].timestamp_ns - events[0].timestamp_ns, 70000)
        broken = [replace(event, timestamp_ns=event.timestamp_ns - (60000 if event.slot == 7 else 0))
                  for event in events]
        with self.assertRaisesRegex(reference.RecordError, "slot cadence mismatch"):
            reference.check_cadence(broken, row)

    def test_raw_pdm_channels_same_instant_not_interleaved_half_frame(self):
        row = vectors.matrix()[120]
        events = synthetic_events(row, controller=1)
        self.assertEqual(events[0].timestamp_ns, events[1].timestamp_ns)
        self.assertEqual(events[2].timestamp_ns - events[0].timestamp_ns, 16000)
        reference.check_cadence(events, row)
        broken = [replace(event, timestamp_ns=event.timestamp_ns + int(event.slot == 1)) for event in events]
        with self.assertRaisesRegex(reference.RecordError, "same instant"):
            reference.check_cadence(broken, row)

    def test_start_reset_segments_preserve_global_sequence_and_timestamp(self):
        first = self.std()
        second = self.std(base_ns=first[-1].timestamp_ns + 1000000, sequence=len(first), phase=31)
        capture = self.capture(first + second)
        with self.assertRaisesRegex(reference.RecordError, "undeclared frame restart"):
            reference.qualify_segment(capture, reference.Segment(1, 0, len(capture.events)))
        segments = [reference.Segment(1, 0, len(first), boundary="start"),
                    reference.Segment(1, len(first), len(capture.events), boundary="reset")]
        result = reference.qualify(capture, segments, 2)
        self.assertEqual([part["vector_phase"] for part in result["segments"]], [17, 31])
        self.assertEqual(result["qualified_events"], len(capture.events))
        with self.assertRaisesRegex(reference.RecordError, "explicit start/reset"):
            reference.qualify(capture, [segments[0], replace(segments[1], boundary="selection")], 2)
        with self.assertRaisesRegex(reference.RecordError, "segment count"):
            reference.qualify(capture, segments, 1)
        second[0] = replace(second[0], sequence=0)
        with self.assertRaisesRegex(reference.RecordError, "sequence"):
            self.capture(first + second)

    def test_unidentified_mixed_rows_never_become_full_matrix_evidence(self):
        first = self.std()
        second = synthetic_events(vectors.matrix()[50], sequence=len(first),
                                  base_ns=first[-1].timestamp_ns + 1000000)
        capture = self.capture(first + second)
        result = reference.qualify(capture, [reference.Segment(1, 0, len(first))], 1)
        self.assertEqual(result["qualified_events"], len(first))
        self.assertEqual(result["unqualified_events"], len(second))
        self.assertFalse(result["full_matrix_qualified"])
        self.assertFalse(result["format_and_master_role_independently_identified"])
        self.assertFalse(result["electrical_unknown_mask_encoded"])
        self.assertIn("caller-declared", result["case_attribution"])
        self.assertEqual({row["status"] for row in result["blocked_rows"]}, {"BLOCKED"})
        self.assertEqual({row["row"] for row in result["blocked_rows"]}, {"external-peer", "pcm2pdm/pdm2pcm"})
        with self.assertRaises(reference.RecordError):
            reference.qualify(capture, [reference.Segment(1, 0, len(capture.events))], 1)

    def test_bad_selection_overlap_phase_and_unrecognized_row(self):
        events = self.std()
        capture = self.capture(events)
        for segment in (reference.Segment(-1, 0, len(events)),
                        reference.Segment(124, 0, len(events)),
                        reference.Segment("external-peer", 0, len(events)),
                        reference.Segment("pcm2pdm", 0, len(events)),
                        reference.Segment("pdm2pcm", 0, len(events)),
                        reference.Segment(1, -1, len(events)),
                        reference.Segment(1, len(events) + 1, len(events) + 2),
                        reference.Segment(1, 0, len(events), phase=96),
                        reference.Segment(1, 0, len(events), phase=18),
                        reference.Segment(1, 0, len(events), expected_dropped=-1)):
            with self.assertRaises(reference.RecordError):
                reference.qualify(capture, [segment], 1)
        segment = reference.Segment(1, 0, len(events))
        with self.assertRaisesRegex(reference.RecordError, "overlapping"):
            reference.qualify(capture, [segment, segment], 2)

    def test_cli_requires_finite_selection_and_emits_evidence_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.rx.bin"
            events = self.std()
            path.write_bytes(binary_capture(events))
            for arguments, expected_exit in ((["--case", "1"], 1),
                    (["--case", "1", "--stop-event", str(len(events))], 0)):
                text = io.StringIO()
                with patch.object(sys, "argv", ["record_reference.py", str(path)] + arguments), redirect_stdout(text):
                    exit_code = reference.main()
                self.assertEqual(exit_code, expected_exit)
                result = json.loads(text.getvalue())
                self.assertFalse(result["full_matrix_qualified"])
                self.assertEqual(result["status"], "FAIL" if expected_exit else "QUALIFIED_SELECTED_RX_SEGMENTS")
            plan = Path(directory) / "synthetic-plan.json"
            plan.write_text(json.dumps({"segments": [dict(case=1, start=0, stop=len(events), phase=17)]}))
            text = io.StringIO()
            with patch.object(sys, "argv", ["record_reference.py", str(path), "--plan", str(plan)]), redirect_stdout(text):
                self.assertEqual(reference.main(), 0)
            self.assertEqual(json.loads(text.getvalue())["segments"][0]["vector_phase"], 17)


if __name__ == "__main__":
    unittest.main(verbosity=2)
