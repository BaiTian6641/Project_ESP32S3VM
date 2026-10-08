# QEMU host-bus barrier prototype design

Design snapshot: 2026-10-07, review tag 773C. **Enabled prototype execution is
executed-qualified for the controlled probe only**: the five reviewed patches
(including `qom-options.patch`) are applied through the guarded helper, and on
binary `91a7f2dee620c5ef4d6e04f304e3c2f123644bbef3efa11e8f1470da37bf0238`
(pinned base `40edccac4156...`) the seven Qt-peer scenarios pass twice, the
31-check raw fault suite passes, restore-denial and startup-restore rejection
tests pass, and the dual-core heartbeat plus prototype-disabled baselines pass.
The first enabled launch historically failed before QMP because pinned QEMU's
QAPI `ObjectType`/`ObjectOptions` schema lacked the prototype;
`qom-options.patch` supplies that registration and is applied.
This is still an M2 scheduler/chardev experiment, not peripheral fidelity.
The execution baseline is Espressif's annotated release
`esp-develop-9.2.2-20260417`, peeled commit
`40edccac415693c5130f91c01d84176ae6008566`. The original
`build-qemu-official-base` checkout and binary remain pristine.

The prototype will be published as GPL-2.0-or-later source under
`qemu-extensions/prototypes/hostbus`. The integration owner applies reviewed
source/build changes to a separate extension checkout and records their hashes.
No firmware MMIO register, replacement IDF driver or ELF-symbol hook is introduced.

## Findings from the pinned implementation

| Pinned source | What it establishes |
| --- | --- |
| `include/qemu/timer.h`, `timer_new_full` | Default virtual timers execute on the vCPU thread in icount mode; other timer lists execute in their owning AioContext |
| `include/block/aio.h`, `aio_timer_new` | An explicit timer can be attached to the main AioContext's timer list |
| `accel/tcg/tcg-accel-ops-icount.c`, `icount_get_limit`/`icount_prepare_for_run` | CPU budgets include global virtual deadlines; zero budget notifies Aio contexts |
| `accel/tcg/tcg-accel-ops-rr.c` | Single-thread TCG serializes both CPUs; instruction-count mode divides its budget across them |
| `system/cpus.c`, `vm_stop` | In a vCPU thread it queues a stop and returns, with an explicit FIXME about returning to device code; this is not an established global barrier |
| `system/cpus.c`, `pause_all_vcpus`/`do_vm_stop` | A main-context stop disables virtual clock and pauses all CPUs, then notifies stopped state; block drain/flush is performed and must be measured |
| `system/cpus.c`, `vm_prepare_start` | Central QMP/GDB start paths reach this function before CPU ticks and resume; a blocker must be checked before its state/events change |
| `monitor/qmp-cmds.c`, `qmp_stop`/`qmp_cont` | A second stop while already paused does not necessarily produce an observable VM-state notification; `cont` calls `vm_start` |
| `gdbstub/system.c`, `gdb_chr_event`/continue helpers | Debugger attach and continue use run-state functions; an attach while already paused cannot be distinguished using a VM-change callback alone |
| `chardev/char-fe.h` | `qemu_chr_fe_write_all` blocks; the prototype must use partial nonblocking writes and an output watch |
| `qobject/json-parser.c` | The pinned JSON parser already rejects duplicate keys; explicit UTF-8/NUL/size and envelope validation are still required |

