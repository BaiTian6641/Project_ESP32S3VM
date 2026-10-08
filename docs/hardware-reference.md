# Native firmware reference acquisition

This tooling builds ordinary ESP-IDF firmware, runs it unchanged in QEMU, and
prepares explicit physical-board capture and byte comparison. **It never flashes
hardware, changes eFuses, or opens a port automatically.** Physical comparison is
pending an authorized fixture deployment and recorded board identity.

Every reference application waits **15 seconds after entering `app_main`** before
its one-shot begin/vector-or-event/end batch. This is an ordinary `vTaskDelay` in the same
binary for QEMU and hardware; there is no emulator-specific API or calibration
bypass. The manifest and begin record report `capture_grace_ms = 15000`. Start
capture promptly after the approved deployment/reset, allowing USB enumeration
to settle inside this grace period. Use a capture deadline of at least 60 seconds.

The initial SIMD catalogue contains 84 vectors across seven instruction/alias
forms: unsigned/signed 8-bit QACC MAC, signed 8-bit add/subtract, Q-register move,
add aliasing its first operand, and signed 16-bit add aliasing its second operand.
It is an initial reference slice, not complete PIE qualification. Edge patterns
cover zero, unsigned 20-bit overflow, signed accumulator limits and lane identity;
seeded vectors use a fixed xorshift32 algorithm. The overflow case specifically
investigates the recovered/official negative unsigned saturation expression.

`pie_reference.S` initializes Q0-Q7 and QACC from supplied bytes, initializes
auxiliary state, and reads the actual state before and after a real assembled
instruction. Captures include all Q registers, both packed QACC halves, SAR,
SAR_BYTE, FFT_BIT_WIDTH, ACCX, UA_STATE and observed load/store pointer increments.
No scalar implementation fabricates the returned Q/accumulator values. Interrupts
are excluded during the small assembly sequence; task/interrupt preservation and
exception tests remain later gates.

## Build without touching the board

From the repository root in WSL2:

```bash
bash tools/build-reference-fixtures.sh simd_reference
bash tools/build-reference-fixtures.sh radio_init wifi
bash tools/build-reference-fixtures.sh radio_init ble
```

Use `all` to build SIMD and the selected radio mode; `radio_init both` intentionally
runs both paths sequentially. Separate modes prevent a blocking Wi-Fi initializer
from concealing the BLE initialization result. Current BLE discovery exercises
the normal controller and Bluedroid lifecycle; NimBLE discovery is not implemented
in this first fixture.

The script accepts an existing `IDF_PATH` or `ESP32S3_IDF_ENV`. It also understands
the installed EIM activation script's `-e` output without `eval`. It writes
sdkconfig and build products below each fixture's ignored `build` directory.
Radio profiles use `build/wifi`, `build/ble` and `build/both`. It produces:

* A 4 MiB merged ROM-bootable flash image, bootloader, partition table and ELF.
* `reference-manifest.json`: source/configuration, IDF commit, compiler, vendor
  library hashes, ELF/flash hashes, expected case/stage catalogue and actual console
  configuration. Freeze this manifest with each acquisition.

The tested development environment is IDF `6.2-dev` at
`25fe69f946311abdaf9ad56591f25fedbc20ac98`, Xtensa GCC 16.1.0. Stable IDF and
other chip/toolchain profiles must be built and qualified separately. Espressif's
official QEMU release has annotated tag object `6cdce334bfa5541a5e46c860a9b40a4fe5a13aae`,
which peels to source commit `40edccac415693c5130f91c01d84176ae6008566`.

## Run the same image in QEMU

Example using the installed official binary:

```bash
python3 tools/hardware-reference.py --qemu \
  --qemu-bin /home/polar/.espressif/tools/qemu-xtensa/esp_develop_9.2.2_20260417/qemu/bin/qemu-system-xtensa \
  --firmware tests/firmware/simd_reference/build/simd_reference.merged.bin \
  --manifest tests/firmware/simd_reference/build/reference-manifest.json \
  --output tests/firmware/simd_reference/build/runs/official-01 --seconds 60
```

Replace paths with the radio mode's artifacts for initialization discovery.
Output must be a new directory. The runner copies the pristine flash into that
directory, so writable NVS/calibration never leaks into another test. An optional
`--efuse` image is also copied before use. Default eFuses are the selected QEMU
runtime's defaults and are recorded as such; they are not a characterized board
profile. The runner stops at a complete batch, cancellation, byte limit or deadline.
Failed/incomplete results retain logs and metadata and return a nonzero status.

