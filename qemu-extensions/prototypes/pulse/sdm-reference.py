#!/usr/bin/env python3
"""SOC16 independent integer/exponential oracle (no QEMU or hardware access).

Run only after the parent's SOURCE_READY gate:
  python3 sdm-reference.py --samples 65536 --require-cycle
  python3 sdm-reference.py --density 0 --density -128 --density 127

The transfer equation y[n]=x[n-1]+(1-z^-1)^2 e[n] with zero initial
errors implies D[n]=sum(y-x)=e[n]-e[n-1] and A[n]=sum D=e[n].
Thus the declared tie-high quantizer is high iff A[n-1]+D[n-1]<=x.
This cumulative-residual construction is independent of the production
error-delay implementation. Python integers impose no wrapping/saturation.
For constant density a repeated (D,A) is a finite-state orbit certificate,
NOT just evidence that a finite prefix happened to remain small. A trajectory
without a repeated state is explicitly unproven; no universal bound is inferred.
The physical RC oracle integrates individual pulses, never a GPIO mean voltage.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

PROFILE = "s3-sdm-second-order-transfer-v1"


def trajectory(density, samples):
    discrepancy = integral = 0
    previous_error = older_error = 0
    seen = {(0, 0): 0}
    period = None
    cycle_start = None
    bounds = [0, 0]
    bits = []
    for n in range(samples):
        high = integral + discrepancy <= density
        output = 128 if high else -128
        discrepancy += output - density
        integral += discrepancy
        # Independently check the public transfer identity at EVERY pulse.
        if output != density + integral - 2 * previous_error + older_error:
            raise AssertionError("second-order transfer residual")
        older_error, previous_error = previous_error, integral
        bounds[0] = max(bounds[0], abs(discrepancy))
        bounds[1] = max(bounds[1], abs(integral))
        bits.append(int(high))
        state = (discrepancy, integral)
        if period is None:
            if state in seen:
                cycle_start = seen[state]
                period = n + 1 - cycle_start
            else:
                seen[state] = n + 1
    cycle_high = None
    if period is not None:
        cycle_high = sum(bits[cycle_start:cycle_start + period])
        if cycle_high * 256 != (density + 128) * period:
            raise AssertionError("closed orbit violates documented density")
    return {
        "density": density,
        "ticks": samples,
        "high": sum(bits),
        "prefix16": "".join(map(str, bits[:16])),
        "bits_sha256": hashlib.sha256(bytes(bits)).hexdigest(),
        "state": [discrepancy, integral],
        "max_abs_delta_error": bounds[0],
        "max_abs_error": bounds[1],
        "cycle_start": cycle_start,
        "cycle_period": period,
        "cycle_high": cycle_high,
        "bounded_state": "proved-reachable-orbit" if period else "unproven-finite-prefix",
    }, bits


def rc_reference(bits, pulse_ns=1000, resistance=1000, capacitance=1e-9,
                 driver_resistance=40, rail=3.3):
    # Output resets low, so each boundary samples the PREVIOUS pulse interval.
    value = 0.0
    previous = 0
    decay = math.exp(-pulse_ns * 1e-9 /
                     ((resistance + driver_resistance) * capacitance))
    steady = []
    for n, high in enumerate(bits):
        target = previous * rail
        value = target + (value - target) * decay
        if n >= len(bits) // 2:
            steady.append(value)
        previous = high
    return {
        "model": "piecewise-pulse-exponential",
        "net": "R.b/C.p/MCU.input-pad",
        "pulse_ns": pulse_ns,
        "R_ohm": resistance,
        "driver_R_ohm": driver_resistance,
        "C_f": capacitance,
        "rail_v": rail,
        "boundary_mean_v": sum(steady) / len(steady),
        "boundary_ripple_v": max(steady) - min(steady),
        "final_charge_c": value * capacitance,
        "charge_at_zero_time_reset_c": value * capacitance,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=65536)
    parser.add_argument("--density", type=int, action="append",
                        help="signed 8-bit density; repeat, default all 256")
    parser.add_argument("--require-cycle", action="store_true",
                        help="fail if any requested reachable orbit lacks a cycle certificate")
    parser.add_argument("--vectors", type=Path,
                        default=Path(__file__).with_name("sdm-reference-vectors.json"))
    args = parser.parse_args()
    if args.samples < 512:
        parser.error("--samples must be at least 512")
    densities = args.density if args.density is not None else list(range(-128, 128))
    if any(d < -128 or d > 127 for d in densities):
        parser.error("density must be signed 8-bit")
    vectors = json.loads(args.vectors.read_text(encoding="utf-8"))
    if vectors["waveform_profile"] != PROFILE:
        raise AssertionError("vector profile mismatch")
    for vector in vectors["analytical_vectors"]:
        record, _ = trajectory(vector["density"], 512)
        if record["prefix16"] != vector["prefix16"]:
            raise AssertionError(f"analytical prefix density={vector['density']}")
        if "cycle_period" in vector and record["cycle_period"] != vector["cycle_period"]:
            raise AssertionError(f"analytical orbit density={vector['density']}")
    records = []
    for density in densities:
        record, _ = trajectory(density, args.samples)
        records.append(record)
    unproven = [r["density"] for r in records if r["cycle_period"] is None]
    summary = {
        "record": "SDM_REFERENCE",
        "profile": PROFILE,
        "oracle": "double-cumulative-output-minus-input",
        "densities": len(records),
        "ticks_per_density": args.samples,
        "transfer_identity": "checked-every-pulse",
        "documented_density": "exact-on-every-certified-orbit",
        "analytical_vectors": "passed",
        "bounded_orbits_proved": len(records) - len(unproven),
        "unproven_densities": unproven,
        "max_abs_error": max(r["max_abs_error"] for r in records),
        "max_abs_delta_error": max(r["max_abs_delta_error"] for r in records),
        "max_cycle_period": max((r["cycle_period"] or 0) for r in records),
        "state_width_claim": "reachable orbit only; no silicon width/wrap claim",
    }
    print(json.dumps(summary, separators=(",", ":")))
    # Compact records for named acceptance extremes, not a vendor register dump.
    for record in records:
        if len(records) <= 8 or record["density"] in (-128, 0, 127):
            print(json.dumps({"record": "SDM_VECTOR", "profile": PROFILE, **record},
                             separators=(",", ":")))
    _, bits = trajectory(0, 512)
    print(json.dumps({"record": "SDM_RC_REFERENCE", "profile": PROFILE,
                      **rc_reference(bits)}, separators=(",", ":")))
    print(json.dumps({
        "record": "SDM_HARDWARE_BITSTREAM", "status": "blocked",
        "reason": "No measured S3 bitstream or public hidden width/tie/wrap/reset/pipeline specification",
        "physical_qualification": False,
        "native_qualification": "separate powered-net qtest required",
        "idf_resources": "ordinary IDF firmware fixture owns real allocation/FSM/PM lifecycle",
    }, separators=(",", ":")))
    return 1 if args.require_cycle and unproven else 0


if __name__ == "__main__":
    raise SystemExit(main())
