# ESP32-S3 native I2C software lane

This lane adds two native S3 controllers at `0x60013000` and `0x60027000`.
Current I2C preparation preserves exact accepted controller/binding/service/test
input copies and applies reviewed physical-source corrections after their copies.
Normal UART transport, Qt, physical-board workflows and recovered bridge sources are unchanged. The bridge
is reference evidence only: cached RX maps, START-as-STOP, and stderr transport
are not used.

## Supported implementation range

* Seven/ten-bit master command execution and native slave byte/callback paths:
  START/address/write/read/repeated START, final read ACK/NACK, STOP and END
  continuation. Exactly eight command registers use the pinned S3 encoding and
  DONE bit31. Separate32-byte RX/TX FIFOs, refill/drain watermarks, pointers,
  overflow/underflow and RAW/ENA/status/CLR remain native state.
* Virtual-time physical data/ACK progression derives from live XTAL/RC_FAST,
  CLK_CONF integer/fractional divider, SCL low/high/wait and START/STOP periods.
  Both SYSTEM gate bits and held reset levels are wired. Gate/reset or
  source/divider changes cancel work; canceled work cannot later
  return bytes or completion IRQs. RC_FAST uses CoreClockWorker's nominal
  17.5MHz output with actual RTC_CNTL CK8M_FORCE_PD validity (0Hz when off),
  not an APB/PLL alias. The shared reset-level bundle is authoritative for
  held-reset write blocking; the SoC separately maps the reset strobe.
* Initial routing uses the actual GPIO matrix input/output selectors on the
  same physical pad, normal (non-inverted) input/output, GPIO IO_MUX ownership,
  enabled input buffer, valid output-enable selection, and an actual
  open-drain-capable path (raw PAD_DRIVER or intrinsic controller OD with
  peripheral-selected OE). Desired signals have one electrical API authority;
  they never override the guest's pad mode or install a second GPIO source store.
  SDA/SCL signal indices are90/89 for controller0 and92/91 for controller1.
  No controller/address metadata
  can substitute for a routed pad or wire.
* The NativeNet electrical graph is the connectivity, power, and line-state
  authority. Address bytes are decoded through actual SDA/SCL wire progression
  against the accepted normalized project-device address. Missing/disconnected
  endpoints, insufficient/unknown power, duplicate addresses, missing pulls and
  stuck lines are not favorable defaults. Actual rail/topology notifications
  cancel lost peers and release native SDA/SCL drivers.
* Stateful native SHT21 and M24C02-style EEPROM services are shared per stable
  component ID, not copied per controller. There is no external-service
  dependency for these two models, so no hostbus request or firmware barrier
  is fabricated. A future external service must use the existing bounded
  hostbus/barrier contract; stderr and UART JSON are not alternatives.
  The binding has128 stable service slots; external unregister reclaims its
  slot without moving any live controller reference. Exhaustion fails closed,
  rather than silently creating a response or growing an unbounded cache.

### Explicit service profiles

Built-in device types are `sht21` and `24c02`, with `kind: device`.
Their terminal roles are `sda`/`scl` (digital,inout), `vdd` (power,input), and
`gnd` (ground,input). `parameters.address` is a mandatory count quantity with
integer value1..127. Unknown types remain unsupported. The graph projection is
an explicit ideal idle-high-impedance/zero-idle-load service profile. Active
SDA/SCL low branches use40ohm to that component's actual GND; release inserts
no high driver. Pulls, supplies and rails must be explicitly wired. SHT21 power
range is2.1..3.6V; EEPROM is2.5..5.5V, measured between actual VDD/GND nodes.