Add `--qemu-diagnostics --byte-limit 524288` for a bounded `unimp,guest_errors`
capture and QMP register snapshot. Add `--qemu-mmio-writes` to enable real write
tracing through QMP after the first fixture record, excluding irrelevant ROM-boot
traffic. Guest stdout is stored in `raw.log`; diagnostics are stored separately in
`qemu.log` so trace text cannot corrupt the framed records. Their combined stored
bytes obey the declared cap. The runner pauses the guest after the run's
deadline before reading registers; this is diagnostic execution, not a firmware
bypass or a successful initialization result. QMP replies/errors are retained in
capture metadata, and the endpoint binds only to host loopback.

Wi-Fi discovery uses ordinary NVS/netif/event-loop initialization, Wi-Fi init,
station mode, start/stop and deinit. BLE discovery uses normal controller and
Bluedroid init/enable/disable/deinit. There are no replacement IDF components,
synthetic QEMU HCI registers, firmware bus hooks or symbol interception. NVS errors
are reported without an automatic erase. A hang/panic is recorded at the last
started stage, not reported as support. Completing init is not scan/security/GATT
qualification.

## COM5 and console routing

Read-only Windows inventory:

```powershell
python tools/hardware-reference.py --inventory
```

Inventory enumerates ports and USB metadata without opening them. The observed
COM5 reports VID:PID `303A:1001`, consistent with Espressif native USB Serial/JTAG;
this is not proof of board/module identity, nor of a UART0 bridge.

The parent subsequently performed ROM identification/security/flash reads on COM5
with reset but **no flash/eFuse writes**, identifying ESP32-S3 revision 0.2,
N16R8 profile, 40 MHz XTAL and native USB Serial/JTAG. Secure boot and flash
encryption were reported disabled. The parent completed a full 16 MiB flash
backup and owns its preservation/hash record. This updates the earlier inventory
only evidence; the reference tool/subagent did not open the port. Fixture flashing
still needs explicit approval. The fixture uses a 4 MiB flash layout and internal
SRAM; it does not qualify the board's 8 MiB PSRAM.

The tested IDF6.2-dev configuration selects UART0 primary **and USB Serial/JTAG
secondary** by default. The manifest records the actual SDK flags rather than
assuming that every IDF build exposes both. QEMU output validates UART0; physical
COM5 delivery through the secondary USB console remains untested. If a selected
build is UART0-only, COM5 cannot be assumed to carry its output. Identify an actual
USB-UART bridge and board TX/RX/GND terminals, or build and verify a secondary-USB
profile. Do not hardcode a fictional COM number or Linux `/dev/ttyS5` mapping.

Before deployment/capture, review the exact manifest, merged image, flash offsets,
existing board firmware preservation/recovery, board/chip/ROM revision, flash/PSRAM
and physical console path. Opening a serial device may toggle DTR/RTS or cause USB
driver pulses even when capture makes no explicit reset request. The capture code
requests its DTR/RTS levels before opening and records them. This is controlled
acquisition, not a guarantee that all serial drivers preserve execution state.

After separately authorized deployment of the identified fixture, acquisition is
an explicit action:

```powershell
python tools/hardware-reference.py --capture --port COM5 --baud 115200 `
  --manifest tests/firmware/simd_reference/build/reference-manifest.json `
  --output tests/firmware/simd_reference/build/captures/board-01 --seconds 60 `
  --board-context '{"board":"confirmed module/board", "chip_revision":"observed", "instruments":[]}'
