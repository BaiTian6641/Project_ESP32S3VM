# Native I2S driver fixture

This folder is an ordinary ESP32-S3 ESP-IDF **6.1** application. Its version
guard rejects another IDF major/minor. Use the repository's `build-idf-6.1`
checkout, not an unpinned installed IDF. It never writes guest MMIO, uses private
I2S APIs, feeds samples through QMP, or enables GPIO signal-loopback.

## Defined vectors (not a verification claim)

| Mode | Driver vector coverage |
| --- | --- |
| Standard | 48 configurations: Philips, MSB, PCM-short; 8/16/24/32 bits; left mono or stereo; either controller master. Every configuration allocates both controllers full duplex, with the other controller slave, and checks both physical data directions. |
| TDM | 72 configurations: Philips, MSB, PCM-short; 8/16 bits in eight physical slots with masks `0x81`, `0x55`, `0xff`; 24/32 bits in four physical slots with masks `0x09`, `0x05`, `0x0f`; either controller master. `skip_mask=false` matches the active-slot-packed DMA vector, ordered by ascending enabled physical slot number. The S3 register's `skip_mask=true` contract instead requires a full total-slot stride and discards disabled-slot words. Both directions are checked. |
| Raw PDM | 4 configurations: packed 16-bit stereo words; either controller TX; either TX or RX controller master. Physical PDM clock is 2 MHz; DMA frame cadence is 62,500 Hz, not the 1 MHz configured raw-PDM rate. |
| Controller capabilities | Normal-driver initialization of raw/PCM TX/RX on both controllers. PCM2PDM/PDM2PCM must reject I2S1 with `ESP_ERR_NOT_SUPPORTED`; raw PDM must initialize on both. This is a driver capability check, **not converter sample qualification**. |
| TDM frame limit | Eight normal-driver rejection observations: eight-slot 24/32-bit TX/RX on both controllers must return `ESP_ERR_INVALID_ARG`. S3 physically supports at most 128 bits/frame; oversized frames are never silently truncated or qualified. |
| Circular DMA/lifecycle | Standard/TDM use four 96-frame descriptors, eight sustained read/write blocks, 80 ms with no producer/consumer, four observed starved blocks, disabled-read/write rejection and halted TX EOF counts, then four restarted read/write blocks. Auto-clear is disabled deliberately: driver-owned circular DMA replays actual last written samples during starvation. This is not a hardware-underrun assertion. |

S3 IDF 6.1 DMA sample storage is packed 1/2/3/4 bytes for 8/16/24/32-bit
samples (`i2s_get_buf_size`), rather than the older high-byte/high-24-bit
container convention. Every 96-frame period contains deterministic, nonzero,
slot- and controller-dependent data. Both guest and independent host generator
calculate exact FNV-1a hashes over these packed bytes. The fixture allows only a
whole-frame cyclic phase, checks an entire descriptor before accepting that
phase, and rejects a phase change within a capture. There is no expected-output
copy from received data and no zero/echo fallback.

The standard/TDM frame clock is exactly specified as `12500/1` Hz and
`80000/1` ns/frame. Standard uses MCLK multiples 256 (8/16/32-bit) or 384
(24-bit); TDM uses 768 to satisfy the driver's receive divider constraint.
`expected_vectors.py` independently derives BCLK and rational DMA cadence.
Cadence observations are real `on_sent` callback timestamps from
`esp_timer_get_time`, not electrical-edge timestamps. The host permits 2% or
2 ms ISR-service/quantization tolerance, whichever is larger. Restart timestamps
are reset while both controllers are disabled, excluding the intentional pause.
This check does not claim sub-microsecond pin timing qualification.

## Native electrical graph

`graph_vectors.py` emits project schema version 3, using public QMP Apply on
`/machine/soc/electrical`:

- Explicit 3.3 V source, ground reference, MCU VDD/GND rails.
- GPIO18--GPIO4 BCLK; GPIO19--GPIO5 WS.
- GPIO20 TX0--GPIO7 RX1; GPIO6 TX1--GPIO21 RX0.
- Separate 10 kOhm BCLK and WS idle-low branches to ground. Low is the
  deasserted PCM-short pulse level; active masters drive the actual WS polarity.
- Distinct input/output pins: no shared-pin IDF loopback special case.

`disconnected-data` splits both data nets and places a physical 10 kOhm
pull-down on each RX pin. The negative runner requires both first-case
receivers to report exactly 1,536 clocked bytes with the independently computed
all-low hash and eight or more actual TX/RX DMA callbacks. An arbitrary error,
abort, missing clock, or mismatched random data is not accepted as a negative
success. These low samples arise from explicit electrical pull-downs, not a
controller fallback.

