# QEMU scheduling, electrical and peripheral takeover

Read [the native prototype contract](../contracts/qemu-hostbus-prototype.md) and
[the peripheral/electrical workstream](../plans/peripherals-electrical.md).

## Actual current blocker

The separate extension builds. Ordinary IDF 6.1 boot with the prototype disabled
passed. The first enabled delay-fast run exited before QMP:

~~~text
Parameter 'qom-type' does not accept value 'esp32s3-hostbus-probe'
~~~

Artifacts: build-runtime-state/hostbus-first-run/delay-fast. This is a real failure,
not a passing virtual-time test. QEMU 9.2's -object uses typed QAPI ObjectOptions;
QOM type registration alone is insufficient.

qom-options.patch now supplies the CONFIG_TCG ObjectType entry, typed chardev
string/optional watchdog-ms string and ObjectOptions arm. Its pristine
applicability check passed. Parent appended it to source-map.json and the guarded
apply whitelist. It has **not been applied/rebuilt/executed** at this snapshot.

## Source mapping and patch order

Copies:

| Tracked prototype input | Cached QEMU destination |
| --- | --- |
| hostbus-probe.c | backends/hostbus-probe.c |
| hostbus-probe.h | include/sysemu/hostbus-probe.h |
| hostbus-json.h | backends/hostbus-json.h |

Ordered patches:

1. integration.patch: backends/meson.build, system/cpus.c.
2. gdb-execution-preflight.patch: gdbstub/gdbstub.c.
3. state-restore-guard.patch: migration/savevm.c, migration/migration.c,
   migration/migration-hmp-cmds.c.
4. qom-options.patch: qapi/qom.json.

Never edit the pristine official lane. The apply script verifies existing deployed
hashes before refreshing owned copies and only applies a newly added common patch
to pristine previously unmodified paths. Changing an already applied patch requires
a fresh profile/reviewed reconciliation, not removing the guard.

## Probe design and repaired review findings

This is a QOM-only UserCreatable experiment, not guest MMIO or native GPIO.
A main-AioContext virtual timer reaches a scheduled dependency. Serialized TCG
icount must stop at that deadline; the main callback calls vm_stop, records exact
stopped time and sends a framed request to the actual Qt transport.

Use -accel tcg,thread=single and -icount shift=0,align=off,sleep=off. Default virtual
timers under icount can execute in vCPU context and are unsuitable for the intended
global stop. BQL/vCPU context assertions are present.

A central vm_prepare_start guard blocks execution while the dependency lease is
owned. GDB execution preflight runs before optional PC, signal, single-step or
replay mutation. Raw c/s commands must be tested; replacing them with vCont to hide
a leaked step flag is unacceptable.

Response resolves the lease. Explicit QOM release clears the blocker and schedules
modeled completion; a separate cont starts execution. No response/watchdog/reset/
disconnect handler calls vm_start. REALTIME watchdogs report host-service failures,
never fake modeled hardware timeout.

Review corrections now in tracked source include strict double-quoted/depth-64
JSON, UTF-16 error-text bounds, bounded consecutive stale discards separate from
cumulative telemetry, exact request context, fresh connection UUID registry,
same-session generation floors and a new-session latch at exhausted epoch.
Reset cancels released future completions and invalidates timestamps/payload.

Restore guards reject HMP loadvm, QMP snapshot-load and incoming requests before
runstate/job/policy mutation, plus central load_snapshot/incoming paths.
migrate_add_blocker alone did not establish these guarantees. Their actual
execution tests are still required.

## Exact next execution

~~~bash
cd /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM
bash tools/build-qemu-hostbus-prototype-wsl.sh
repo="$PWD"
proto="$repo/qemu-extensions/prototypes/hostbus"
qemu=/home/polar/.cache/esp32s3vm/qemu-extension-40edccac4156/build-hostbus/qemu-system-xtensa
flash="$repo/tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.qemu_flash_4MB.bin"
rom="$repo/build-qemu-official-base/pc-bios"
bash "$proto/build-peer.sh" /tmp/esp32s3vm-hostbus-peer
python3 "$proto/probe-test.py" --qemu "$qemu" +  --peer /tmp/esp32s3vm-hostbus-peer/hostbus-peer --flash "$flash" +  --data-dir "$rom" --output "$repo/build-runtime-state/hostbus-qapi-run" +  --scenario delay-fast
~~~