```

This example is not permission to flash or a claim that current COM5 firmware is
the reference fixture. `--port` is mandatory; Ctrl+C cancels while preserving
partial bytes. No serial capture command contains a flash, erase, download or
eFuse instruction. Hardware instrumentation and safe voltage/wiring profiles are
separate requirements for analog and timing comparisons.

The one-shot vector batch must start **after** acquisition is open. The tool prints
when the port has opened; arrange an approved fixture restart/manual reset then,
or an equivalent reviewed deployment/capture sequence. It does not reset the board
to recover a missed begin record. A missing begin/USB re-enumeration is an
acquisition failure and is not automatically an instruction mismatch.

Current reviewable SIMD artifact after the capture-grace rebuild:

| Item | Frozen identity |
| --- | --- |
| Merged image | `tests/firmware/simd_reference/build/simd_reference.merged.bin`, 4 MiB |
| Flash SHA256 | `c4263180bd9f6531408707c2ba2d628b98359d82fb012916b2d71a3e20a3caeb` |
| ELF SHA256 | `f27ed7ce28f787cbd882ce58373622683d981e8c44539b19db509692f698b41e` |
| Producer source SHA256 | `cd5467df69de44827f14651508797fcf2f6f742430af6bb99448e41c51c8b7b3` |
| Manifest | `tests/firmware/simd_reference/build/reference-manifest.json`; `capture_grace_ms=15000` |
| Fresh QEMU evidence | `build/runs/grace-official-01/capture`: complete 84 vectors, all initial Q/QACC inputs match, embedded ELF digest matches the rebuilt ELF |

The older immediate-start captures remain historical evidence. Use the new image
and its corresponding manifest for board deployment/comparison; a comparator must
not equate captures from different ELF/build identities. No hardware write was
performed during this rebuild/revalidation.

The separate radio profiles now use the same 15-second grace. Their frozen
deployment identities are:

| Profile | Merged flash SHA256 | ELF SHA256 |
| --- | --- | --- |
| Wi-Fi | `73928a983806aacba641aa75cb6897f3b95994d7d48785f3865b9ba7086c21b3` | `cab0bcf0a3bfa255207c22f0763b24bc02fcd1e41234f7bfe0713df883e747cc` |
| BLE | `435a71cd0daf9f4758f1929eafa032cf9afef3c2232348acb3ec3ad7218396d2` | `f8d5d9b6086114151644cc1600694aaccea2dcb3d037586a8a0fcf24a800cf9a` |

Corresponding manifests/images remain below `tests/firmware/radio_init/build/wifi`
and `build/ble`. The common radio source SHA256 is
`89ec6d551bbab046ac71d69575430c7a26fc70b6c2fb7119df0a416d1eb8e4c0`.
Fresh 35-second official-QEMU discoveries (`grace-wifi-official-02` and
`grace-ble-official-01`) validate their embedded identities and reported grace,
while preserving the native calibration stall and controller-clock assertion.
They remain nonpassing/incomplete radio evidence. The initial grace-Wi-Fi run
was rejected for a stale compiled source digest and retained; the build helper now
explicitly reconfigures before building to refresh embedded provenance.

Earlier Wi-Fi/BLE hashes and PC/trace captures below identify historical firmware,
not these newly prepared deployment images. No driver bypass or calibration skip
was introduced with the grace period.

## Evidence framing and comparison

Normal ROM/IDF logs coexist with lines prefixed `ESP32S3VM_REF `. Records use schema
1 and contiguous sequence numbers. Exactly one begin declares fixture, seed,
compiled source hash, embedded ELF digest and expected record count. Vector
records contain input bytes and captured before/after state. Radio records pair
started/result stages with attempted flag and numeric error. A valid end must
account for the entire declared catalogue. A complete radio batch containing an
error or skipped call is still unsuccessful.

The parser rejects malformed/duplicate-key JSON, NaN, invalid hex/state sizes,
duplicate/reordered sequence, incorrect source/catalogue/count, truncated batches
and records after end. Partial logs do not become passing evidence. Captures retain
`raw.log`, `records.json`, the original manifest and `metadata.json` with time,
session/tool/manifest/raw hashes and transport/board context.

The embedded application ELF digest is checked against the built ELF SHA256; this
was verified against the real IDF6.2-dev image. Manifest bytes and the tool hash
are frozen before acquisition so concurrent workspace changes cannot relabel a
running collection job. The copied QEMU flash must match that frozen manifest.

Import an existing log and compare completed captures:

```bash
python3 tools/hardware-reference.py --parse board.log \
  --manifest tests/firmware/simd_reference/build/reference-manifest.json \
  --output tests/firmware/simd_reference/build/captures/imported-board
python3 tools/hardware-reference.py --compare \
  tests/firmware/simd_reference/build/captures/board-01 \
  tests/firmware/simd_reference/build/runs/official-01/capture \
  --output tests/firmware/simd_reference/build/comparisons/board-vs-official
