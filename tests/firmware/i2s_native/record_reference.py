#!/usr/bin/env python3
"""Independent comparison of finite, immutable *actual RX* event streams.

The recorder has no case ID, mode, format, role, electrical known-mask, or reset
marker. Row attribution and start/reset boundaries therefore MUST be supplied
by the capture owner, never inferred as full-matrix coverage. Header/event
checks apply to the entire file; sample/cadence qualification applies ONLY to
explicit half-open event ranges. UART hashes, playback queues and ISR counters
are deliberately not inputs.

CLI examples (event counts must come from the actual capture):
  python record_reference.py i2s0.rx.bin --case 0 --stop-event 1536 --phase 0
  python record_reference.py i2s0.rx.bin --plan capture-plan.json --segment-count 2

A plan is {"segments": [{"case": 0, "start": 0, "stop": 1536,
"boundary": "start", "phase": 0, "min_frames": 96, "drops": "forbid"},
{"case": 0, "start": 1536, "stop": 2304, "boundary": "reset"}]}. Omitted
phase is determined by an exact, unique cyclic-frame match to the independent
96-frame vector, not by changing expected samples. Optional initial/final
partial-frame allowances must be explicit. The unrecorded Philips preceding
slot tail is never a sample, a FIFO drop, or an invented event.
"""
import argparse
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

import expected_vectors as vectors

HEADER = struct.Struct("<8sII")
EVENT = struct.Struct("<QQQIHBB")
MAGIC = b"S3I2SRX1"
VALID, ACCEPTED, DROPPED = 1, 2, 4
BLOCKED_ROWS = (
    {"row": "external-peer", "status": "BLOCKED",
     "reason": "no registered independent external I2S sample-peer reference"},
    {"row": "pcm2pdm/pdm2pcm", "status": "BLOCKED",
     "reason": "no independently specified converter coefficients/reference"},
)


class RecordError(ValueError):
    """Malformed stream or insufficient/incorrect capture evidence."""


def require(condition, message):
    if not condition:
        raise RecordError(message)


@dataclass(frozen=True)
class RxEvent:
    timestamp_ns: int
    sequence: int
    frame: int
    sample: int
    slot: int
    width: int
    flags: int

    @property
    def accepted(self):
        return bool(self.flags & ACCEPTED)

    @property
    def dropped(self):
        return bool(self.flags & DROPPED)


@dataclass(frozen=True)
class Capture:
    controller: int
    events: tuple
    sha256: str


@dataclass(frozen=True)
class Segment:
    case: int
    start: int
    stop: int
    boundary: str = "selection"
    phase: int | None = None
    min_frames: int = vectors.FRAMES
    drops: str = "forbid"
    expected_dropped: int | None = None
    allow_initial_partial: bool = False
    allow_final_partial: bool = False


def read_capture(path):
    """Read a closed capture, rejecting truncation and every malformed event."""
    digest = hashlib.sha256()
    events = []
    with Path(path).open("rb") as stream:
        header = stream.read(HEADER.size)
        require(len(header) == HEADER.size, "truncated 16-byte header")
        digest.update(header)
        magic, controller, version = HEADER.unpack(header)
        require(magic == MAGIC, "bad record magic")
        require(controller in (0, 1), "unsupported controller")
        require(version == 1, "unsupported record version")
        while True:
            raw = stream.read(EVENT.size)
            if not raw:
                break
            require(len(raw) == EVENT.size, f"truncated 32-byte event {len(events)}")
            digest.update(raw)
            event = RxEvent(*EVENT.unpack(raw))
            require(event.width in vectors.WIDTHS, f"invalid width at event {len(events)}")
            require(event.slot < 8, f"invalid physical slot at event {len(events)}")
            require(event.sample < (1 << event.width), f"sample exceeds width at event {len(events)}")
            # There is no known-mask field. Only actual, fully resolved RX words
            # are legal: unknown flag bits or a false VALID claim cannot pass.
            require(event.flags in (VALID | ACCEPTED, VALID | DROPPED),
                    f"invalid validity/disposition flags at event {len(events)}")
            if events:
                previous = events[-1]
                require(event.sequence > previous.sequence, "duplicate/decreasing immutable sequence")
                require(event.sequence == previous.sequence + 1, "missing immutable event sequence")
                require(event.timestamp_ns >= previous.timestamp_ns, "decreasing timestamp")
            else:
                require(event.sequence == 0, "complete immutable file must begin with sequence zero")
            events.append(event)
    require(events, "empty RX stream cannot qualify samples")
    return Capture(controller, tuple(events), digest.hexdigest())


def physical_slots(row):
    return (8 if row["bits"] <= 16 else 4) if row["mode"] == "tdm" else 2


def logical_frame(event, row):
    # Philips shifts the last slot completion one bit over the physical frame
    # boundary. Normalize by *physical* last slot, not the last selected slot.
    return event.frame - int(row["format"] == "philips" and
                             event.slot == physical_slots(row) - 1)


