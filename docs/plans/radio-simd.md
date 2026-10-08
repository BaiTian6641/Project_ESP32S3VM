# Wi-Fi, Bluetooth LE, SIMD, and hardware reference plan

Planning snapshot: **2026-10-07**. This document records proposed work and
acceptance gates; it does not promote existing radio or SIMD code to verified
support. The parent [development plan](../development-plan.md) owns delivery
sequencing, integration ownership, and release decisions.

## Scope, baseline, and evidence rules

The execution base remains Espressif's official QEMU fork, pinned to
`esp-develop-9.2.2-20260417`, commit
`40edccac415693c5130f91c01d84176ae6008566`. Recovered extensions at
`33c2bdd17104b3baeea3b788bafd27b2f921c13f` are candidates to port and qualify,
not a replacement for the official base. Preserve the working launcher, boot,
circuit, and I2C changes while doing this work.

The existing successful firmware baseline uses ESP-IDF `6.2-dev`, commit
`25fe69f946311abdaf9ad56591f25fedbc20ac98`. It proves boot/control and one native
I2C fixture, not radio operation or SIMD correctness. The full existing fixture
suite was rerun by the parent on October 7: **5/5 passed, 0 skipped, 8.49 s**.
COM5 is present in the host's serial-port inventory and has **not been opened**.

