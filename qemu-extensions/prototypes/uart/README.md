# ESP32-S3 native UART overlay

This GPL-2.0-or-later lane implements UART-01/02/03 source behavior against the
locked official QEMU commit `40edccac415693c5130f91c01d84176ae6008566` and the
immutable 26-patch prefix `dbe2a8a5ad4e07a607e76ab751e77ac85fe3bf30974359bd25a69354499f9b32`.
It does not declare the 119-package plan complete, blanket UART support, or
independently qualified hardware timing. QEMU remains a separate executable;
these GPL native models are never linked into Qt.

## Source and integration contract

`source-map.json` is the copy/patch/machine ordering contract and precise mode
qualification matrix. `uhci-integration.json` records target-specific DMA and
shared-memory references, native vector linking, and unresolved primary-spec
requirements. Existing aggregate profiles and sibling source files are not
modified by this lane.

1. Apply the recorded immutable prefix.
2. Apply the integration owner's frozen native foundations: central clock ports,
   GPIO raw-drive/strict sampling, the single SENS owner, and actual v3 electrical
   graph published-frame subscriptions. UART direct IO_MUX output mappings belong
   to the electrical owner, not to a second GPIO source store.
3. Preserve the SPI RX-EOF overlay bytes
   `d0a472c53f444304e29dbd4c979d60840e73a42bfbc935277041deec6b8ad422`.
4. Apply GDMA owner `0004-hw-dma-esp_gdma-served-RX-bytes-survive-post-store-a.patch`.
   The author's pinned revision `7640a6e06bf9c7d9919cbd4837c25dab3e85f0dfb03da74eb77ee3718dead251`
   was later revised by the GDMA owner; the reviewed dependency record carries the
   current revision `77b875566f9b1d6a4a605c8995a8bed5cdeae604c93b427932a74df39e852295`,
   which is the artifact this lane's shared API patch is layered on. The old
   revision no longer exists in the tree and is not silently substituted.
5. Apply the UART-owned shared DMA API patch
   `0001-gdma-uhci-demand-and-packet-status.patch`
   (**raw** `2626ab0cad1955d02bc20cb9a190bc782a388eea0fac452b3524f15b4ab7b9fa`).
   It supplies actual accepted counts/demand/packet EOF and the real I2S continuous
   RX-segment hook; it does not replace the earlier SPI artifact.
   Historical `3a8169...` was a normalized-text hash, not the raw artifact
   identity. Only Main-approved RAW2626 is accepted; original CRLF bytes are
   preserved. The source/application order and reviewed logic are unchanged.
   Main-approved `patch_context=ignore-space-change` applies only as an explicit
   matching operation on RAW2626; no input normalization or retry fallback.
   The lane's own `integration.patch`, `integration-electrical.patch` and
   `integration-qtest.patch` were re-authored as LF patches with the actual
   frozen-foundation context and exact line numbers (their authored bytes were
   CRLF copies of the parent-state context and could not be matched by a strict
   apply); added/removed content is byte-identical modulo line endings and they
   now apply strictly. They no longer carry patch-context exceptions, so
   `source-map.json`'s `patch_context` records RAW2626 only.
   Other patches remain strict, exact numstat target scope is required, and
   application policy plus resulting source hashes are included in provenance.
6. Apply `0002-parent-uart-defer-fifo.patch`, then the seven declared copies.
   The inherited parent keeps ordinary ESP32/C3 allocation; S3 attaches actual
   shared RAM without first allocating private FIFO buffers.
7. Apply `integration.patch`, `integration-electrical.patch`,
   `integration-qtest.patch`, and `integration-unit.patch`.

The three UART controllers remain `/machine/soc/uart0`, `uart1`, `uart2`, mapped
at `0x60000000`, `0x60010000`, `0x6002e000`, each through S3 ID/REG_UPDATE at
`0x80`. `/machine/soc/uart-memory` owns one physical 1024-byte array.
`/machine/soc/uhci` is the single UHCI0 controller at `0x60014000`, linked to the
actual GDMA controller, interrupt matrix, reset-domain provider and UART FIFOs.
No SPI0/1 operation is altered.

UART source clocks are common `apb-clk`, `xtal-clk`, `rc-fast-clk`, connected before
UART realization. Common UART0/1/2 gates and held-reset inputs stop progression
without discarding queued bytes; explicit reset clears controller state. Clock
and divider changes retime pending phases using rational virtual-clock periods.
CPU-only SoC resets preserve peripheral state through the inherited reset-domain
contract. Neither source timestamps nor UART timers use wall time.

