# ESP32-S3 simulator takeover packet

Snapshot date: **2026-10-07, Asia/Shanghai**. This packet describes work already
done, exact evidence, outstanding implementation and how another agent can resume.
The complete simulator goal is **active and unfinished**.

**Current continuation:** [09-takeover-2026-10-07.md](09-takeover-2026-10-07.md).
Read it first: it supersedes this frozen packet's launch blocker, unanswered flash
approval, SIMD comparison and worker-state entries. Files 01–08 retain the original
snapshot and source/evidence context, not live status.

## Read in this order

| Document | Purpose |
| --- | --- |
| [01-current-state.md](01-current-state.md) | User requirements, repository identity, source lanes, delivered state and actual blockers |
| [02-wsl2-runbook.md](02-wsl2-runbook.md) | Windows/WSL paths, commands, environments, builds, tests and process recovery |
| [03-contracts-and-ownership.md](03-contracts-and-ownership.md) | Public interfaces, ownership boundaries and integration rules |
| [04-ux-editor.md](04-ux-editor.md) | Carbon UI/project editor implementation and precise next tasks |
| [05-qemu-and-electrical.md](05-qemu-and-electrical.md) | QEMU barrier launch failure, patch order, DC solver and hardware foundation tasks |
| [06-radio-simd-hardware.md](06-radio-simd-hardware.md) | Native radio failures, SIMD evidence, COM5 preparation and approval boundary |
| [07-validation-and-task-register.md](07-validation-and-task-register.md) | Evidence matrix, fine-grained next steps and completion criteria |
| [08-agent-prompts.md](08-agent-prompts.md) | Ready-to-use coordinator and worker prompts |

The detailed **full-scope** plan remains authoritative:
[master plan](../development-plan.md), [UX](../plans/ux.md),
[peripherals/electrical](../plans/peripherals-electrical.md),
[radio/SIMD](../plans/radio-simd.md), [review](../plan-review.md), and
[coverage manifest](../plan-coverage.json). Its 30 functionality rows, 119 work
packages, 15 acyclic milestones and 5 shared gates passed structural verification.
This packet supplies operational detail; it does not replace or shrink that plan.

Machine-readable snapshot: [state.json](state.json). It records source identity,
the current failure, completed verification, prepared patch order and unanswered
hardware approval. It is a dated snapshot, not live state.

## Critical facts before taking ownership

* Actual repository is the nested Project_ESP32S3VM directory. Everything is local
  and uncommitted on codex/simulator-foundation; a fresh clone will lose the work.
* Espressif's official fork is the selected base. The pristine baseline, recovered
  custom core and new extension checkout are three distinct lanes.
* Electrical routing must include analog voltage and ADC behavior. Saved wires
  and a passing standalone solver do not establish native pin connectivity.
* The current enabled hostbus test fails **before QMP**, because the QAPI object
  type needs registration. A corrective patch is prepared; timing is unqualified.
* COM5 original flash was read and backed up. **No test firmware was flashed.**
  User approval to flash SIMD/all images is still pending.
* The latest UI integration now passes the rebuilt **9/9 required regression**,
  0 skipped, 38.45 seconds. Fresh Light/Dark previews were inspected; remaining
  visual/product limitations are recorded separately from test success.
* A 30-minute heartbeat, Resume ESP32-S3 simulator, remains ACTIVE in the original
  chat. Goal/automation state was not paused or completed by this documentation
  request. Coordinate ownership with that chat before concurrent edits.

## Suggested takeover sequence

1. Read the packet and inspect live Git/process/agent state.
2. Assign one integration owner; allocate disjoint file scopes to workers.
3. Preserve the existing uncommitted tree and required ignored local artifacts.
4. Reproduce the currently documented failure before changing its assertions.
5. Finish the UX shared verification and enabled QEMU probe, then proceed through
   clock/reset/IRQ/GDMA/pad foundations and the reviewed peripheral/radio/ISA gates.
6. Update evidence and handoff documents after each meaningful completed slice.

An incoming agent's first report should name its exact file scope, current
reproduced result, next testable change and any dependency. It must not claim full
peripheral/radio/SIMD support from these foundation checks.

Fresh handoff previews (2026-10-08 rebuild): [Light](assets/light.png),
[Dark](assets/dark.png). The right-hand table header clipping was stale-binary
skew (the pre-fix binary had been captured); the frozen source was already
correct. Rebuilt shared lane verified 10/10 CTest, 0 skipped; fresh captures
show both headers fully rendered in both themes.