Current official stable ESP-IDF is **v6.1**. Stable release qualification starts
there, rather than treating the local development checkout as a production
compatibility claim. [IDF v6.1 release](https://github.com/espressif/esp-idf/releases/tag/v6.1),
[IDF version policy](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/versions.html)

The real S3 target has 2.4 GHz 802.11 b/g/n and Bluetooth LE. Classic Bluetooth,
5 GHz Wi-Fi, and IEEE 802.15.4/Thread are not S3 hardware requirements. BLE
certification/version labels do not prove that every later optional LE feature
is present in every controller release; query and qualify the exact capability
set. [S3 product description](https://www.espressif.com/en/products/socs/esp32-s3/),
[S3 BLE architecture](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html)

Every feature has these separate evidence states:

* **Catalogued (`catalogued`):** hardware/API semantics and required variants identified.
* **Implemented (`implemented`):** source path exists, with source/model unit checks.
* **Native fixture verified (`native-tested`):** ordinary ESP-IDF/Arduino driver or library executes
  against the model without application API interception or test MMIO hooks.
* **Hardware compared (`hardware-compared`):** the same identified fixture was compared with a real S3
  profile and a retained reference record.
* **Qualified (`qualified`):** supported version/profile combinations and failure cases passed
  the declared release matrix.

These are the canonical capability maturity values. Availability/reason for the
selected runtime/profile is a separate field; user-facing experimental/verified
labels must not promote maturity without evidence.

Synthetic MMIO, a replacement IDF component, ELF-symbol interception, a success
log line, or a fabricated completion interrupt cannot qualify native firmware
compatibility. They may be separately labeled development adapters. No claim of
all-version compatibility follows from testing one IDF branch or one board.

## Source audit that changes the plan

| Observation in recovered source | Implication and next investigation |
| --- | --- |
| `qemu/hw/net/esp32s3_wifi.c`, `slc0_dma_rx_run`, returns hardware-owned RX descriptors with zero length when started | Establish actual blob descriptor semantics and remove premature empty completion before qualifying asynchronous RX injection. |
| `qemu/hw/net/esp32s3_wifi_backend.c`, `build_auth_response`, returns successful Open System authentication; beacon capability/IE construction advertises an open ESS | Existing virtual AP is not evidence for protected association, EAPOL, key installation, or WPA2/WPA3 data traffic. |
| Wi-Fi source has frame conversion and virtual beacon/probe/auth/association paths but no native firmware Wi-Fi fixture | First run the vendor initialization path, then use real-driver scenarios to locate missing behavior. |
| `qemu/include/hw/misc/esp32s3_bt.h` explicitly defines QEMU-only transport registers at offsets `0xF00-0xFFF` | The HCI model is a bypass API. Real controller MMIO and initialization need a distinct native compatibility investigation. |
| `qemu/hw/misc/esp32s3_bt.c` uses one deterministic handle, immediately creates a connection, and returns SMP Pairing Failed | Per-peer timing/state, flow control, and security remain required. Advertised feature masks must match actual implementation. |
| `qemu/target/xtensa/translate_tie_esp32s3.c` has 246 named translator entries, including RUR/WUR operations | This is a source inventory count, not a verified count of SIMD instructions or complete ISA coverage. |
| Unsigned 20-bit MAC overflow assigns `-0xfffff` in recovered source near lines 1971/1973 | Packing the low 20 bits yields 1. This is a concrete differential test candidate; confirm expected saturation with TRM and hardware before correction. |

The same unsigned MAC expression occurs in the currently published official
[QEMU PIE translator](https://raw.githubusercontent.com/espressif/qemu/esp-develop/target/xtensa/translate_tie_esp32s3.c).
Official provenance alone is therefore not correctness evidence. Port review
must distinguish existing upstream behavior from extension changes.

Official QEMU documents virtual OpenETH networking with firmware configured for
that driver. Use it to isolate lwIP/backend regressions if useful, but it cannot
substitute for native Wi-Fi acceptance.
[Official S3 QEMU guide](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32s3/README.md)

## Compatibility and reproducibility matrix

| Tier | Firmware/toolchain selection | Qualification policy |
| --- | --- | --- |
| Release gate | Exact latest patch tags from supported IDF 5.3, 5.4, 5.5, 6.0, and 6.1 lines, resolved and frozen when building the matrix | All mandatory native fixtures pass for each supported applicable API/profile. v6.1 is the initial repair target; expand before claiming the release matrix. |
| Forward detection | Current installed 6.2-dev pinned commit, later deliberately refreshed commits | Nonblocking early detection while unstable APIs/blobs change; failures must still be recorded and classified. |
| Expanded historical | IDF 4.4 and intervening S3-supported lines, selected exact historical patch tags | Preserve regression inputs and work toward hardware compatibility. Historical toolchain/API adaptations are fixture build changes, not emulator-side firmware API hooks. |
| Arduino | Exact supported Arduino-ESP32 releases, their bundled IDF/toolchain/library versions, and normal compiled sketches | Boot, Wi-Fi, BLE, DSP/peripheral use through public Arduino APIs. Arduino qualification is separate from IDF examples. |
| Chip profiles | Actual acquired board chip/ROM revision first; other documented revisions added explicitly | A board comparison validates that profile. Record revision-specific ROM/eFuse/calibration/errata rather than claiming all silicon. |

Do not guess current patch numbers for every line. Resolve official tags, record
commit hashes and component submodule/library hashes, and freeze a manifest.
Each row includes:

* IDF tag/commit, compiler and binutils versions, sdkconfig, Arduino version if
  applicable, fixture source commit, ELF and complete flash-image SHA256.
* Wi-Fi, Bluetooth controller, PHY, DSP/DL library revisions and binary hashes.
  Vendor libraries are precompiled and may change undocumented hardware access
  independently of public application API compatibility.
* Official QEMU commit, extension patch manifest, host/tool versions, machine
  profile, ROM hash, chip revision, flash/PSRAM geometry, eFuse fixture hash,
  clock configuration, and initial NVS state.
* Native/adapted execution mode, expected events and error reasons, modeled
  network/peer setup, deterministic seed, observed results, and artifact paths.

Vendor library boundaries are explicit in the official
[Wi-Fi library repository](https://github.com/espressif/esp32-wifi-lib) and
[Bluetooth controller library repository](https://github.com/espressif/esp32-bt-lib).

Persistent NVS/calibration/bond storage must be fixture-owned. Test erased NVS,
retained valid state, deliberately invalid state, and reset during update. Do
not silently reuse one version's writable flash across unrelated tests. Snapshot
restore across emulator or vendor-library versions is unsupported unless a
versioned state migration is implemented and separately tested; cold-start
compatibility does not imply cross-version restore compatibility.

## Shared radio model requirements

Use QEMU virtual-clock events, bounded TX/RX queues, explicit buffer ownership,
and deterministic timeout/error injection. Do not let host wall-clock blocking
stall the guest's radio engine. Every event trace records a guest timestamp,
controller/interface/peer identity, cause, state transition and frame length.
Packet captures and decoded metadata supplement firmware assertions.

Keep network transport distinct from simulated RF/link conditions. A virtual AP
and virtual BLE peers use seeded channel/RSSI/loss/delay inputs; host NAT/TAP
connectivity has separately declared limits. Tests use local services and fixed
credentials/certificates to avoid Internet variability. Physical host adapters
are an optional integration mode and cannot be the only reproducible CI path.

Functional compatibility, deterministic link behavior, and RF/cycle fidelity
are different acceptance levels. Antenna effects, waveforms, true RF power and
regulatory measurements need a separate instrumented hardware programme;
simulated RSSI/CSI is identified as synthetic. Espressif's physical RF test setup
uses an RF tester and controlled conditions.
[Official S3 RF test procedure](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/development_stage/rf_test_items/wifi_non_signaling_test.html)

## Wi-Fi work items

The complete catalogue includes STA/AP/AP+STA, b/g/n, security, HT20/40,
aggregation/QoS, power save, and vendor features. Each advanced item below
remains visible even when phased after the initial usable path.
[IDF Wi-Fi overview](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/overview.html),
[Wi-Fi driver catalogue](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/index.html)

| ID | Phase / work | Dependencies | Native acceptance and failure cases |
| --- | --- | --- | --- |
| WIFI-01 | Native initialization and lifecycle | Pinned matrix, boot gate | Ordinary `esp_wifi_init/start/stop/deinit`, erased/retained NVS, clock/reset/calibration, task and interrupt setup; capture unknown MMIO/polling and watchdog locations. Repeat 100 start/stop cycles as the initial stress threshold; no hangs, stale IRQs, leaked buffers or API interception. |
| WIFI-02 | MAC/SLC/DMA and IRQ semantics | WIFI-01 | Real descriptor format, RX metadata, chained buffers, length/EOF/ownership, TX completion and interrupt clear. Test delayed RX, starvation/replenishment, malformed descriptors, oversized/truncated frames, reset in flight and bounded queue overflow. |
| WIFI-03 | Scan and configurable virtual APs | WIFI-02 | Active/passive scans, hidden SSIDs, SSID/BSSID/channel filters, multiple APs and scan cancellation return expected records and scan-done state. Missing AP, changing channel, duplicate advertisements and injected RSSI changes are observable. |
| WIFI-04 | Open station authentication and association | WIFI-03 | Ordinary station fixture produces connected/disconnected events with correct BSSID/channel. Reject authentication/association, absent SSID, capacity exhaustion, forced deauthentication and beacon timeout with expected reasons. |
| WIFI-05 | IP traffic and network backend | WIFI-04 | DHCP/static IPv4, ARP, DNS, TCP/UDP local echo and HTTP/TLS; compare payload bytes and event order. Delayed/expired lease, DNS failure, dropped packets, closed sockets and backend disconnect fail predictably. Track IPv6/SLAAC/DHCPv6 and multicast separately according to firmware/backend support. |
| WIFI-06 | WPA2-Personal | WIFI-02, WIFI-05, crypto qualification | EAPOL handshake, RSN negotiation, key installation, protected data and replay counters. Correct credentials connect and exchange traffic; wrong credentials, replay, expired keys and incompatible policy fail. Do not report an open connection as WPA2 success. |
| WIFI-07 | Reconnect, persisted state and recovery | WIFI-05/06 | Missing AP then restored AP, AP reboot, lease loss, repeated disconnect/connect, guest reset and reconnect under traffic. Initial stress gate: 100 reconnect cycles and a one-hour local traffic soak with asserted payload integrity and bounded memory/queues. |
| WIFI-08 | Guest SoftAP and multiple stations | WIFI-02, WIFI-05/06 | Guest beacon/auth/association, station table and per-peer address handling; local peers obtain DHCP leases from guest firmware, exchange data and disconnect independently. Capacity limit, rejected credentials and station timeout work. |
| WIFI-09 | AP+STA coexistence | WIFI-07/08 | Distinct interfaces and shared-channel behavior remain correct during station scan/reconnect/channel change; simultaneous guest AP client and station traffic have no cross-peer corruption. |
| WIFI-10 | WPA3/PMF and other security modes | WIFI-06, crypto | SAE, transition mode, PMF policy, downgrade refusal, handshake retry and wrong credentials. Enterprise EAP with pinned local RADIUS/certificates; older WEP/WPA, WAPI, WPS and DPP each get an applicable-version submatrix rather than one blanket security claim. |
| WIFI-11 | 802.11 MAC feature completeness | WIFI-02/09 | b/g/n protocol selection, HT20/40, QoS queues, AMPDU/AMSDU, fragmentation/reassembly, retransmission and block acknowledgement. Fixture assertions and frame traces verify ordering, malformed-frame handling and unsupported negotiation. |
| WIFI-12 | Power save and RF/clock interactions | WIFI-07/09, clock/power and ADC gates | Modem sleep, DTIM/listen interval, wakeup, pending traffic and reset across clock changes; Wi-Fi/BLE contention and ADC2 arbitration follow the selected IDF/profile. Compare documented behavior with the board; do not infer physical current consumption. |
| WIFI-13 | Promiscuous/raw/vendor IE interfaces | WIFI-02/11 | Sniffer filtering, raw frame TX limits, vendor IE insertion/parsing, channel metadata and malformed frame handling. Explicit capture/replay fixtures verify guest-visible bytes. |
| WIFI-14 | ESP-NOW and mesh | WIFI-02/10/13 | ESP-NOW unicast/broadcast, encrypted peers, peer limits, callback ordering and loss/retry; ESP-WIFI-MESH discovery/join/routing/recovery against multiple virtual nodes. Independent feature status and version compatibility. |
| WIFI-15 | Roaming, location and auxiliary features | WIFI-03/10/11 | WNM/BSS transition, radio resource measurement, fast BSS transition where applicable, FTM/location and antenna configuration APIs. Each optional feature has a capability/unsupported matrix and trace-based fixture. |
| WIFI-16 | CSI and RF fidelity extension | WIFI-11/12 | CSI configuration and record formatting can use documented synthetic channels. A separate hardware/instrument gate is required for quantitative RF/channel fidelity; no real RF accuracy claim from fabricated data. |

The event/failure oracle follows the applicable IDF version's station and
security documentation. Assert API result, event reason, descriptor/IRQ state,
and payload integrity together.
[Station scenarios](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/station-scenarios.html),
[Security and roaming](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/security-and-roaming.html),
[Power save](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)

## Bluetooth LE work items

The first decision is **native controller hardware compatibility**, not which
host-side GATT library to use. Native controller init and the separate LE host
must both execute before a peer scenario can qualify ordinary firmware.
[Controller API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/controller_vhci.html)

| ID | Phase / work | Dependencies | Native acceptance and failure cases |
| --- | --- | --- | --- |
| BLE-01 | Native controller boundary and init | Matrix, boot/clock/IRQ gates | Normal controller init/enable/disable/deinit and NimBLE/Bluedroid host init without synthetic MMIO or ELF interception. Trace actual vendor MMIO, timers, IRQs and initialization blockers; record differences by controller-library hash. A bypass-only result stays labeled adapted. |
| BLE-02 | HCI correctness and transport accounting | BLE-01 | Truthful supported commands/features/states and event masks, command credits, reset/unknown-command errors, packet validation, ACL queue bounds and completion counts. Host backpressure and malformed/truncated HCI packets do not corrupt state. |
| BLE-03 | Advertising and scanning | BLE-02 | Legacy advertising, scan response, directed/nonconnectable modes, active/passive scan, duplicate filtering, whitelist/filter accept list, public/random addresses, cancellation and interval/window behavior. Independent peer sees exact payload/address and guest sees exact reports. |
| BLE-04 | Central/peripheral connections | BLE-03 | Guest initiates and accepts connections, validates parameters, reports role/address/status, handles cancel, connection failure, supervision timeout, update and disconnect. State transitions are scheduled, not unconditional immediate success. |
| BLE-05 | L2CAP and ACL data plane | BLE-02/04 | ACL fragmentation/reassembly, credits, ATT fixed channel, signaling and connection parameter procedures; bounded partial packets, MTU limits, malformed lengths and peer disconnect during transfer. |
| BLE-06 | GATT client and server | BLE-05 | Service/characteristic/descriptor discovery, read/write, write without response, MTU exchange, long/prepared writes, CCCD, notification/indication/confirmation, attributes with 16/128-bit UUIDs and correct ATT errors. Both guest client and guest server compared with an independent peer. |
| BLE-07 | Pairing, encryption and bonding | BLE-05/06, crypto and persistent state | Applicable legacy pairing and LE Secure Connections, Just Works/passkey/numeric comparison/OOB modes, encrypted/authenticated attribute permissions, key distribution and bond persistence. Wrong passkey, declined pairing, mismatched IO/security policy and replay fail; no global encrypted-peer shortcut. |
| BLE-08 | Multiple peers and role concurrency | BLE-04/07 | Independent handle, address, MTU, CCCD, key and credit state; concurrent links and scan/advertise roles according to declared controller limits. Capacity errors and one peer's disconnect do not affect another. |
| BLE-09 | Privacy and address resolution | BLE-03/07/08 | Resolving lists, identity versus resolvable private addresses, rotation, filter interaction, reconnect to bonded peer and unknown address/key failures. |
| BLE-10 | LE PHY, data length and extended advertising | BLE-04/08 | Applicable data-length extension, LE 2M/Coded PHY negotiation, extended/periodic advertising and scan/sync reports with fragment/status handling. Per-version/chip capability matrix determines supported variants; unsupported requests return documented errors. |
| BLE-11 | Power, clocks and Wi-Fi coexistence | BLE-08/10, WIFI-12 | Modem sleep/wakeup, connection timers, Wi-Fi/BLE simultaneous throughput and loss, repeated enable/disable/reset and resource release; assert functional scheduling/state rather than physical RF power or current. |
| BLE-12 | Host profiles and provisioning | BLE-06/07/08 | BLE Mesh provisioning/node roles, BluFi secure Wi-Fi provisioning, HID and other declared LE profiles. These are separate host-level fixtures and must not be conflated with basic controller support. |
| BLE-13 | Stress, failure injection and restore | BLE-08/11 | Initial 100 connect/disconnect cycles and one-hour notification traffic; peer loss, missing acknowledgements, queue overflow, bond corruption and guest reset. Snapshot restore only after radio/peer/timer state has a defined replay/reconciliation policy. |
| BLE-14 | Optional physical adapter integration | BLE-02/07, host backend | External Linux/WSL-compatible adapter transport and phone interoperability, with explicit controller ownership and timestamp limitations. No physical adapter needed for the deterministic virtual-peer CI suite. |

Evaluate a pinned Google Bumble release for independent virtual peers, GATT and
security scenarios. Its provided controller/host/link/security components and
TCP transport can reduce test-harness work; its own advertised limitations must
be recorded, and it cannot solve the S3 vendor controller MMIO boundary.
[Bumble documentation](https://google.github.io/bumble/)

## SIMD and PIE work items

The S3 PIE includes eight 128-bit Q registers, a 320-bit QACC interpreted as
sixteen 20-bit or eight 40-bit accumulators, a 40-bit ACCX, and auxiliary
shift/unaligned state. Alignment, post-update addressing and combined memory
operations require instruction-specific semantics.
[TRM v1.8, chapter 1](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)

| ID | Phase / work | Dependencies | Acceptance and failure cases |
| --- | --- | --- | --- |
| SIMD-01 | Complete instruction/state catalogue | Pinned official/recovered decoder and TRM | Manifest reconciles assembler mnemonics, decoded encodings, translator names, operand widths, implicit state, side effects, memory accesses and documented exceptions. Missing/aliased names and unsupported encodings are explicit; 246 source entries are not a completion metric. |
| SIMD-02 | Reference harness and state capture | SIMD-01, HW-01..04 | Same deterministic firmware executes on QEMU and hardware; capture full Q0..Q7, QACC, ACCX, auxiliary state, AR updates, touched memory and exception state as bytes/integers. Golden record provenance and hashes retained. |
| SIMD-03 | Movement, lanes and alignment | SIMD-02 | Zero/move, RUR/WUR, lane extraction/insertion, broadcast, QACC/ACCX loads/stores, aligned/unaligned operations, zip/unzip and bit reversal. Test every lane, legal address offset, post-increment variant, source/destination alias and page boundary. |
| SIMD-04 | Integer ALU and shifts | SIMD-03 | Signed/unsigned 8/16/32-bit arithmetic, min/max/compare, bitwise logic and every shift form; min/max, all-ones/zero, alternating signs, shift boundaries and saturation. Compare complete state and memory, not checksum alone. |
| SIMD-05 | Multiplication and accumulation | SIMD-03/04 | All widening, signed/mixed/unsigned, vector/scalar MAC variants, QACC/ACCX widths, saturation, extraction and rounding. Prioritize unsigned 20-bit overflow and negative signed limits; derive intended results from TRM plus board vectors. |
| SIMD-06 | Combined and DSP instructions | SIMD-05 | Arithmetic plus simultaneous load/store, complex multiplication, FFT butterflies, ReLU/PReLU and unaligned pipeline state. Verify input snapshot versus destination alias ordering, memory faults and address updates. |
| SIMD-07 | Machine semantics and context | SIMD-01..06, core/reset/IRQ gates | Per-core independence, reset state where documented, task/interrupt context preservation, access permissions, illegal encodings, fault PC/cause and partial side effects. Determine relevant coprocessor/access controls rather than assuming scalar FP CPENABLE applies to every PIE operation. |
| SIMD-08 | Implementation portability | SIMD-03..07 | Replace undefined signed overflow/shifts, host-endian aliasing and out-of-range lane access where found. Run meaningful sanitizers/helper checks alongside guest differential fixtures; mathematical reference code must be independent of emulator helpers. |
| SIMD-09 | ESP-DSP/ESP-DL workload qualification | SIMD-06/07 | Pinned optimized FFT, FIR/IIR, dot product, matrix and neural kernels compare against scalar reference and hardware outputs. Retain ordinary optimized firmware build paths; tolerance applies only to documented numerical behavior. |
| SIMD-10 | Debugger and fidelity reporting | SIMD-02/07 | True Q/QACC/ACCX and auxiliary state displayed with widths, lane interpretation and per-core identity. Step/reset/read validation; F-registers remain scalar floating point. Functional instruction accuracy is separate from cycle/performance accuracy. |

For every applicable instruction, test a fixed edge vector set, all legal
immediate/selection classes, relevant register aliases and seeded random input.
The release report lists exact tested dimensions and unverified combinations;
it does not label the whole ISA verified from a DSP example alone.
ESP-DSP supplies relevant optimized kernels, not an independent oracle for
every instruction. [Official ESP-DSP](https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/index.html)

## Hardware reference work items and COM5

The user has an ESP32-S3 board and identified COM5 as a possible reference
transport. Port presence is the only confirmed acquisition fact. Read-only
inventory can proceed; flashing, changing eFuses, and destructive persistence
tests require explicit user authorization. Never burn eFuses for this suite.

| ID | Work | Acceptance / artifact |
| --- | --- | --- |
| HW-01 | Board/transport discovery | Inventory COM5 USB identity and bridge/native USB type, existing serial data if available, board/module part number and reset wiring. Controlled DTR/RTS settings; no implicit reset/flash during discovery. Obtain chip/ROM revision and flash/PSRAM profile through existing firmware or separately authorized ROM query. |
| HW-02 | Fixture and recovery protocol | Prepare source, sdkconfig, flash map, merged image, ELF, vector manifest and flashing plan. Explain retained user firmware/flash backup options and recovery. Wait for explicit flashing authorization before deployment; eFuse mutations excluded. |
| HW-03 | Deterministic producer | Versioned record format; fixed PRNG algorithm/seed; explicit input register initialization; sequential test IDs; length/CRC framing; reset/fault recovery and resumable execution. No dependency on accidental register reset values. |
| HW-04 | Acquisition and immutable references | Serial consumer retains raw log and parsed results with firmware/ELF/IDF/toolchain/blob hashes, chip/ROM revision, eFuse calibration metadata, clocks, flash/PSRAM, test seed and source revision. Store full values as well as summary hashes. |
| HW-05 | Differential replay | Same identified merged image on hardware and QEMU; compare Q/accumulator/auxiliary/AR/memory/exception state. Output first mismatch and neighboring records, reproducible minimal failing seed and coverage matrix. A missing record is a failure, not a skipped success. |
| HW-06 | Radio and ADC reference scenarios | Known local AP/peer/service setup, identifiable security/channel/address configuration, firmware events and captured packets. ADC input voltages and measurement method recorded; test ADC1/ADC2 with radio off/on and driver-specific arbitration/calibration behavior. |
| HW-07 | Revision expansion and uncertainty | Keep board-specific references; compare additional ROM/chip/module profiles when acquired. Record physical setup and repeatability limits; one board does not establish all silicon or RF/calibration fidelity. |

A SIMD record contains `schema_version`, `test_id`, `seed`, complete input state,
complete output state, address updates, touched memory, exception PC/cause and
sequence integrity. Its run header contains all binary/source/profile hashes.
Checksums accelerate comparison but do not replace raw values needed to debug
mismatches. Fault cases run with a watchdog/reboot and acquisition recovery
strategy; absence of a terminating record cannot pass.

ADC2 and radio behavior must follow each S3 driver/revision combination. Older
ADC APIs and newer oneshot APIs describe different arbitration outcomes; do not
copy the original ESP32 blanket restriction into the S3 model. ADC voltage,
attenuation, bit width, calibration and radio occupancy fixtures need a shared
time/profile contract with the electrical simulation workstream.
[S3 ADC oneshot driver](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/adc_oneshot.html),
[S3 RF calibration](https://docs.espressif.com/projects/esp-idf/en/v5.1.4/esp32s3/api-guides/RF_calibration.html)

## Milestone dependency and uncertainty review

1. Freeze manifests and run WIFI-01/BLE-01/SIMD-01 before extending success
   state machines. Their results decide the actual native compatibility gap.
2. WIFI-02..07, BLE-02..07 and SIMD-02..07 establish the first useful functional
   paths. UX shows experimental/unavailable features until their native gates
   pass; development adapters have separate labels.
3. Expand the release IDF/Arduino/profile matrix, then peer/interface concurrency,
   advanced features, context/security/persistence and sustained workloads.
4. Hardware vector acquisition proceeds after board and flashing authorization
   are concrete. Firmware/reference preparation can proceed while hardware is
   unavailable; hardware-comparison gates remain unpassed.
5. Advanced backlog items are required to converge toward broad hardware
   fidelity. Defer them visibly, with dependencies and evidence states, rather
   than silently shrinking "full support" to an initial connection demo.

Major uncertainty is the undocumented vendor radio hardware interface and its
revision dependence. Public TRM/API coverage does not supply the complete PHY,
baseband/controller contract. Native initialization traces, inspected linked
libraries, and board comparisons are prerequisites for a defensible effort
estimate. Current source presence cannot close that uncertainty.

Reset, timers, interrupts, RNG/crypto, NVS/eFuse state and electrical/ADC behavior
are cross-workstream dependencies. Assign shared interface ownership in the
master plan and retain regression fixtures whenever those foundations change.
Snapshot migration and external radio peers additionally require explicit
reconciliation rules; restoring the CPU alone does not restore a radio session.

Espressif now publishes an [esp-emulator beta](https://github.com/espressif/esp-emulator/blob/main/README.md)
with documented S3 Wi-Fi/BLE workflows. It is a different emulator and documents
BLE ELF-symbol interception. It can be evaluated as a pinned secondary
behavioral comparison after checking its license and reproducibility, but does
not change the chosen QEMU base or qualify native QEMU radio hardware fidelity.

This is a research/planning document. No new radio, SIMD or physical-board test
has been run as part of writing it; its acceptance criteria are proposed gates.