Externally owned native models use the published `S3I2CServiceOps` wrapper.
All eight callbacks are mandatory: address, write, read, read ACK/NACK, STOP,
cancel, power reset and virtual readiness. Initialization failure produces an
inert wrapper, never an old addressable device. Register/unregister by the
actual preflighted component ID through `esp32s3_i2c_register_service`; there
is no controller or address registration argument. Registration cannot replace
a built-in device or steal another owner's opaque state. Routing, physical
SDA/SCL connectivity and known power are checked before external dispatch;
model-specific reset/PWDN/XCLK preflight stays in the external owner's address
callback. Unregister cancels matching active work and releases native line
drivers before the owner destroys its opaque state.

The approved `ov2640-dvp` body and preflight belong to LCD_CAM; its SCCB bank,
pointer, reset and PID bytes are actual owner state, not a copied RX table.
NativeNet owns its registered electrical routes and rails. These function-level
transactions do not qualify SCCB bit-edge fidelity or DVP capture.

The SHT21 model implements E3/F3 temperature and E5/F5 humidity commands,
resolution-dependent conversion deadlines, CRC8 polynomial0x31, E6/E7 user
register, and FE soft reset. No-hold premature reads NACK; hold-master reads
stretch SCL until the modeled deadline or the configured hardware timeout.
Samples are a deterministic per-conversion modeled ambient sequence with
resolution quantization. Values become readable at their conversion deadlines;
this is not physical thermal dynamics or an external environmental provider.
Heater bit storage does not claim heater thermal dynamics.

The EEPROM profile is256 bytes with a one-byte pointer and16-byte page wrap.
Page data becomes committed only on STOP, followed by a5ms write-cycle NACK
window. Repeated START reads see previous committed data; they do not synthesize
STOP or commit pending writes. Pointer-only STOP preserves the pointer. Cancel
discards uncommitted page data while retaining committed memory. This is the
M24C02-style16-byte-page profile, not every vendor's24C02 variant.

Actual registered-device rail state is queried independently of SDA routing
for lifecycle only, never to grant an ACK. Known power transitions and the
provider's availability epochs invalidate volatile service state. Unknown rails
remain unknown and cannot answer. SHT21 power-on readiness starts15ms after the
provider's actual `power_on_ns`, not the first address request. EEPROM power
reset discards its volatile pointer/staged page/busy state but keeps committed
nonvolatile memory. Wire-only disconnects do not reset a powered sensor.
The binding uses bounded multicast registration and unregisters its matching
callback on destruction, so subsequent peripheral lanes cannot replace it.

## Dependencies and source integration

The official base is `40edccac415693c5130f91c01d84176ae6008566`.
`foundation-dependencies.json` is the current frozen common recipe; it preserves
published NetIRQ/RMT/SPI-PSRAM inputs and applies the atomic I2C
controller/header/binding/fixture plus partial-validity corrections explicitly
after original copies, before the UART integration tail. Each shared clock,
GPIO, electrical/kernel and memory dependency is applied once.
`integration-after-electrical.patch` binds the existing electrical child; it
does not create another graph, DC kernel or clock definitions.
`prefix.json`, `dependencies.json`, `dependencies-restack.patch`,
`electrical-prerequisites.patch` and `integration.patch` describe the historical
isolated105a bootstrap, not the current default. Its retired stand-alone
preparer must not replace the common foundation or its later reviewed fixes.
Published clock source/gate/reset IDs remain authoritative; no APB/PLL alias.
The native S3 master build entry replaces the unused S3 ESP32-controller entry
without changing the original ESP32 controller.

Current guarded preparation uses the same native UART preparer and explicit
ordered source identities as other qualified peripheral families:

```sh
python3 qemu-extensions/prototypes/uart/prepare.py --dependencies qemu-extensions/prototypes/i2c/foundation-dependencies.json
```

Existing destinations/source records remain preserved. Actual current source
`3cc39bfdc7bd3515`/`19e0edb4` passes361 native with159 target hashes, I2C38/
service18, three powered OV SCCB tests and all21 ordinary outcomes:17 PASS,
four original ten-bit convenience failures. No partial/old-executable proof
inheritance. The common DC kernel is compiled once and the ADC/I2C providers
use that same NativeNet graph/solver.
ADC clock connections must precede realization, as included in the machine
integration patch. The electrical device has no invented MMIO.