Primary source references are the
[pinned QEMU tree](https://github.com/espressif/qemu/tree/40edccac415693c5130f91c01d84176ae6008566),
[timer API](https://github.com/espressif/qemu/blob/40edccac415693c5130f91c01d84176ae6008566/include/qemu/timer.h),
[CPU run-state implementation](https://github.com/espressif/qemu/blob/40edccac415693c5130f91c01d84176ae6008566/system/cpus.c),
and [round-robin TCG](https://github.com/espressif/qemu/blob/40edccac415693c5130f91c01d84176ae6008566/accel/tcg/tcg-accel-ops-rr.c).
Upstream [QTest 9.2 documentation](https://qemu.readthedocs.io/en/v9.2.4/devel/testing/qtest.html)
is a test-interface reference, not authority over Espressif-specific changes.

## Prototype boundary and architecture

Use a `UserCreatable` QOM object named `esp32s3-hostbus-probe`, attached through
`-object` and a dedicated socket chardev. It needs no machine MMIO mapping or
dynamic sysbus support. QMP/QOM arms and inspects a scheduled dependency; normal
firmware runs independently of that control interface.

The peer protocol is the existing
[host transport v1.0](bus-transport.md) and [envelope schema](bus.schema.json):
four-byte big-endian body length, bounded UTF-8 JSON, host-first hello, exact
session/connection/generation, decimal uint64 counters, phase/payload/correlation
checks. `phase_statuses` must carry exactly one entry for the fixed one-phase
request (review tag 773C); empty or multiple entries fail the barrier closed.
The QEMU object supports one pending probe and declares only the specific
transport features it handles. Handshake is not native bus qualification.

Implemented barrier path (differs from a naive in-callback stop, which
self-deadlocks on the virtual clock's per-timerlist done event): the armed
main-loop virtual timer fires in whichever thread reaches the deadline first;
the serialized vCPU budget has already pinned the clock at `armed_ns`; the
callback claims the barrier and sets the vCPU stop flags (`cpu_pause`) before
any later budget window can prepare; a bottom half on the main loop then runs
`vm_stop` outside the timerlist run. A deferred bottom-half warp covers the
idle-WFI-guest case where arming happened while the machine was paused.

1. The main AioContext owns a `QEMU_CLOCK_VIRTUAL` dependency timer. A QOM arm
   operation schedules a positive delay relative to the observed virtual clock.
2. Fixed icount and serialized TCG stop CPU instruction budgets at that deadline
   while notifying the main AioContext. The callback must assert it is outside a
   vCPU thread and operates with the required BQL/global-state context.
3. Establish the global barrier using `vm_stop(RUN_STATE_PAUSED)` in that context.
   Record the actual stopped virtual time after all CPUs have stopped. Only then
   send the external request and start a separate realtime host watchdog.
4. The chardev and QMP sockets continue processing through the main event loop.
   `QEMU_CLOCK_REALTIME` watchdog expiry reports host-service failure while keeping
   virtual time stopped. It does not manufacture a guest hardware timeout.
5. A valid response supplies bytes and a modeled completion time. The object marks
   the dependency resolved. It **does not call `vm_start`**.
6. An explicit QOM release clears the resume blocker and schedules any known
   modeled-completion timer. The caller separately issues QMP `cont` if it wants
   execution; ordinary pause/debugger intent therefore remains intact.

Use `-accel tcg,thread=single -icount shift=0,align=off,sleep=off` for the first
exact-time qualification. Verify both S3 CPUs participate, and that the stopped
time equals the scheduled time for this profile. Other shifts/adaptive time,
MTTCG, hardware acceleration and migration/replay are not implicitly qualified.
The timer API uses signed 64-bit deadlines: reject times/delays beyond its domain
rather than narrowing the protocol's uint64 timestamps silently.

## Central resume blocker is required

Stopping the machine once is insufficient: external QMP/GDB `cont` or step could
otherwise restart clocks while a request is unresolved. A reviewed minimal hook
must check a prototype resume-blocker registry at the start of `vm_prepare_start`,
before clearing stop requests, emitting RESUME, enabling ticks or changing run state.
All registered unresolved/error barriers must permit release before starting.

Do not recursively call `vm_stop` from a start VM-change notifier. Notifiers cannot
veto starting, are invoked after state/event changes and may be reentered; using
them that way would notify other devices with inconsistent run state.

Prototype behavior for a blocked `cont` must be explicit: no CPU/tick progress or
RESUME event, and inspection still reports paused/blocked. QMP's existing void
`vm_start` path may acknowledge the command without starting; an integration patch
can supply a clear QMP rejection once the common blocker API is agreed. GDB partial
continue/step also reaches `vm_prepare_start` and must be included in testing.

Manual release is deliberate. Safe automatic resume would additionally require
an explicit control-intent generation or lease, recording idempotent user stop,
debugger attach/interrupt and resets. Merely remembering `wasRunning` is unsafe;
current notifiers cannot recover those invisible intents. Automatic resume is a
separate design/acceptance gate.

## Lifecycle and ownership

* The object owns both timers, partial-write buffer/watch, frontend callbacks,
  pending request identity and its resume-blocker registration.
* Failed negotiation, malformed frames, disconnect and host watchdog leave an
  established dependency barrier stopped/error. Explicit cancellation/reset
  disposes pending work; no failure callback automatically resumes CPUs.
* System reset cancels dependency/completion timers, invalidates pending IDs and
  detaches/reconnects the device channel. The host owns wire reset/topology epochs;
  QMP reset observation must advance the host generation before the new handshake.
  Initial pre-handshake board reset must not falsely require an already advanced
  host epoch.
* Old connection/epoch replies are discarded and cannot release a new barrier.
  Counters and parser/queued storage have fixed limits; no log/UART mixing.
* Object deletion while armed/blocked is refused. Finalization unregisters timers,
  reset handlers, chardev watches and blocker state without resuming execution.
* Migration/snapshot restore denial is implemented by preflight guards and is
  runtime-tested: repeated HMP `loadvm`, QMP `snapshot-load` and QMP
  `migrate-incoming` rejections leave the stopped barrier's phase, clock,
  runstate, jobs and CPU registers unchanged, and startup `-loadvm`/`-incoming`
  abort with the probe-specific reason. The migration blocker alone would not
  establish these guarantees. QTest clock driving is separately checked;
  it must not be mistaken for proof of two executing TCG CPUs.

## Acceptance before promotion

| Scenario | Required evidence |
| --- | --- |
| Host compatibility | Real compiled Qt `PeripheralTransport` negotiates with the actual QEMU object; framed request and response bytes/context validate against v1.0 |
| Exact stopped time | Dual-core fixed-shift serialized TCG reaches an armed virtual time and records exactly that deadline, then stays unchanged during several different host delays |
| Responsive control | QMP query/QOM inspection and quit remain bounded/respond while the barrier and realtime host watchdog are active |
| Resume protection | QMP cont and GDB continue/step cannot advance clock/CPU while pending; no spurious RESUME event |
| Pause ownership | Response, watchdog, disconnect and stale reply never auto-resume; explicit release alone also leaves the machine paused |
| Modeled completion | After explicit release+cont, a future completion fires at its supplied virtual time, independently of prior host latency |
| Failure/reset | Wrong IDs/generations, duplicate/reordered/oversized/truncated replies, disconnect, object removal, reset during each stage and watchdog produce bounded observable states |
| Baseline | Existing official boot/control fixture remains green; prototype-disabled build changes do not alter native peripheral behavior |

Store source/build/patch hashes, exact command line, CPU count/state, arm/stopped/
completed timestamps, host delay, all QMP events and raw protocol frames. Repeat
identical scripted scenarios with fast/slow hosts and compare recorded virtual
events. This would qualify this controlled dependency probe only. Native I2C/SPI,
GPIO/analog routing, live networking determinism and whole-machine replay remain
separate gates.

## Frozen takeover checkpoint

All owned source, patches and test files are frozen at this checkpoint. Continue
in WSL2 Ubuntu; do not rebuild or patch the pristine official source/binary. The
integration owner maintains the separate native checkout
`/home/polar/.cache/esp32s3vm/qemu-extension-40edccac4156`, branch
`codex/hostbus-prototype`, and the `build-hostbus` build beneath it. Its executable
is `build-hostbus/qemu-system-xtensa`. Existing parent-owned apply/build helpers
verify source hashes and the locked base before mutation; use those helpers rather
than applying patches a second time by hand.

Copy the following public GPL-2.0-or-later implementation files. Test processes
and patch-generation helpers are separate files; they do not link into QEMU.

| Prototype file | Extension destination |
| --- | --- |
| `hostbus-probe.c` | `backends/hostbus-probe.c` |
| `hostbus-probe.h` | `include/sysemu/hostbus-probe.h` |
| `hostbus-json.h` | `backends/hostbus-json.h` |

Apply these patches in order, recording their SHA256 and all changed source
hashes in the integration manifest:

1. `integration.patch`: build inclusion and the early `vm_prepare_start` blocker.
2. `gdb-execution-preflight.patch`: reject execution commands before PC, signal,
   stepping or replay mutation in `gdbstub/gdbstub.c`; preserve its existing LGPL
   license/header. Plain uppercase `S` is unsupported in this pinned dispatcher;
   supported signal-step is `vCont;S`.
3. `state-restore-guard.patch`: reject HMP `loadvm`, QMP `snapshot-load`, incoming
   migration and central snapshot/incoming paths before their relevant runstate,
   job, storage, policy or transport mutation. Changes are confined to
   `migration/savevm.c`, `migration/migration.c` and
   `migration/migration-hmp-cmds.c`; upstream license headers remain intact.
4. `qom-options.patch`: register the CONFIG_TCG object type and typed `chardev` /
   optional string `watchdog-ms` creation properties in `qapi/qom.json`.

`source-map.json` lists all copies and four ordered patches. The parent appended
`qom-options.patch` and `qapi/qom.json` during takeover documentation. The QAPI
patch is still unapplied in the cached checkout and untested at runtime. The
initial integration patch is already applied to the native extension and must
retain its original hash. Additional source/patch changes require the guarded
helper's explicit hash reconciliation, not an implicit checkout reset.

QOM controls are `control=arm:<positive relative decimal ns>`, `release` and
`cancel`. Readable properties are `phase`, `resume-blocked`, `virtual-ns`,
`armed-ns`, `stopped-ns`, `delivered-ns`, `last-error`, `last-payload`,
`late-replies`, and `cpu-step-flags` (`cpu-index:flags` pairs). No property
implements a guest peripheral. Each peer connection needs a never-used UUID;
the prototype stores at most 256 accepted connection UUIDs before requiring a
new object. Same-session epoch/topology floors persist across reconnects; a
system reset at maximum epoch requires a different session. `late-replies` is
cumulative telemetry; a separate consecutive discard limit fails at 256 and
resets with valid traffic/reset/reconnect. The fixed probe requires negotiated
frame capacity of at least 1024 bytes.

Response `error_message` follows the Qt runtime limit of 1024 **UTF-16 units**,
not a byte count. The QEMU helper counts pinned modified-UTF8 code points as one
BMP unit or two supplementary units. JSON Schema's ordinary `maxLength` counts
Unicode code points, so its structural limit is an upper bound; runtime UTF-16
validation additionally rejects supplementary strings exceeding the Qt limit.
The strict prescan forbids single-quoted strings and nonstandard escapes, limits
nesting to 64, and delegates full grammar/duplicate-key validation to QEMU.

## Commands and evidence at handoff

Run these commands from WSL2. Paths below are the current workspace, and every
test copies its input flash to an isolated output directory.

`probe-test.py` writes the live UART stream to an independent native temporary
file beside its Unix-socket staging directory, keeping per-byte console writes
and boot-marker polling off DrvFS. After QEMU terminates, it exclusively creates
the usual `<scenario>/uart.log`, copies the raw bytes, flushes and synchronizes
the archive, then removes the live file. If archiving fails, the error identifies
the retained raw path; socket-directory cleanup cannot delete that file. The
90-second boot preflight and all dependency/barrier assertions are unchanged.

```bash
repo=/mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM
proto="$repo/qemu-extensions/prototypes/hostbus"
qemu=/home/polar/.cache/esp32s3vm/qemu-extension-40edccac4156/build-hostbus/qemu-system-xtensa
flash="$repo/tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.qemu_flash_4MB.bin"
rom="$repo/build-qemu-official-base/pc-bios"
bash "$proto/build-peer.sh" /tmp/esp32s3vm-hostbus-peer
python3 "$proto/probe-test.py" --qemu "$qemu" \
  --peer /tmp/esp32s3vm-hostbus-peer/hostbus-peer \
  --flash "$flash" --data-dir "$rom" --scenario delay-fast
```

After correcting any real first-case failure, run all scripted scenarios by
omitting `--scenario`, then run the raw fault suite:

```bash
python3 "$proto/raw-peer-test.py" --qemu "$qemu" --flash "$flash" --data-dir "$rom"
cc -std=c11 -Wall -Wextra -Werror "$proto/json-prescan-test.c" \
  -o /tmp/esp32s3vm-hostbus-peer/json-prescan-test
/tmp/esp32s3vm-hostbus-peer/json-prescan-test
```

Actual completed checks (evidence under
`build-runtime-state/hostbus-run-02/` and
`build-runtime-state/hostbus-review-2026-10-07/`; review tag 773C binary
`91a7f2de...`):

* Host transport's isolated Qt suite passed 69 checks, zero failures/skips.
* The real Qt peer compiled independently with that transport, including the
  reproducible `build-peer.sh` build.
* The standalone strict lexical/depth suite passed 24 checks under
  `-Wall -Wextra -Werror`.
* All five ordered patches (integration, gdb-execution-preflight,
  state-restore-guard, qom-options, creation-order) were applied through the
  guarded helper with hash verification; `qom-options.patch` was previously
  unapplied and is now applied and runtime-proven (`-object` accepts
  `esp32s3-hostbus-probe`).
* Enabled execution is qualified for the probe: the seven Qt-peer scenarios
  (delay-fast, delay-slow, watchdog, disconnect, reset, released-reset,
  gdb-raw-step) pass, twice on the pre-review binary and once on the 773C
  binary, with `armed-ns == stopped-ns` exact, modeled completion at the
  supplied virtual time, responsive QMP/GDB during the barrier, and no
  auto-resume.
* The raw fault suite passes 31 checks: generation floors, UUID reuse,
  malformed JSON, UTF-16 bounds, stale-response limits, incomplete/oversize
  frames, empty and multiple `phase_statuses` (tag 773C), wrong
  endpoint/controller/net context, reset while armed (stale timer cancelled,
  step flags clean, no auto-resume) and MAX-epoch session rotation.
* Restore denial is executed: repeated HMP/QMP/incoming rejections with
  unchanged barrier state and startup `-loadvm`/`-incoming` rejection.
* Dual-core participation is proven by pinned per-core heartbeats
  (iterations 1/2/3 on both cores) before and through a barrier.
* The prototype-disabled baseline (same binary, no probe object) boots both
  firmware images and passes stop/cont/system_reset control.

Remaining gates are outside this probe's scope and stay ordered: whole-machine
replay and CPU cycle accuracy, native GPIO/I2C/SPI/analog routing, live
networking determinism, and the parent-owned shared lanes. The first enabled
smoke's pre-QMP failure artifacts remain in
`build-runtime-state/hostbus-first-run` for history; the QAPI registration gap
it exposed is fixed and runtime-proven.
