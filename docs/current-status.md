# Current implementation evidence — 2026-10-07

The takeover has delivered additional verified slices; the **full simulator remains
unfinished**. Live ownership and evidence are recorded in
[the continuation log](handoff/09-takeover-2026-10-07.md).

* Earlier required GUI/runtime regression: **9/9 CTest, zero skips, 47.49 seconds**.
  The newer typed analog/net editor was independently built and visually inspected:
  circuit/theme/project targets pass **3/3**, with 9/6/18 Qt results and no skips.
  Live electrical QMP integration remains a separate, pending verification gate.
* ESP-IDF 5.5.5 and Arduino 3.3.12 each passed the real official-QEMU boot/control
  fixture. These are specific firmware passes, not blanket compatibility.
* The hostbus dependency probe passes its enabled peer, fault, restore-denial and
  dual-core scenarios. Review correction increased raw fault coverage to **31
  checks**; lifecycle scenarios pass **7/7**. Combined-runtime verification observes
  exact armed/stopped deadlines and modeled completion. This remains probe-only
  evidence, not production native bus qualification.
* The authorized physical run captured 84 PIE vectors and ordinary Wi-Fi/BLE
  lifecycle stages. The user approved re-baselining and waived restoration;
  both complete local flash images remain preserved.
* Hardware comparison found 38 PIE vector discrepancies. Three tracked translator
  patches now match **84/84 vectors byte-exactly**, including the previously failing
  saturation cases. This is a partial instruction catalogue.
* DC projection ABI 2 handles unfinished wiring as physical state; its review
  corrections pass 14 scenarios/2780 checks. RC review exposed floating-current,
  charge-remap and crossing defects; corrected kernel plus probe API pass **637 checks** and
  the six normal/sanitized electrical targets. Native pad/ADC integration is pending.
* Current timer-integrated runtime, fingerprint `dbe2a8a5ad4e07a6`, binary SHA256
  `72ea3caf8f01c4cf5e27db8c2cb4faca0c7153ba7e3b48287b3a4ae3359473a8`,
  was independently verified: three boot/control profiles, same-image SIMD
  84/84, barriers/GDB, clock 10/10, matrix 9/9, GDMA 16/16 and the native
  dual-core shared-IRQ two-boot fixture. All seven original corpus inputs stayed
  unchanged; writable disks were evidence-local copies.
  Evidence: `build-runtime-state/combined-runtime-timer-integrated-verification/20261007T060433Z/`.
* Profile selection now accepts `prepare-qemu-runtime.py --profile <json>` and
  `ESP32S3_RUNTIME_PROFILE=<json>` in the build/validation scripts. Actual
  default/explicit preparation resolves the same fingerprint; the selected
  build preserves the executable hash above. The newer selected-profile run
  passes three boot/control profiles and SIMD 84/84, but **fails hostbus
  delay-fast's normal-boot preflight**: no firmware marker after 90 seconds.
  The barrier was never armed; slow-delay/GDB assertions were not exercised.
  Subsequent diagnostics show continuous guest progress, not a blocked lease.
  Evidence: `build-runtime-state/runtime-profile-cli-verification/20261007T073543Z/`.
* Read-only profiling isolates the bootstrap cost: CPU1 performs 96.66 million
  reads of `SYSTEM_CORE_1_CONTROL_1` before CPU0's first control write.
  `CORE_1_CONTROL_0` reset/gate state is unmodeled in the frozen runtime;
  primary S3 defaults hold CPU1 in reset with its clock disabled. The hardware
  lifecycle correction is in progress and has not passed native boot yet.
  Native-UART staging removes measured DrvFS blocking and durably archives logs
  after exit; fast barrier/GDB guards pass, but slow and dedicated-GDB boot
  preflights still exceed the unchanged 90-second window. Whole-lane failure
  remains recorded; neither a timeout increase nor a ROM-PC shortcut is used.