```

Importing a log does not establish hardware provenance. The comparator checks
capture integrity, identical build/input identities and byte-exact record states;
it retains every differing record and copies both evidence sets into a new result
directory. It never overwrites existing evidence. Board/revision context can differ
from the emulator's declared profile and must be reviewed, not silently equated.
Comparing QEMU with itself checks collection/repeatability, not hardware fidelity.

Host tooling verification:

```bash
python3 tests/hardware_reference_test.py
```

## Sources and remaining gates

Recorded verification on **2026-10-07**:

| Check | Observed result |
| --- | --- |
| Build-only script | `all wifi` and separate `radio_init ble` exit successfully and emit images/manifests |
| Host tooling | Nine tests pass, including malformed/partial identity/sequence rejection, explicit port control, cancellation, limits, mismatch retention and integrity checks |
| Real official QEMU PIE | Complete 84-vector batches; all captured initial Q/QACC bytes match supplied inputs; all 12 Q-move cases match; observed load/store increments are 128/128 |
| Repeat collection | Official runs `official-04` and `official-05` compare byte-for-byte with zero differences |
| Unsigned MAC edge | Encoded initial lane 1048565, input bytes 255 and 255, observed result 65014. This is a potential 20-bit wrap/saturation discrepancy, pending a physical board oracle |
| Native Wi-Fi | Both official9.2.2 and recovered-extension images initialize Wi-Fi and select station mode, then fail to return from `esp_wifi_start` within the 10 s discovery window during full RF calibration |
| Native BLE | Official runtime asserts during main-XTAL low-power-clock selection/divider configuration; recovered candidate proceeds further but faults reading address `0x3fc00100`. Neither completes controller initialization |

Wi-Fi bounded diagnostics retain CPU0 PC `0x40036a46`; its caller `0x42011772`
maps to the vendor `ram_read_sar2_code`. The matching ROM hash is
`b5c7090cc22efce51c1323cef31ca96fcd673a5b1d1b1017e219d393f2659913`.
Disassembly of the existing ROM shows a loop reading `0x6000e050`, extracting bits
24..26 and waiting for value 7. The recovered REGI2C model stores this offset and
special-cases only a different BBPLL-complete register. This identifies a missing
analog/SAR/PHY progression prerequisite for investigation; it does not justify
unconditionally asserting completion. Neither runtime passed the full Wi-Fi
lifecycle. No calibration-skipping firmware option was enabled.

The Wi-Fi ELF SHA256 is
`a6dbe9431a413c0d756933e29e50ddc0523b6d52d041ee3b067047aeebf43260`.
The application-stage trace's final analog-window writes before the poll include
`0x6000e060 <- 0x00ff501a`, `0x6000e05c <- 0x0080016a`, and
`0x6000e05c <- 0x0088016a`. The last two differ in bit 19. Public installed S3
headers did not provide an identified name/contract for the observed
`0x6000e050[26:24]` field; related SENS/APB SAR status definitions are at different
addresses. Treat the field label and full state transitions as unresolved rather
than mapping a convenient documented DONE bit onto it. A timed model must account
for requests, clocks, controller ownership, calibration/conversion and reset.

The BLE ELF SHA256 is
`d51bdbd532d96fe8510e75c7e56ffa7e2b72aaf44d5b6efb96b95dd485a403b3`.
In official QEMU, the backtrace identifies `btdm_low_power_mode_init` at the pinned
IDF's `components/bt/controller/esp32c3/bt.c:1757`, asserting the combined return
values of `btdm_lpclk_select_src(XTAL)` and `btdm_lpclk_set_div`. The precise failed
subcondition still needs investigation. In the recovered candidate, the fault is
`LoadStorePIFAddrError`, PC `0x40056f60`, EXCVADDR `0x3fc00100`, EXCCAUSE `0x0f`;
the vendor/ROM call chain includes `r_rw_rf_init`, `r_rwip_init`, `rw_pre_main` and
`btdm_controller_task`. Rebooted batches are rejected as incomplete evidence.

Raw and parsed evidence is in each fixture's ignored `build/runs` directory; it
is deliberately separate from source. Record these hashes/paths in the master
evidence manifest before packaging or clearing build output.

The fixture uses the installed real assembler and the S3 PIE state/instruction
contract from [TRM chapter 1](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf).
Actual vector load/store syntax is also present in
[official ESP-DSP assembly](https://github.com/espressif/esp-dsp/blob/master/modules/math/add/fixed/dsps_add_s16_aes3.S).
The radio boundary follows the normal
[Wi-Fi driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/overview.html)
and [controller lifecycle](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/controller_vhci.html).
Pin these references/toolchain revisions when extending the catalogue.

Hardware vectors and COM5 console delivery remain pending; board identity/read
preparation was performed by the parent as described above.
Instruction exception/context semantics, the rest of PIE, NimBLE, radio data/security
and stable/older framework matrices remain explicit future acceptance work in the
[development plan](development-plan.md) and [radio/SIMD plan](plans/radio-simd.md).