## Acceptance and evidence

The ordinary fixture is `tests/firmware/i2c_native`, built against pinned IDF6.1.
It uses normal synchronous master probe/transmit/receive/transmit-receive APIs
on both controllers. Default pads are SDA8/SCL9 and SDA10/SCL11. Configure
100000 or400000Hz and the expected graph profile through its Kconfig; internal
pull-ups are deliberately disabled. Assertions consume actual returned bytes,
CRC, timed sample changes, EEPROM page/STOP semantics and transfers over32 bytes.

The public-QMP-only runner applies the actual graph while stopped, then runs
firmware with unmodified UART output:

```sh
python3 qemu-extensions/prototypes/i2c/run-native-fixture.py \
  --qemu BIN --flash merged.bin --sdkconfig build/sdkconfig --mode MODE \
  --evidence build-runtime-state/i2c-native-2026-10-07/
```

Modes are `connected`, `wrong`, `disconnected`, `no_pull`, `stuck_sda`,
`stuck_scl`, `power_disconnected`, `power_undervoltage`, `power_overvoltage`,
`slave`, and `slave_disconnected`. Each run creates a fresh scoped evidence
directory with
UART, stderr, graph snapshots, QMP transcript, command, status and hashes. Its
600-second host watchdog diagnoses an unfinished run; it never injects a firmware
bus timeout. The runner uses single-thread TCG/icount virtual time and waits for
a complete DONE/result line, not a partially flushed prefix. Service GLib tests
and `esp32s3-i2c-test` exercise state, FIFO/error boundaries, cancellation, IRQs,
time and actual graph routes.

The `slave` fixture uses ordinary `i2c_new_slave_device`, receive/request
callbacks, `i2c_slave_write` and `i2c_slave_reset_tx_fifo` on both controllers.
Physical graph masters clock65-byte receive/request transfers and verify that
49 discarded TX bytes never precede17 replacement bytes. Public GPIO12 input
is the host gate; reserve it outside the four SDA/SCL pads. After each complete
READY line, the runner pauses, applies actual powered peer terminals and the
gate pull, then resumes. It never injects data, callback results or register
completions. Response service precedes blocking UART reports.


The changed service-only executable passed **18/18** cases in delegated
verification (the original14 plus external register-bank dispatch boundaries):
`build-runtime-state/i2c-native-2026-10-07/external-service-20261007T090614Z-f87cb4fc/`.
The earlier14-case proof remains preserved in its original scoped directory.
Coverage includes every missing callback/NULL ops, actual register mutations,
pointer wrap, START/restart/STOP, address/data/read failures, final NACK,
cancel preservation, physical power reset and readiness deadlines.
This is service-state evidence only, not native controller/electrical/camera evidence.
Current consolidated candidate `1e937cf0ad1822bb`, executable SHA256
`6e2c78411f0ef48215ad1c8be3f23fd0eef0ccfe4ea151c891a357ef8c49347f`,
passes **18/18 native controller/edge cases** and **18/18 service cases**.
All127 applied source hashes match its immutable preparation receipt.

**Ten ordinary pinned-IDF scenarios PASS on both controllers**: connected
device transfers at100000/400000Hz, wrong/disconnected routes, no pull-ups,
stuck SDA/SCL, and disconnected/under-/over-voltage device rails. The pinned
SDK's probe API always programs100000Hz; negative probes do not inherit the
printed device-transfer frequency. Actual bytes, CRC/conversion timing,
EEPROM page/STOP semantics and transfers beyond FIFO depth are checked.
The same executable also passes UART64, UHCI17, GDMA18 and memory12 native cases
and all five strict ordinary UART/UHCI profiles: **147 native +18 ordinary PASS**,
zero native skips. Exact frozen inputs, reports and snapshots:
`build-runtime-state/uart-continuation/i2c-slave-final/qualification-receipt.json`.