* **Full native graph BINARY_READY.** Prepared source fingerprint
  `0e2ab32b9d57dee3d0454e063153f820877477e692a145a380640cd3a3193ccd`, binary
  SHA256 `d82ae837c3f909be753f921b0e7de86bb6f48dda62df7514c282e469cc27ac20`.
  Same-image results: electrical 8/8 (incl. native-adc 3079, rc-firmware-step,
  unknown-consumer pause, sleep/awake strict), GPIO 19/19, ADC 14/14, coreclk
  14/14, GDMA 16/16, intmatrix 9/9, numerical kernels 6/6, ordinary GPIO fixture
  PASS, ordinary ADC fixture PASS under a running clock (three rounds of
  3079/3350/3350 then ADC_FX done), three boot/control profiles PASS. The
  ordinary-ADC hang root cause (provider live-clock timestamp equality) is fixed
  across all three affected getters by the aperture echo. Receipt:
  `build-runtime-state/electrical-runtime-2026-10-07/final-binary-ready.json`.
* The locked IDF 6.1 SDK and full submodule tree now build on native Linux
  storage. Independent preparation, ordinary SPI-fixture compile and 4 MiB
  merge succeed; the actual QEMU boot observes the expected application
  markers. Compile took 142 seconds and native-UART boot 6.23 seconds in this
  run. This qualifies the SDK/build path, **not SPI transfers**.
  Evidence: `build-runtime-state/native-sdk-2026-10-07/firmware-verification.json`.
* Native GPIO now observes ordinary loopback and `GPIO_NET_ISR_SEEN` counts
  1 and 2 without an interrupt storm; the low-level fixture needed software
  W1TC acknowledgement. No automatic model acknowledgement or INTC workaround
  was added. The foundation GPIO suite passes 12/12.
* ADC-01's final binary `11967a87…` passes 14 QTests and ordinary IDF divider,
  source-step and floating-input scenarios. A real timerlist/QMP deadlock was
  fixed with an epoch/source-aware main-loop pause BH: invalid sampling produces
  no DONE/data; QMP remains responsive; physical repair remains paused until
  explicit reset/continue. All three boot/control profiles pass 3/3 on that
  same final binary. This is the declared ideal ADC profile, not silicon
  calibration, continuous ADC, hardware comparison or full v3 graph proof.
  Receipt: `build-runtime-state/adc-independent-final-20261007/ADC-01-final-foundation-receipt.json`.

Earlier binary results below remain historical evidence, not current source
qualification. The superseded constant-completion bridge is absent from v3.
Native GPIO/ADC leaf proof does not qualify the full v3 electrical graph or
future additions; each combined integration still requires same-binary proof.

This file supersedes completion claims in the older GPT-generated roadmap,
backlog, comparison and audit reports. Those remain useful as investigation leads.
Presence of a register model, a passing bridge parser test, or an immediate DONE
interrupt is not proof that normal firmware can use the peripheral correctly.

## Recorded baseline