## Electrical and console separation

The only output authority is
`esp32s3_electrical_set_matrix_drive(dev, signal, oe, level, open_drain)`.
RX/TX matrix IDs are **12/15/18**, CTS/RTS **13/16/19**, from the pinned S3
`gpio_sig_map.h`. Direct UART inputs validate IO_MUX ownership and FUN_IE before
using the actual solved pad. Matrix and direct inputs reject unresolved levels
and invoke the canonical strict unknown-consumption gate when a bit or CTS/idle
condition is actually consumed. Held GPIO samples are never accepted as valid.

Each controller subscribes to post-solved/published physical frames, including
actual edges, route/IE changes, topology, power and RC updates. Desired UART
signals survive circuit replacement; physical reachability never comes from a
hidden MCU peer or console wire. External proof uses explicitly crossed MCU
TX/RX nets with explicit rails and pull resistors, not echo surrogate devices.

The chardev observes completed TX frames independently of those external nets.
Its bounded incoming queue is an explicit 8N1 host-console sender at
`console-baud` (default 115200), with actual virtual-time frame events available
to ROM autobaud. It never drives external pads or produces external UHCI idle
EOF events. UART0 ROM/download and ordinary console compatibility require fresh
same-binary regressions; prior prefix boot evidence is not evidence for this
replacement.

## Native behavior and qualification boundaries

UART TX/RX uses fixed storage and bounded persistent timers. Data widths 5–8,
even/odd parity, 1/1.5/2 stops, break, actual parity/frame/overflow errors,
threshold/timeout interrupts, RTS watermarks, CTS stall/resume, REG_UPDATE,
clock/gate/reset transitions, RS485 physical echo/collision and turnaround/DE
behavior, IrDA half-duplex mid-bit 2/16 or 3/16 pulses, actual-edge autobaud and AT
PRE/GAP/POST detection have separate authored
vectors and separate qualification rows. TX_DONE waits for configured TX idle.
Physical UART idle EOF is independent of RXFIFO timeout enable and remains
observable after GDMA drains the FIFO.

IrDA follows TRM26.4.7: TX_EN selects transmit versus receive, the receiver expects
idle HIGH/active-LOW pulses, and encoder zero pulses occupy cycles9–11 (ticks8..11)
with WCTL selecting whether cycle11 remains HIGH. Native vectors use100kbit/s,
within the documented115.2kbit/s SIR range. The pinned public IDF UART API exposes
receive-only mode but no TX_EN/WCTL setters; ordinary firmware therefore exercises
genuine GPIO-generated9600bit/s input and explicitly reports unavailable TX
direction/WCTL controls rather than pretending ordinary full-duplex IrDA works.

Source-divider nonzero A/B arithmetic has conflicting primary descriptions
(TRM B/A versus register comments A/B); the pinned HAL only programs NUM+1.
The model interpretation is [INFERENCE] and remains target-unqualified.
UART CLKDIV_FRAG uses rational-average bit timing; independently referenced
16-pulse phase/interleave jitter fidelity is unqualified. Integer-source and
average-baud software evidence must not be promoted into hardware cycle proof.

All six FIFO windows alias the same RAM: TX bases 0/128/256, RX 512/640/768;
maximum documented blocks TX 8/7/6 and RX 4/3/2. Expansion does not move bases or
gain private storage. The TRM explicitly makes UART1 TX unusable when UART0 TX
occupies two blocks. Concurrent non-overlapping controllers and DMA channels
must be exercised separately from intentionally aliased memory. Zero-size and
out-of-extent native vectors are memory-safety boundaries only: **SIZE=0 encoding
is unresolved**, not silently qualified as disabled or eight blocks.

UHCI raw bytes, programmed escape pairs, separator framing, length/idle/break
EOF, correct channel/descriptor ownership, actual accepted counts and error EOF
have native register/controller vectors. Ordinary pinned IDF UHCI proof is a
separate real UART1/GDMA path with a UART2 physical peer and concurrent UART0
physical loopback. The public pinned `driver/uhci.h` exposes no separator/escape
configuration, and its implementation disables separator substitution; native
register vectors must not be mislabeled ordinary-driver evidence.

