# Ready-to-use takeover prompts

These prompts carry requirements, file scope and evidence boundaries explicitly.
Replace role/scope only after the integration owner allocates files. They do not
grant physical flash approval or authorize publication.

## Coordinator

~~~text
Take over the ESP32S3VM implementation in
C:/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM.
Read docs/handoff/README.md and its linked packet, then the canonical master and
three workstream plans. Preserve all uncommitted/untracked work. The goal is
ordinary Arduino/recent ESP-IDF firmware, accurate digital AND analog electrical
routing/ADC, full listed peripherals, native Wi-Fi/BLE/SIMD, and Carbon light/dark UX,
using Espressif's official QEMU fork. This full goal is unfinished.

Inspect live agents/processes and the original active 30-minute heartbeat before
claiming sole integration ownership. Allocate disjoint worker paths; you own
shared MainWindow/CMake/builds and cross-contract integration. Check frozen GUI
validation result. Enabled hostbus currently fails before QMP; a QAPI options patch
is prepared but unapplied. Reproduce and finish that gate before claiming timing.
Keep pristine official/custom/new extension lanes separate. WSL2 runbook gives
exact commands, paths and activation pitfalls.

COM5 original flash is preserved locally. Hardware flashing approval is unanswered:
do not execute a physical test or infer consent from continuation/documentation.
Use primary source research and actual fixture evidence. Report exact results,
limitations and next dependencies; update the handoff/evidence ledger.
~~~

## UX/model worker

~~~text
Read docs/handoff/04-ux-editor.md and 03-contracts-and-ownership.md.
Own BoardWorkspace.*, ProjectDocument.*, their circuit/theme/project tests and
project schema only. Coordinate any MainWindow/CMake change with the integrator.
Latest isolated suites pass 5/5, 6/6, 16/16; latest shared required lane also passes
9/9 with 0 skipped in 38.45 seconds. Fresh theme previews retain a clipped table
header polish issue.
Retain one pure-v3 graph; no mixed legacy devices/buses aliases. Save/Open/geometry/
undo must never restart devices. Apply is explicit, strict and blocked for unknown
or active runtime state. Preserve paths, invalid drafts, stable IDs and viewport.
Finish H-UX-01..05 evidence, then dependent analog/trace/accessibility functionality.
Use isolated worker builds; no physical hardware or unrelated core edits.
~~~

## QEMU/transport worker

~~~text
Read docs/handoff/05-qemu-and-electrical.md and the hostbus contract.
Own tracked prototype source/tests and patch suggestions; integrator applies them
to the separate pinned WSL checkout. Do not edit the pristine baseline or shared
cache directly. Base is 40edccac415693c5130f91c01d84176ae6008566.
All four ordered patches are prepared; QAPI registration is unapplied and the
actual enabled test fails before QMP. No virtual-time pass exists yet.
Finish H-BUS-01..09, preserving raw c/s debugger tests, exact stopped/completion
timestamps, no-auto-resume semantics, reset/disconnect/fault and restore denial.
Fix documented raw-harness false-positive risks before promotion. Host codec
formatting is not full bus semantics. No physical hardware.
~~~

## Electrical/peripheral worker

~~~text
Read the peripheral/electrical plan, electrical-dc contract and handoff section.
The standalone GPL DC kernel passes 553 analytic/failure checks, but native GPIO/
IRQ/ADC/RC routing is not implemented. Work on a specifically allocated foundation
package (graph adapter, clock/reset, IRQ, GDMA, GPIO/mux, RC or ADC), not all shared
SoC files simultaneously. Publish tracked patches against pinned official QEMU,
with native firmware/MMIO positive/error/reset/concurrency evidence. Preserve
unknown floating/indeterminate states and explicit solver failure. No implicit
electrical supplies or arbitrary SPICE claims. Integrator owns machine wiring,
shared headers/application and final build. No physical stimulus without approval.
~~~

## Radio/SIMD/reference worker

~~~text
Read docs/handoff/06-radio-simd-hardware.md and radio-simd plan.
Unmodified Wi-Fi currently stalls in RF/SAR calibration; BLE has separate LP-clock/
RF-memory failures. Preserve traces; do not intercept APIs or force DONE.
SIMD has 84 actual QEMU vectors across seven forms; hardware comparison/full ISA
remain pending. Own allocated fixtures/acquisition/parser tests only; coordinate
deployment runner changes. 14 acquisition and 14 deployment mocks pass.
COM5 flash approval is unanswered. You may build/run QEMU and use mocked tests,
but may not write/reset/capture physical hardware on assumed authorization.
Pin all profile/blob/ELF/flash/tool hashes, preserve partial evidence, and retain
all oracle differences. Follow prerequisite power/clock/crypto/analog gates.
~~~

## Required worker report

~~~text
Scope and files:
Current reproduced behavior:
Change and contract implications:
Actual tests and exact command/profile:
Evidence/artifacts:
Known failure or remaining boundary:
Next testable task and dependency:
Active processes/builds:
~~~

Use spaces and normal sentences in coordination messages. A short accurate report
is more useful than an inflated support claim or a vague list of future intentions.
