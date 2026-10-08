#!/usr/bin/env python3
"""Lane-level comparator for ESP32-S3 SIMD fixture captures (simd-pie prototype).

Compares a QEMU capture against hardware golden records byte-for-byte and,
optionally, against a previous QEMU capture. Complete batches and source/build
integrity are required. Lane values have explicit reference/candidate roles;
no unequal input or captured state is discarded.

Usage:
  python3 compare-captures.py \
      --hw build-hardware-host/run-2026-10-07-rebaselined/simd/capture/records.json \
      --qemu <candidate>/capture/records.json \
      --baseline tests/firmware/simd_reference/build/runs/grace-official-01/capture/records.json \
      --output build-runtime-state/simd-fix-2026-10-07/after-compare.json \
      --markdown build-runtime-state/simd-fix-2026-10-07/after-compare.md

Exit status 0 only when every vector matches the hardware records byte-exactly.
"""

import argparse
import importlib.util
import json
import sys
from pathlib import Path

STATE_FIELDS = ("q", "qacc", "aux", "ar_deltas")
ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("hardware_reference", ROOT / "tools/hardware-reference.py")
capture_reader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture_reader)


def load_capture(path):
    path = Path(path)
    folder = path.parent
    try:
        data = json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=capture_reader.unique_object)
        metadata = json.loads((folder / "metadata.json").read_text(encoding="utf-8"),
                              object_pairs_hook=capture_reader.unique_object)
        manifest_path = folder / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"),
                              object_pairs_hook=capture_reader.unique_object)
        raw_path = folder / "raw.log"
        if capture_reader.digest(raw_path) != metadata.get("raw_sha256"):
            raise ValueError("Raw capture integrity mismatch")
        if (capture_reader.digest(manifest_path) != metadata.get("manifest_sha256") or
                manifest != metadata.get("manifest")):
            raise ValueError("Manifest integrity mismatch")
        artifacts = manifest.get("artifacts", {})
        if (manifest.get("fixture") != "simd_reference" or
                not capture_reader.hex_bytes(artifacts.get("flash_sha256"), 32) or
                not capture_reader.hex_bytes(artifacts.get("elf_sha256"), 32)):
            raise ValueError("Missing SIMD firmware identity")
        parsed = capture_reader.parse_log(raw_path.read_bytes(), manifest)
        if (not parsed["successful"] or data != parsed or
                metadata.get("complete") is not True or
                metadata.get("successful") is not True):
            raise ValueError("Incomplete, unsuccessful or changed capture records")
        return parsed, manifest
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f"{path}: {error}") from error


def require_same_fixture(reference, candidate):
    for field in ("fixture", "source_sha256", "expected_records",
                  "artifacts", "source_files", "mode"):
        if reference[1].get(field) != candidate[1].get(field):
            raise SystemExit(f"Capture firmware/fixture identity differs: {field}")
    for field in ("seed", "elf_digest", "source_sha256", "fixture", "mode"):
        if reference[0]["records"][0].get(field) != candidate[0]["records"][0].get(field):
            raise SystemExit(f"Compiled vector identity differs: {field}")


def byte_diffs(a_hex, b_hex):
    """Byte-level differences between two equal-length hex strings."""
    if a_hex == b_hex:
        return []
    if len(a_hex) != len(b_hex):
        return [{"error": f"length {len(a_hex)} vs {len(b_hex)}"}]
    a, b = bytes.fromhex(a_hex), bytes.fromhex(b_hex)
    return [
        {"byte": i, "reference": f"{x:02x}", "candidate": f"{y:02x}"}
        for i, (x, y) in enumerate(zip(a, b))
        if x != y
    ]