**Unresolved primary target specifications:** UHCI header byte layout,
sequence/ack/checksum transformations, CRC seed/byte order, quick-send NUM
meaning, hung-timeout clock units, and shared FIFO SIZE=0 encoding are absent
from the inspected target TRM/register/HAL sources. These rows remain explicitly
unqualified. Unknown packet modes never fabricate packets or successful status.
RS485 digital comparison is not a differential transceiver qualification; IrDA
pulses are not optical-medium qualification. No hardware operation is performed.

Primary references: [ESP32-S3 TRM v1.8, UART chapter26](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf),
pinned `build-idf-6.1/components/soc/esp32s3/register/soc/{uart,uhci}_{reg,struct}.h`,
and `components/esp_hal_uart/esp32s3/include/hal/{uart,uhci}_ll.h`.

## Immutable preparation and delegated execution

The integration owner publishes the frozen common profile/source record.
`prepare.py` takes that **hash-recorded source record**, not a mutable build
cache, and constructs a unique new `qemu-uart-40edccac4156-<fingerprint>` checkout.
Existing destinations are preserved, and every applied source/patch identity and
resulting touched source hash is recorded under the evidence directory.

```sh
python3 qemu-extensions/prototypes/uart/prepare.py --dependencies "$FOUNDATION_SOURCE_RECORD"
```

A verification subagent, not the UART author, must configure/build that exact
checkout and run the complete authored native targets:

```sh
ninja -C "$BUILD" qemu-system-xtensa tests/qtest/esp32s3-uart-test test-esp32s3-uhci
UART_QTEST_PROJECT_DIR="$ROOT/qemu-extensions/prototypes/uart/firmware" \
QTEST_QEMU_BINARY="$BUILD/qemu-system-xtensa" \
  "$BUILD/tests/qtest/esp32s3-uart-test" --verbose
"$BUILD/test-esp32s3-uhci" --tap -k
```

The qtests assert actual GPIO samples and per-bit native trace timestamps. The
per-UART `trace` property enables `tx-drive` and `rx-sample` events, uses virtual
nanoseconds, and flushes QEMU logs for evidence barriers.

The ordinary driver also selects UART1's direct IO_MUX TX function on GPIO17,
not its GPIO-matrix output. The electrical solver resolves native U0/U1 TX/RTS
pad functions from the same controller drive sources. The regression
`iomux-tx-release-restore` checks actual GPIO16 waveforms and UART2 received
bytes with GPIO17's matrix output and GPIO output-enable disabled, then verifies
that changing the mux releases the wire before restoring it.

`TX_DONE` is latched on final busy-to-idle completion. Clearing it while idle
must not immediately regenerate it; `tx-done-clear-next-completion` checks that
invariant and the next real transfer's IRQ on each UART. The current native
suite is **64/64 UART + 17/17 UHCI**, zero skipped, on source-bound
candidate `1e937cf0ad1822bb`. All five strict ordinary
connected/absent/disconnected/wrong/UHCI firmware runners **PASS** on that same
executable. This includes FIFO/error recovery, physical flow control and queued
VM pause/resume, digital RS485, receive-only IrDA, and actual 513-byte UHCI DMA
idle/multibuffer/length/break packets with partial callbacks and TX completion.
The central GDMA `IN_DONE` descriptor-boundary correction prevents the ordinary
driver from recycling buffers after each single-byte pump.

Arduino, ROM download, undocumented packet fields and independent hardware
timing remain separate gates. See `source-map.json` and
`build-runtime-state/uart-continuation/i2c-slave-final/qualification-receipt.json`.


`tests/firmware/uart_native/README.md` contains ordinary pinned IDF build/freeze
and runner commands, including actual UHCI and the separately pinned Arduino
wrapper where available. Firmware images, ELF, source/config/graphs, runtime
source record and exact executable identity are frozen before any run. Each run
copies flash into an evidence-local writable disk; original corpus/frozen bytes
are not launched writable or altered. Stop/resume changes only a real resistor
on GPIO10 while physical CTS holds a queued transfer; it does not inject guest
bytes or fake UART status.

Runtime evidence and the exact mode qualification matrix must be updated after
those delegated runs. Authored vectors and static review are not runtime passes.
Ordinary boot/download regressions, all physical firmware modes and native DMA
ownership/error/concurrency vectors remain required for this exact executable.
