# Development plan review — 2026-10-07

Status: **planning gate passed; no unresolved P1 findings**. This log verifies the proposed plan's
coverage/feasibility, not implementation correctness or completed functionality.

## Review method

Three agents audited source and primary references for separate workstreams. The
parent synthesized the master plan and requirement/milestone manifest. Review
scopes are then rotated across workstreams to challenge assumptions, shared
contracts, dependencies, coverage, failure tests and hardware/version evidence.

Structural checks use `python3 tools/verify-plan.py --self-test`. The checker looks
for omitted functionality/work-package references, missing acceptance dimensions
or documents, unknown/cyclic milestone dependencies, master-table/manifest disagreement,
and evidence-free or incomplete aggregate support promotion. It does not prove
hardware semantics, numerical solver accuracy or native radio feasibility.
Fine-grained task interface/phase dependencies remain a human technical review gate.

## Findings and disposition

| Finding from independent cross-workstream review | Correction and disposition |
| --- | --- |
| NET-03 required DC/RC while M4A followed completed digital M4 | Basic DC/pad resolution moved to M3/M4; RC/ADC to M4A; staged qualification explicit. Closed |
| NET-04 required new clocks but M2 preceded CORE-02 | M2 feasibility prototype uses existing virtual clock; final clock-integrated qualification in M3. Closed |
| Radio security/coexistence/ADC and SIMD context prerequisites absent | M3C crypto/NVS, M3P power, M4A core ADC, M3/M4 SIMD interrupt/pad dependencies added. ADC-radio integration remains M9. Closed |
| Hardware IDs had duplicate meanings | Peripheral acquisition IDs renamed EHW-01..03; shared HW catalogue and all 119 work IDs unique. Closed |
| Capability labels differed between documents | Canonical maturity catalogued/implemented/native-tested/hardware-compared/qualified; availability/reason separate. Closed |
| Unknown pad state cannot be returned as a GPIO bit | Explicit strict-pause or seeded/profiled guest-bit sampling with uncertainty/provenance and IRQ tests. Closed |
| Equal-time analog/source/peripheral/ADC events and stiff-RC behavior underspecified | Stable phased delta-cycle order, bounded settling/oscillation, timestep-convergence and aperture-edge tests added. Closed |
| Async timestamps or live networking implied determinism | Virtual barrier/service-watchdog contract and scripted/recorded/live modes separated. Closed |
| Granular coverage and aggregate promotion could be omitted | Every package mapped to milestones and requirement/shared gates; package closure and evidence required for aggregate promotion. Fine-phase dependencies explicitly manual. Closed |
| Dedicated GPIO, SDM, brownout and retention engine omitted | PIE/bundle-to-pad core-affinity gate and SOC-16..18 added; TEMP is also explicit. Closed |
| Validator matched the ID header and excluded digit-containing IDs | Alphanumeric parsing with header exclusion corrected; I2C/I2S/TEMP and new rows included. Regression mutations pass. Closed |
| M10 analog-dependent exit and diagram dependencies needed clarification | M4A added to M10 exit prerequisites while early tasks remain parallel; M4→M7 and M4/M4A→M10 arrows present. Closed |

## Independent final decisions

* UX reviewer: **Pass, no remaining P1**; cross-reviewed engine/analog/radio/ISA
  contracts, not only its authored UI document; independently reran checker.
* Radio/SIMD reviewer: **Pass, no remaining P1**; cross-reviewed full SoC/analog,
  dependency/ID traceability and qualifier rules; independently reran checker.
* Peripheral/electrical reviewer: **No unresolved engineering P1 or cycle**;
  checked shared time/net/analog/ADC/radio prerequisites and final phase boundaries.

The parent applied corrections and retained integration ownership. The source audit
and plan synthesis involved three permitted subagents with disjoint document files;
reviews were read-only and rotated across workstreams.

## Recorded verification

* Coverage: **30 functionality rows, 119 unique work packages, 15 acyclic milestones,
  5 shared gates**, matching the master dependency table.
* Checker negative cases: **8/8 rejected** — missing functionality, cycle,
  missing negative acceptance, evidence-free promotion, invalid document,
  missing granular package, master/manifest mismatch, and premature aggregate promotion.
* Existing simulator regression: **5/5 CTest targets passed**, both real firmware
  fixtures enabled, recovered extension binary, **0 skipped, 8.49 seconds**.
* Whitespace validation: `git diff --check` passed.
* COM5: enumerated only; no port opened or firmware flashed.

## Remaining implementation risks and evidence boundaries

Native vendor Wi-Fi/BLE controller interfaces, the virtual-time barrier integration,
analog accuracy, broad firmware compatibility and SIMD semantics still require
prototypes and actual acceptance results. Hardware timing/ADC transfer accuracy
needs independent instruments/reference inputs in addition to serial access.
QEMU does not establish physical CPU-cycle or RF waveform accuracy. The plan is
technically reviewed and structurally checked; implementation remains under the
active goal. Product code was unchanged in this planning delivery.