**External registered peer runtime coverage is unqualified.** The actual
powered `i2s-sample-peer` registry/terminal binding and a separate 68-case
ordinary driver fixture now exist under `peer_fixture/` and `newpeer/`.
This wire fixture's `external-peer` scenario still reports `UNQUALIFIED` and
refuses to launch; cross-controller wires are not an external device.
The separate peer runner requires real graph Apply, immutable actual RX/DIN
records, and independent source-transition attribution. Its built firmware and
synthetic reference tests do not qualify physical transport. PCM2PDM/PDM2PCM
exact output also remains unqualified without an authoritative converter
reference; neither fixture invents coefficients.

## Host-only unit seams

Run from this directory:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v test_host_vectors
```

These tests cover matrix uniqueness/counts, packed sample bytes and slot order,
known FNV-1a vectors, cyclic frame rotation, rational cadence, connected and
disconnected graph topology, unqualified-peer rejection, and parser acceptance
and rejection. The complete parser transcript is **synthetic unit input**,
not captured firmware output or evidence of controller/runtime qualification.

## Build evidence and gated smoke

The corrected `skip_mask=false` application was built through the canonical
native-Linux IDF 6.1 activation script for pinned commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. Build-only evidence is in
`build-runtime-state/i2s-native-2026-10-07/firmware-fixed-tdm`: reproducible
`build-command.sh`, full build log, immutable exact source copy and source
manifest, SDK manifest, ELF/application/bootloader/partition binaries,
configuration/flash layout, 4 MiB `i2s-native.merged.bin`, SHA-256 inventory,
structured build result, and offline ELF-bound GDB source/address manifest.
The native content-addressed build uses the canonical SDK `builds` root and
native compiler cache; its single build completed successfully. No compiler fix
was needed. The merged image SHA-256 is
`bb78309eed975628ac07bf5c9f2f2d76cb2e88dbbc7c8e39fcb6f87677c3581a`.

The earlier `firmware` directory and old image
`965df0d22949e21a8bc0afcb1577a9c8740d57f857804237bed9735d121aa066`
remain untouched historical **build-only** evidence, including its initial
1,800-second interrupted attempt and continuation. That old build used
`skip_mask=true` with active-slot-packed vectors and is withdrawn from full
TDM/runtime qualification; never use it as the golden firmware. All 11 earlier
host-only unit tests passed using synthetic input, not runtime captures.

Vendor activation/configuration notes, unavailable Git-description warning,
private-include CMake warnings, and esptool legacy-option/command deprecation
warnings are preserved in the logs; vendor sources were not modified.
**No hardware or QEMU runtime was exercised by this build verification.**
The listed case/record totals remain expected acceptance criteria, not runtime
observations. Callback cadence is not an electrical-edge timing qualification.

Ordinary wire and peer qualification additionally require the same new native
I2S binary derived from CombinedVerify's corrected 40-patch parent source
`a45abaa18d905226`. Its source record is now published, but Main requires
NativeNet's renewed coherent freeze covering GPIO `722b9e31…` ordering after
pad corrections/tests and current input hashes, including the reset transaction
`prototypes/coreclk/0016-system-capture-reset-entry-cause-gate-S3-reset.patch`
(SHA256 `f1a3c4986080edb56fe679b451427157015761ca1f68b5142eaea9f878521898`).
This reset-domain patch is not the separate historical SYSTEM-storage `0016`
mentioned in the receipt's evidence boundary. The renewed freeze is required
before any I2S overlay derivation or verification. Previous FC61/c6fc receipts,
historical `19e9…` binaries, and the I2S-only historical delta cannot qualify
this gate. See `qemu-extensions/prototypes/i2s/README.md` for exact source and
preserved build/failure identities.

After this coherent controller/electrical runtime overlay is ready:

1. Use `firmware-fixed-tdm/i2s-native.merged.bin` above, or reproduce that
   directory's build-only command script in WSL Ubuntu as user `polar` with
   the recorded native `CCACHE_DIR`.
2. Never write to an original corpus image or hardware.
3. Use the integrated native QEMU binary and an unused evidence directory:

   ```sh
   python3 tests/firmware/i2s_native/run_fixture.py \
     --qemu /absolute/path/to/native/qemu-system-xtensa \
     --flash /absolute/path/to/new/i2s-fixture-flash.bin \
     --evidence /absolute/path/to/new/i2s-connected-run
   ```

4. Repeat with `--scenario disconnected-data` in another new evidence
   directory. Inspect any failure before reporting qualification.

The runner uses Unix QMP sockets and is intended for the same Linux/WSL
execution environment as the native runtime. It copies the supplied flash into
its new evidence directory before QEMU launch and hashes the original before
and after. QEMU only receives the local copy. Evidence includes the exact
project graph, expected mode matrix, UART/QEMU logs, snapshots, binary/image
hashes, and a structured result. Connected success is named
`PASS_GPIO_CROSS_CONTROLLER`, deliberately not full native-I2S qualification.
It requires all 124 cases, 724 exact-sample records, 364 callback-cadence records,
240 disable records, eight capability records, eight oversized-frame rejection
records, and zero firmware failures.