def compare_index(a_vectors, b_vectors, a_name, b_name):
    """Index b_vectors against a_vectors (reference). Returns per-vector report."""
    by_key = {}
    for v in b_vectors:
        by_key[(v["seq"], v["case"], v["operation"])] = v
    report, mismatches = [], 0
    for ref in a_vectors:
        key = (ref["seq"], ref["case"], ref["operation"])
        cand = by_key.pop(key, None)
        entry = {"seq": ref["seq"], "case": ref["case"], "operation": ref["operation"]}
        if cand is None:
            entry["error"] = f"missing in {b_name}"
            mismatches += 1
            report.append(entry)
            continue
        diffs = []
        for field in ("input_q", "input_qacc"):
            d = byte_diffs(ref[field], cand[field])
            if d:
                diffs.append({"field": field, "lanes": d})
        if ref["vector_seed"] != cand["vector_seed"]:
            diffs.append({"field": "vector_seed", "reference": ref["vector_seed"],
                          "candidate": cand["vector_seed"]})
        for phase in ("before", "after"):
            for field in STATE_FIELDS:
                d = byte_diffs(ref[phase][field], cand[phase][field])
                if d:
                    diffs.append({"field": f"{phase}.{field}", "lanes": d})
        if diffs:
            mismatches += 1
            entry["diffs"] = diffs
        report.append(entry)
    for key in sorted(by_key, key=lambda k: k[0]):
        v = by_key[key]
        report.append({"seq": v["seq"], "case": v["case"],
                       "operation": v["operation"], "error": f"extra in {b_name}"})
        mismatches += 1
    return report, mismatches


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hw", required=True, help="hardware golden records.json (reference)")
    ap.add_argument("--qemu", required=True, help="candidate QEMU records.json")
    ap.add_argument("--baseline", help="previous QEMU records.json for lane-change report")
    ap.add_argument("--output", required=True, help="JSON report path (new file)")
    ap.add_argument("--markdown", help="optional human-readable summary path")
    args = ap.parse_args()

    hardware = load_capture(args.hw)
    candidate = load_capture(args.qemu)
    require_same_fixture(hardware, candidate)
    hw = [r for r in hardware[0]["records"] if r["type"] == "vector"]
    qemu = [r for r in candidate[0]["records"] if r["type"] == "vector"]

    out = Path(args.output)
    if out.exists():
        raise SystemExit(f"refusing to overwrite existing evidence: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)

    vectors, mismatched = compare_index(hw, qemu, "hardware", "qemu")
    by_op = {}
    for v in vectors:
        if "diffs" in v or "error" in v:
            by_op.setdefault(v["operation"], []).append(v["seq"])
    result = {
        "comparison": f"hardware golden vs QEMU {args.qemu}",
        "schema_version": 2,
        "reference_role": "hardware",
        "candidate_role": "qemu",
        "hw_records": str(args.hw),
        "qemu_records": str(args.qemu),
        "total_vectors": len(hw),
        "matched_vectors": len(hw) - mismatched,
        "mismatched_vectors": mismatched,
        "by_operation": {k: sorted(v) for k, v in sorted(by_op.items())},
        "vectors": vectors,
    }

    if args.baseline:
        previous = load_capture(args.baseline)
        require_same_fixture(hardware, previous)
        base = [r for r in previous[0]["records"] if r["type"] == "vector"]
        # Lanes the candidate changed relative to the previous QEMU build.
        changed, _ = compare_index(base, qemu, "baseline-qemu", "candidate-qemu")
        changed = [v for v in changed if "diffs" in v or "error" in v]
        changed_by_op = {}
        for v in changed:
            changed_by_op.setdefault(v["operation"], []).append(v["seq"])
        # Classify against actual baseline-vs-hardware results, not raw
        # baseline vectors (which never contain a comparison's "diffs" field).
        old_comparison, _ = compare_index(hw, base, "hardware", "baseline-qemu")
        previously_matching = {
            (v["seq"], v["case"], v["operation"]) for v in old_comparison
            if "diffs" not in v and "error" not in v}
        regressed = [
            v["seq"] for v in vectors
            if (v["seq"], v["case"], v["operation"]) in previously_matching
            and ("diffs" in v or "error" in v)]
        result["baseline_comparison"] = {
            "baseline_records": str(args.baseline),
            "reference_role": "baseline-qemu",
            "candidate_role": "candidate-qemu",
            "vectors_changed_vs_baseline": len(changed),
            "changed_by_operation": {k: sorted(v) for k, v in sorted(changed_by_op.items())},
            "changed_vectors": changed,
            "regressions_vs_hardware": regressed,
        }
        result["baseline_comparison"]["previously_matching_still_matching"] = not regressed

    out.write_text(json.dumps(result, indent=1) + "\n")

    if args.markdown:
        lines = [
            "# SIMD capture comparison",
            "",
            f"- hardware reference: `{args.hw}`",
            f"- candidate QEMU: `{args.qemu}`",
            f"- result: **{result['matched_vectors']}/{result['total_vectors']} vectors byte-exact**, "
            f"{mismatched} mismatched",
        ]
        if args.baseline:
            bc = result["baseline_comparison"]
            lines += [
                f"- baseline QEMU: `{args.baseline}`",
                f"- vectors changed vs baseline: {bc['vectors_changed_vs_baseline']}",
                f"- changed operations: {json.dumps(bc['changed_by_operation'])}",
                f"- regressions (matched hw before, mismatch now): {bc['regressions_vs_hardware'] or 'none'}",
            ]
        if by_op:
            lines += ["", "| Operation | Mismatched seq |", "| --- | --- |"]
            lines += [f"| {k} | {sorted(v)} |" for k, v in sorted(by_op.items())]
        else:
            lines += ["", "All vectors match hardware byte-exactly."]
        Path(args.markdown).write_text("\n".join(lines) + "\n")

    print(json.dumps({"total": result["total_vectors"],
                      "matched": result["matched_vectors"],
                      "mismatched": mismatched,
                      "by_operation": result["by_operation"]}))
    return 0 if mismatched == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