The new software vectors exercise routed controller0 slave writes, clocked
read/relay bytes, served and unserved address stretching, one ten-bit write with
seven-bit-alias rejection, general-call enable/disable,100ns SDA glitch rejection,
and address-arbitration win/loss. Peers drive actual registered NativeNet
terminals; no RX FIFO injection or controller/address response table is used.

Three ordinary slave runs also PASS on both controllers: connected100/400kHz
and disconnected100kHz. The native continuous-stretch watchdog now retains its
original deadline across unrelated solved frames. GPIO toggles previously
postponed timeout indefinitely; the retained regression fails before the fix
and passes afterward. Scripted peer payloads are bounded at256 bytes so actual
FIFO32/software-buffer crossings can be exercised without a synthetic shortcut.


Corrections include route-OK enum handling (success is zero), physical
SCL-before-SDA sequencing and real STOP edges, address-ACK/data-bit separation,
matched final-NACK-to-STOP completion, initial glitch dispatch, observed-clock
address synchronization and loser release without a STOP. START/END ownership
and repeated START stay controller state. Malformed integer peer config now
rejects Apply without a crash or loss of queued bytes; the regression fails
against the preserved old executable and passes against the corrected one.

Earlier paused-harness and9-case master proofs remain historical under
`build-runtime-state/uart-continuation/i2c/`. These software subsets do not
close every canonical gate below.

## Canonical gates not silently marked complete

Exact silicon phase/latch/filter/metastability/analog-reference timing remains
unqualified. Four unmodified pinned-SDK convenience ten-bit17/17 programs
retain their original FAIL criteria; public explicit-operation coverage does
not replace them. Broader non-FIFO RAM, asynchronous queues, inverted/direct/
split routes, EEPROM endurance/protection, peer capacity/power-cycle replay,
environmental/hostbus and independent backend/replay equivalence remain open.
Current source emits real data/ACK edges; no controller byte/address shortcut
or invented ninth command masks those boundaries.

## 2026-10-10 parent-compatible physical source refinement

The final epoch10 candidate retains real wire/ACK ownership, normalized accepted
project addresses, stable-known-high SCL START/STOP recognition and one-shot
UNKNOWN filter expiry. Its private37 native/18 service/three OV SCCB and21
ordinary results remain scoped:17 PASS, four original pinned-SDK convenience
ten-bit17/17 failures. Public explicit-operation runs are additional coverage,
not replacements for those failed original criteria; no SDK shim or ninth
command slot.

Parent review additionally found that a valid SDA reversal was discarded while
SCL was UNKNOWN, retaining an old persistence age and accepting a new
subthreshold LOW as START. The fresh post-copy
`0003-valid-line-filter-persistence.patch` tracks independently valid candidates
at both published-frame and filter-expiry sampling without accepting a bit,
advancing the decoder or polling at1ns. Actual identical-fixture before/after
controls fail before / pass after; DC/RC subthreshold rejection and subsequent
real address/ACK/STOP/FIFO byte recovery pass. Affected native93 pass with zero
skips: I2C38/service18/GPIO19/electrical8/interrupt-matrix10.

Exact scoped proof:
`build-runtime-state/uart-continuation/i2c-parent-stage/partial-validity-qualification.json`.
Original accepted input copies and active camera authorities remain unchanged;
the physical controller/header/binding/fixture are applied atomically through
declared post-copy overlays. Canonical `wire`/`wire_defined` fixtures retain
callback context, task and buffers until successful device teardown; unsafe
cleanup reports failure and does not reuse them. Actual fresh SDK5.5.5/6.1
images and canonical runners now complete all21 ordinary outcomes on this same
source with17 PASS/four unchanged original FAIL; no host timeout is counted as
completed. Full361 native and all159 target hashes pass. The separate generic
SYSTIMER fractional-phase correction still requires its own fresh source/
native/ordinary proofs; none are inherited. No exact-silicon timing, full I2C
or simulator completion claim.