def check_cadence(events, row):
    """Exact rational elapsed cadence; at most 1 ns timestamp quantization.

    Adjacent and first-to-current same-physical-slot comparisons both apply,
    so per-interval tolerances cannot accumulate into hidden long-term drift.
    Inter-slot spacing also constrains actual geometry. Raw PDM channels must
    complete at exactly the same instant within each frame.
    """
    frame_ns = Fraction(vectors.cadence(row)["interval_ns"])
    slot_ns = Fraction(0) if row["mode"] == "pdm" else frame_ns / physical_slots(row)
    first_by_slot, last_by_slot = {}, {}
    same_slot_intervals = 0
    origin = events[0]
    for index, event in enumerate(events):
        frame = logical_frame(event, row)
        elapsed = (frame - logical_frame(origin, row)) * frame_ns + (event.slot - origin.slot) * slot_ns
        require(abs(Fraction(event.timestamp_ns - origin.timestamp_ns) - elapsed) <= 1,
                f"slot cadence mismatch at selected event {index}")
        if event.slot in first_by_slot:
            for reference in (first_by_slot[event.slot], last_by_slot[event.slot]):
                delta_frames = frame - logical_frame(reference, row)
                require(delta_frames > 0, "duplicate/decreasing same-slot frame")
                require(abs(Fraction(event.timestamp_ns - reference.timestamp_ns) - delta_frames * frame_ns) <= 1,
                        f"same-physical-slot cadence mismatch at selected event {index}")
            same_slot_intervals += 1
        else:
            first_by_slot[event.slot] = event
        if index and row["mode"] == "pdm" and frame == logical_frame(events[index - 1], row):
            require(event.timestamp_ns == events[index - 1].timestamp_ns,
                    "raw PDM channels did not complete at the same instant")
        last_by_slot[event.slot] = event
    require(same_slot_intervals > 0, "no actual same-physical-slot cadence observation")
    return dict(frame_interval_ns=str(frame_ns), slot_interval_ns=str(slot_ns),
                same_physical_slot_intervals=same_slot_intervals,
                timestamp_tolerance_ns=1, raw_pdm_same_instant=row["mode"] == "pdm")


def qualify_segment(capture, segment):
    require(type(segment.case) is int and 0 <= segment.case < len(vectors.matrix()), "invalid selected case")
    require(type(segment.start) is int and type(segment.stop) is int and
            0 <= segment.start < segment.stop <= len(capture.events), "invalid finite event range")
    require(segment.boundary in ("selection", "start", "reset"), "explicit boundary must be selection/start/reset")
    require(type(segment.min_frames) is int and segment.min_frames >= vectors.FRAMES,
            "at least one complete independent vector ring is required")
    require(segment.drops in ("forbid", "allow", "require"), "invalid drop policy")
    require(segment.expected_dropped is None or
            (type(segment.expected_dropped) is int and segment.expected_dropped >= 0), "invalid expected drop count")
    require(type(segment.allow_initial_partial) is bool and type(segment.allow_final_partial) is bool,
            "partial-frame allowances must be booleans")
    row = vectors.matrix()[segment.case]
    events = capture.events[segment.start:segment.stop]
    if row["mode"] == "pdm":
        require(capture.controller == 1 - row["source"], "wrong raw PDM destination controller")
    require(not segment.allow_initial_partial or row["format"] == "philips",
            "initial partial allowance is only for explicitly selected Philips frames")
    active = [slot for slot in range(physical_slots(row)) if row["mask"] & (1 << slot)]
    first_frame = logical_frame(events[0], row)
    require(first_frame >= 0, "initial partial Philips tail is not a completed RX sample")
    first_slot = events[0].slot
    require(first_slot in active, "first physical slot is outside selected mask")
    first_position = active.index(first_slot)
    require(first_position == 0 or segment.allow_initial_partial, "missing initial physical slots")
    position = first_position
    frame = first_frame
    frame_counts = {}
    for index, event in enumerate(events):
        require(event.width == row["bits"], f"selected width mismatch at event {index}")
        require(event.slot == active[position] and logical_frame(event, row) == frame,
                f"missing/reordered physical slot or undeclared frame restart at event {index}")
        frame_counts[frame] = frame_counts.get(frame, 0) + 1
        position += 1
        if position == len(active):
            position = 0
            frame += 1
    require(position == 0 or segment.allow_final_partial, "missing final physical slots")
    complete_frames = sum(count == len(active) for count in frame_counts.values())
    require(complete_frames >= segment.min_frames, "insufficient complete frames for finite sample qualification")
    source = row.get("source", 1 - capture.controller)
    payload = vectors.vector(source, row["bits"], row["mask"])
    sample_bytes = row["bits"] // 8
    stride = sample_bytes * len(active)

    def matches(phase):
        for event in events:
            vector_frame = (phase + logical_frame(event, row) - first_frame) % vectors.FRAMES
            offset = vector_frame * stride + active.index(event.slot) * sample_bytes
            if event.sample != int.from_bytes(payload[offset:offset + sample_bytes], "little"):
                return False
        return True

    if segment.phase is None:
        phases = [phase for phase in range(vectors.FRAMES) if matches(phase)]
        require(len(phases) == 1, "no unique exact cyclic-frame sample match")
        phase = phases[0]
    else:
        require(type(segment.phase) is int and 0 <= segment.phase < vectors.FRAMES, "invalid explicit vector phase")
        phase = segment.phase
        require(matches(phase), "exact actual RX sample mismatch")
    dropped = sum(event.dropped for event in events)
    accepted = len(events) - dropped
    require(accepted > 0, "no accepted actual RX samples")
    require(segment.drops != "forbid" or dropped == 0, "unexpected actual FIFO-dropped samples")
    require(segment.drops != "require" or dropped > 0, "no actual FIFO-dropped sample evidence")
    require(segment.expected_dropped is None or dropped == segment.expected_dropped, "actual FIFO drop count mismatch")
    timing = check_cadence(events, row)
    return dict(selected_case=row, start_event=segment.start, stop_event=segment.stop,
                declared_boundary=segment.boundary, vector_phase=phase,
                first_logical_frame=first_frame, last_logical_frame=logical_frame(events[-1], row),
                complete_frames=complete_frames, actual_samples=len(events),
                accepted_samples=accepted, dropped_samples=dropped,
                omitted_initial_slots=first_position,
                omitted_final_slots=(len(active) - position) if position else 0,
                exact_samples=True, exact_order_width_geometry=True, cadence=timing)