Output must be new; choose another explicit name if it exists. After the first
case truly passes, omit --scenario for the full Qt-peer scenario set. Run the
standalone lexical suite and raw peer harness as documented in the contract.

## Fine-grained barrier tasks

| ID | Work | Required result |
| --- | --- | --- |
| H-BUS-01 | Apply QAPI patch, rebuild and reproduce enabled launch | Probe creates and negotiates; preserve first failure artifacts |
| H-BUS-02 | Debug real timer/context/runstate failures | Armed == stopped; clock stays fixed while QMP remains responsive |
| H-BUS-03 | Test raw GDB c/s/PC/signal/vCont/reverse commands | Rejected commands preserve PC/step flags; no RESUME; explicit release+cont reaches completion |
| H-BUS-04 | Run fast/slow host delay and modeled future completion | Completion at supplied virtual time; host delay does not advance blocked clock |
| H-BUS-05 | Reset/disconnect/watchdog/released-completion lifecycle | Old callbacks/timers cannot resolve/fire in new generation; no auto-resume |
| H-BUS-06 | Repair raw harness observation boundaries | shutdown tolerates remote close; inherited error phase cannot fake new bad-frame rejection |
| H-BUS-07 | Malformed/UUID/floor/MAX/depth/UTF16/stale/assembly faults | Bounded rejection after actual processing, no false positive or stale release |
| H-BUS-08 | Add restore-denial tests, including startup paths | Probe-specific rejection before state mutation; repeated requests leave phase/time/runstate unchanged |
| H-BUS-09 | Confirm actual dual-core participation and disabled baseline | Configured CPU count alone is insufficient; ordinary control/reset still passes |

Only the controlled dependency probe can be promoted after these results.
Whole-machine replay, CPU cycle accuracy, electrical routing and native buses are
separate gates.

## DC kernel and immediate native foundations

qemu-extensions/electrical implements bounded linear DC MNA: 64 nodes, 128 elements,
resistors, ideal voltage/current sources and finite-impedance drivers. No implicit
ground/supply/pull is inferred. Floating absolute voltages are NaN; relative currents
may be solved. Source conflicts, no return path and numerical failure are explicit.
See [the complete numerical contract](../contracts/electrical-dc.md).

Eight analytic/failure scenarios now pass 553 checks. Review repaired an impossible
external injection masked by a large internal circulation, and mislabeled numerical
overflow. Sanitizers passed the physical correction; latest normal tests include
the diagnostic correction. GPIO output impedances are explicit model parameters,
not characterized silicon values. There is no RC, native pad or ADC integration.

| ID | Foundation work | Acceptance |
| --- | --- | --- |
| H-NET-01 | Strict v3 graph → native primitive/terminal adapter | Actual IDs resolve; geometry never connects; unsupported topology fails explicitly |
| H-CORE-01 | Clock/reset ports and domain propagation | Gates/dividers alter progress; reset cancels correct timers/IRQs/state |
| H-CORE-02 | Interrupt matrix source OR/remap/mask behavior | Shared asserted sources cannot clear one another; both CPUs tested |
| H-CORE-03 | Persistent bounded GDMA descriptors/cursors/handshakes | Partial bursts/rings/ownership/errors/memory paths do not hang or cross channels |
| H-NET-02 | GPIO/IOMUX/matrix/RTC pad resolution + IRQ | External input and finite drivers affect actual register/interrupt behavior |
| H-ANALOG-01 | RC integrator/state/crossings with bounded error | Analytic reference/transient and reset/topology preservation pass |
| H-ADC-01 | Acquisition/controller/calibration/continuous sampling | Native drivers read modeled voltages; clock/GDMA/filter/arbitration/errata validated |

Follow CORE-01..05, NET-01..06, ANALOG-01..04 and ADC-01..05 in the full workstream
for register semantics, quantitative acceptance and dependencies. Implement these
before porting superficially complete custom peripheral models.