| Item | Revision / environment |
| --- | --- |
| Simulator checkout | `8cb306f` plus current working changes |
| Official runtime | Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)` |
| Recovered extension source | `BaiTian6641/qemu` at `33c2bdd17104b3baeea3b788bafd27b2f921c13f` |
| Firmware toolchain | Installed ESP-IDF `6.2-dev`, commit `25fe69f946311abdaf9ad56591f25fedbc20ac98` |
| Host | Ubuntu 24.04, WSL2, x86-64 |
| Studio | Qt 6.4.2, GCC 13, CMake/Ninja |

## Acceptance evidence

Final WSL2 CTest run: **5/5 targets passed, 0 skipped** with both firmware images
enabled. Targets are bridge contract, launch options, real boot/control, circuit
workspace, and native I2C sensor. The Python device smoke suite also passed.

The actual `QemuController` booted a newly compiled firmware image through the
official ESP32-S3 ROM and second-stage bootloader. The firmware initializes NVS,
queries chip core count and flash size, configures GPIO2, and emits periodic UART
records based on `esp_timer`. The test checks `cores=2`, `flash=4194304`, multiple
ticks, QMP readiness, an actual scalar register/PC snapshot, pause/resume, and a
second successful boot after reset. This proves this fixture and toolchain combination,
not universal compatibility with arbitrary Arduino or ESP-IDF binaries.

The recovered custom QEMU revision was built from source in WSL2 and passed the
same fixture/control test. A one-second delayed launch adapter around the official
binary also passed, exercising QMP connection retries instead of assuming that
the server is ready after a fixed 250 ms.

The circuit test edits SDA on one component and verifies the change propagates
to the other I2C component and bus registry. It saves to another directory,
starts the relocated device process, adds a component with a non-conflicting
address, rejects nonexistent GPIO22, and verifies invalid JSON leaves the current
runtime intact. An offscreen render of the real application was visually inspected,
including startup with firmware and orderly process shutdown.

The existing Python device smoke suite verifies modeled I2C, SPI and UART requests.
It exercises host-side device models; it does not execute ESP-IDF peripheral drivers.

Separately, the **native I2C firmware fixture** uses the ordinary ESP-IDF master
driver (`i2c_master_transmit_receive`) on GPIO8/9. Against the recovered QEMU
extension it read the host SHT21 model at 25°C, verified CRC8, received NACK for
an absent address, and then read 30°C after changing the host model parameter.
No serial JSON protocol or special firmware bus hooks are used. This verifies
one repeated-start sensor path through the existing cached response map; it does
not qualify generic asynchronous I2C devices, timing or clock stretching.

## Capability assessment

| Goal | Evidence now | Remaining acceptance work |
| --- | --- | --- |
| Normal firmware boot | Development IDF, stable IDF 6.1/5.5.5 and Arduino 3.3.12 boot/control fixtures pass on pinned official QEMU | Remaining versions, PSRAM variants, OTA, encrypted flash, configuration matrix and soak |
| UART | ROM/app UART0 works through the launcher and PTY; host loopback model passes | UART1/2 external devices, flow control, framing/IRQ/FIFO stress |
| GPIO | Fixture configures/toggles GPIO2; project wiring edits persist | Observe actual pin transitions; external input injection; GPIO matrix and interrupt semantics; no electrical routing yet |
| I2C | Real IDF master driver reads SHT21 with repeated-start, CRC, NACK, and live host temperature changes through the custom bridge | Other devices/controllers, generic responses, stretching/timeouts and controller timing |
| SPI | Python flash/display models pass; custom source emits TX bridge events | Full-duplex/MISO delivery, CS selection, DMA chains, modes and timing against normal drivers |
| I2S | Custom source consumes/fills at most a 256-byte DMA kick and asserts immediate completion | Clocked streaming audio, continuous DMA, RX sources, sample correctness, descriptor/IRQ behavior |
| RMT | Custom register model exists | Timestamped TX/RX waveform behavior, DMA and a logic-analyzer fixture |
| LCD | Custom LCD_CAM source drains TX DMA and discards pixel bytes | Real LCD stream/framebuffer delivery, I80/RGB timings, buffer ownership |
| Camera | Custom CAM registers are stored; no sensor frame injection verified | Camera model, sync/pixel source, complete RX DMA frames and IRQs |
| Wi-Fi | Unmodified native discovery fixture initializes driver/mode; a dedicated official-base extension models the REGI2C analog-master transaction SEQUENCING (timed, TRM/ROM-derived) with measurement surfaces fail-closed behind a provider interface: firmware reaches the SAR2/PWDT measurement dependency, which pauses the VM with one precise diagnostic instead of fabricating data. Next evidenced blocker (unqualified trace): an IQ-calibration poll in the undocumented modem window 0x60006000-0x60007FFF | Measured SAR2/PWDT/TSENS data semantics (needs an authorized hardware MMIO-read trace), modem-window contract, controller start, scan/auth/connect/DHCP/socket/security qualification |
| Bluetooth | Same extension: native controller init fully returns OK; enable enters PHY calibration and reaches the same fail-closed SAR2 measurement dependency | Same measurement prerequisites, then controller enable completion, BLE host/GATT/security/peer qualification |
| SIMD | Identical 84-vector firmware ran on hardware and QEMU; three translator patches correct all 38 observed discrepancies, and the fixed binary matches 84/84 | Remaining 258-entry inventory, uncovered opcode/state/fault/context cases, per-core/reset behavior and Q-register debugger support |
| Analog/electrical | Typed graph; DC adapter ABI2 (14 scenarios/2780 checks); corrected RC kernel and state-copy planner API (637 checks, six normal/sanitized targets), with conserved floating-island current, element-index charge preservation and committed-state crossings | Native solver-owned pad/IRQ/ADC integration; selected impedance/threshold profiles and supported topology ranges; no measured silicon accuracy claim |
| Host dependency scheduling | Bounded codec and QOM dependency probe; 31 raw-fault checks, seven lifecycle cases, combined-v3 exact-time/GDB smoke | Production native bus dependencies, clock/routing integration, multi-service horizons and whole-machine replay |
| Internal chip state | Timer-integrated clock 10/10, matrix 9/9 and GDMA 16/16; exact encoded divider boundaries/rational tick carry; native shared IRQ fixture passes on both cores over two boots | Complete gate/reset/retention and memory-path matrix; full debugger operation coverage |
| External PSRAM / memory | CORE-04 binary (`6dfa7350`): ordinary IDF 6.1 and 5.5.5 quad 8MiB firmware boots, passes its own PSRAM startup memory test and completes every CPU cache/GDMA consumer assertion with three boots (initial, QMP reset, same-disk relaunch); 12/12 native memory qtests and 16/16 frozen GDMA cases pass on the same executable; the SPI1 transport control shows fail-before/pass-after only for the duplex/asymmetric cases | Octal/DDR/DQS and other densities, CPU access to a proved-unmapped alias (no safe exception profile), independent PSRAM instruction-cache/XIP visibility, preload/autoload/lock/tag assists, cache-error IRQ and silicon timing, camera/RGB consumers on the camera-capable tree |
| UI | Carbon studio with typed analog catalogue/parameters, arbitrary nets/branches, undo/save and explicit native-versus-legacy Apply; real Light/Dark surfaces inspected | Real electrical QMP ACK/snapshot and firmware-driven circuit workflows; traces, accessibility/platform and packaging qualification |

## Concrete issues found in the recovered code

* Launcher used custom machine properties against any discovered QEMU binary;
  official QEMU rejected them. Fixed with machine-help probing and official global options.
* The GUI attempted bridge QOM properties even on official QEMU. Fixed with a
  native bridge probe and a visible capability state.
* Host peripheral responses were written into firmware UART. This is now an
  explicit legacy-test compatibility option; ordinary firmware UART stays clean.
* Initial sensor maps/DC configuration could be dropped before QMP was ready.
  Settings are cached and replayed before normal boot is released.
* QMP was attempted only once, 250 ms after process startup. Normal boot now waits
  for bridge initialization and retries QMP connections within a bounded deadline.
* Invalid peripheral configs discarded the active circuit. Validation now occurs
  before replacing the runtime.
* Qt shutdown invoked MainWindow after its derived destructor and destroyed
  running peripheral QProcesses. Lifecycle teardown now disconnects and reaps them.
* Floating-point F registers were labeled vector registers and absent data
  appeared as zero. The label is corrected; unavailable data is shown explicitly.
* Missing submodule metadata and CRLF shell scripts prevented reproducible builds.
  Metadata is restored; root shell files use LF and QEMU builds from a separate Linux checkout.
* Cache MMU translations for PSRAM targeted an address space rooted in an
  uninitialised region, so ordinary firmware faulted on its first PSRAM access
  while the same-driver qtests kept passing. Translations now resolve inside
  system memory through a private alias, whose base must stay inside the 32-bit
  CPU physical space; the qtest address register was also written with a stale
  shift that only PIO-versus-PIO comparisons could not detect.

## Next delivery gates

1. The recovered extension builds and boots the fixture. Qualify its individual
   peripheral data paths independently of the official baseline.
2. Replace stderr/serial bridge messages with a dedicated, versioned transport.
   A normal IDF I2C sensor read must return the current host model value; SPI must
   return actual MISO bytes and respect controller/CS. No firmware log hooks.
3. Make circuit nets drive real GPIO matrix routing and input/output changes.
   Add a button/LED fixture, edge timestamps, and IRQ assertions.
4. Implement peripheral data paths in order: SPI+DMA, RMT, I2S, LCD, then camera.
   Each requires real-driver fixtures and bounded timing behavior before promotion.
5. Qualify SIMD and chip internals with hardware reference vectors and reset/IRQ tests.
6. Treat Wi-Fi and BLE as separate major workstreams; validate unmodified vendor blobs
   and network/peer state machines before claiming support.

Old "complete" labels must not be carried forward without these observable gates.
