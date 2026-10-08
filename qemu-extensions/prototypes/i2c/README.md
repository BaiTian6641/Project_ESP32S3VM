# ESP32-S3 initial native I2C master lane

This lane adds two native S3 controllers at `0x60013000` and `0x60027000`.
It does not modify the aggregate runtime profile, normal UART transport, Qt,
physical-board workflows, or recovered I2C bridge sources. The recovered bridge
is reference evidence only: cached RX maps, START-as-STOP, and stderr transport
are not used.

## Supported implementation range

* Seven-bit master command execution: START/address/write/read/repeated START,
  final read ACK/NACK, STOP, and END continuation. Eight command registers use
  the pinned IDF 6.1 S3 opcode encoding and DONE bit31. Separate 32-byte RX/TX
  FIFOs, refill/drain watermarks, counts/pointers, overflow/underflow, and
  RAW/ENA/status/CLR interrupt side effects are native state, not init logs.
* Virtual-time byte completion derives from live XTAL/RC_FAST clocks,
  CLK_CONF integer/fractional divider, SCL low/high/wait periods, and START/STOP
  timing. Both SYSTEM gate bits and held reset levels are wired. Gate/reset or
  source/divider changes cancel outstanding work; canceled work cannot later
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
  authority. Address decoding happens only after SDA and SCL share the actual
  endpoint nets. Missing address, disconnected matching address, insufficient
  power, duplicate address, missing pull, and stuck lines are not favorable
  defaults. Each byte rechecks reachability/power. Electrical notifications
  invalidate lost endpoints and release native SDA/SCL drivers.
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

The official base is `40edccac415693c5130f91c01d84176ae6008566`. The frozen combined
v3 prefix is fingerprint `105a156e5dec5fc6`; GPIO, ADC and NativeNet's actual
project/DC/RC providers are additional required inputs. `prefix.json` records
the isolated dependency checkpoint; `dependencies.json` records copied source
hashes. `source-map.json` describes lane copies, provider copies, and affected
QEMU paths. `dependencies-restack.patch` handles the three overlapping
GPIO/ADC machine/meson hunks against that combined prefix. `integration.patch`
is the reproducible isolated bootstrap, including shared prerequisites.
`electrical-prerequisites.patch` records that bootstrap's common foundation;
`integration-after-electrical.patch` is the **I2C consumer-only additive overlay**.
The aggregate must use only that additive overlay after NativeNet's common
electrical/kernel/meson freeze and CoreClockWorker's centralized named ports.
It binds the existing electrical child and does not create another graph,
compile another DC kernel, or add clock enum definitions. Published clock
source/gate/reset IDs are authoritative; no APB/PLL frequency alias is assumed.
The native S3 master build entry replaces the unused S3 ESP32-controller entry
without changing the original ESP32 controller.

Prepare a **new** isolated checkout under WSL Ubuntu:

```sh
python3 qemu-extensions/prototypes/i2c/prepare.py
```

Existing destinations are preserved. The default owned checkout is
`~/.cache/esp32s3vm/qemu-i2c-40edccac4156`; configure/build in its `build-i2c`
directory with BUILD_JOBS=4. The common DC kernel is compiled exactly once in
hw/adc; the ADC provider and I2C bridge use the same NativeNet graph/solver.
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
`stuck_scl`, `power_disconnected`, `power_undervoltage`, and
`power_overvoltage`. Each run creates a fresh scoped evidence directory with
UART, stderr, graph snapshots, QMP transcript, command, status and hashes. Its
90-second host watchdog diagnoses a stalled run; it never injects a firmware
bus timeout. Service GLib tests and `esp32s3-i2c-test` exercise genuine state,
FIFO/error boundaries, cancellation, IRQs, time and actual graph routes.

The changed service-only executable passed **18/18** cases in delegated
verification (the original14 plus external register-bank dispatch boundaries):
`build-runtime-state/i2c-native-2026-10-07/external-service-20261007T090614Z-f87cb4fc/`.
The earlier14-case proof remains preserved in its original scoped directory.
Coverage includes every missing callback/NULL ops, actual register mutations,
pointer wrap, START/restart/STOP, address/data/read failures, final NACK,
cancel preservation, physical power reset and readiness deadlines.
This is service-state evidence only, not native controller/electrical/camera evidence.
Full native qualification remains blocked on the shared GPIO OEN/selector,
electrical observer/power-tracker and ADC/radio single-owner provider freeze.
Initial isolated configure/build found and reported missing frozen hostbus
copies, then native math/GString API defects; those source prerequisites were
repaired. The ordinary IDF build stopped at its host watchdog without a reported
firmware/compiler failure. No native fixture, QTest or boot/control pass is
claimed until the actual consolidated binary is exercised.

## Canonical gates not silently marked complete

Native slave,10-bit/general-call addressing, multi-master arbitration, edge/fast
path equivalence and replay, glitch filter waveform behavior, slow analog RC
rise/recovery, inverted or direct IO_MUX routing, different input/output pads,
non-FIFO RAM mode, asynchronous queued multi-batch transfers, EEPROM write
protection/endurance, and external environmental or hostbus device services
remain separate canonical gates. The initial timed
transaction fast path does not generate or qualify every individual I2C bit
edge. Timing, routing and edge-equivalence qualifications are distinct.