def qualify(capture, segments, segment_count):
    require(type(segment_count) is int and segment_count > 0 and len(segments) == segment_count,
            "explicit segment count mismatch")
    results = []
    prior = None
    for segment in segments:
        result = qualify_segment(capture, segment)
        if prior:
            require(segment.start >= prior.stop, "overlapping/reordered selected ranges")
        if segment.start:
            current, previous = capture.events[segment.start], capture.events[segment.start - 1]
            if current.frame < previous.frame:
                require(segment.boundary in ("start", "reset"), "frame counter restart needs an explicit start/reset segment")
        results.append(result)
        prior = segment
    selected = sum(result["actual_samples"] for result in results)
    return dict(status="QUALIFIED_SELECTED_RX_SEGMENTS", evidence_kind="immutable_actual_rx_record",
                controller=capture.controller, record_sha256=capture.sha256,
                file_events=len(capture.events), qualified_events=selected,
                unqualified_events=len(capture.events) - selected, segments=results,
                case_attribution="caller-declared; no case ID encoded in stream",
                full_matrix_qualified=False, electrical_unknown_mask_encoded=False,
                format_and_master_role_independently_identified=False,
                blocked_rows=list(BLOCKED_ROWS))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("record", help="closed i2s0.rx.bin or i2s1.rx.bin actual capture")
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--case", type=int, help="externally attributed expected_vectors matrix row")
    selection.add_argument("--plan", type=Path, help="JSON explicit finite event-range segments")
    parser.add_argument("--segment-count", type=int, default=1)
    parser.add_argument("--start-event", type=int, default=0)
    parser.add_argument("--stop-event", type=int, help="exclusive event index; required for single-row selection")
    parser.add_argument("--phase", type=int, help="first logical frame cyclic-vector phase; otherwise exact unique match")
    parser.add_argument("--boundary", choices=("selection", "start", "reset"), default="selection")
    parser.add_argument("--min-frames", type=int, default=vectors.FRAMES)
    parser.add_argument("--drops", choices=("forbid", "allow", "require"), default="forbid")
    parser.add_argument("--expected-dropped", type=int)
    parser.add_argument("--allow-initial-partial", action="store_true")
    parser.add_argument("--allow-final-partial", action="store_true")
    args = parser.parse_args()
    try:
        if args.plan:
            document = json.loads(args.plan.read_text(encoding="utf-8"))
            require(isinstance(document, dict) and set(document) == {"segments"}, "plan must contain only segments")
            require(isinstance(document["segments"], list), "plan segments must be a list")
            segments = [Segment(**entry) for entry in document["segments"]]
        else:
            require(args.stop_event is not None, "--stop-event is required; never assume a mixed stream is one case")
            segments = [Segment(args.case, args.start_event, args.stop_event, args.boundary,
                                args.phase, args.min_frames, args.drops, args.expected_dropped,
                                args.allow_initial_partial, args.allow_final_partial)]
        result = qualify(read_capture(args.record), segments, args.segment_count)
    except (RecordError, OSError, TypeError, json.JSONDecodeError) as exc:
        print(json.dumps(dict(status="FAIL", error=str(exc), full_matrix_qualified=False,
                              blocked_rows=list(BLOCKED_ROWS)), indent=2))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
