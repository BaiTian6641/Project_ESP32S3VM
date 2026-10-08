# gdma — persistent bounded GDMA descriptors/cursors/handshakes (H-CORE-03 / CORE-04)

Tracked patches for the ESP32-S3 GDMA model (`esp32s3.gdma` + generic
`esp.gdma` engine), applied on top of the pinned official QEMU base
`40edccac415693c5130f91c01d84176ae6008566`, in order (re-issued
2026-10-07 after independent review; the earlier 0001-0003 are
superseded — see `build-runtime-state/gdma-2026-10-07-fix/`):

1. `0001-hw-dma-esp_gdma-bounded-descriptor-walking-persisten.patch` —
   engine: TRM 3.4.6 descriptor checks (owner, DW1/DW2 windows, inlink
   burst alignment with explicit internal/external window test — the
   internal window lies numerically above the external base),
   persistent descriptor+byte cursors, bounded work quantum continued
   through a main-loop bottom half (valid streaming rings yield per
   main-loop iteration; no zero-delay timer re-arm, so QMP/vCPU are
   never starved and the stream never stalls on a frozen clock),
   non-progressing-cycle rejection, per-quantum/per-pump/per-restart
   walk bounds, halt state after DSCR_ERR, LINK PARK/AUTO_RET/STOP/
   RESTART semantics per TRM, CONF0 RST FSM reset, EOF descriptor
   registers, per-direction errors.
2. `0002-hw-dma-esp32s3_gdma-validate-GDMA_PERI_SEL-periphera.patch` —
   ESP32-S3 `is_periph_invalid`: PERI_SEL 0..9 valid (SPI2, SPI3, UHCI0,
   I2S0, I2S1, LCD/CAM, AES, SHA, ADC, RMT), 10..63 invalid (TRM table
   3.4-1).
3. `0003-tests-qtest-add-esp32s3-gdma-test-suite.patch` —
   `tests/qtest/esp32s3-gdma-test.c` + meson registration; 16 cases
   (chain/owner, owner/window/burst-alignment/PSRAM errors, restart
   cursor, quantum streaming + mid-stream reset, self-loops, dual
   channel, PERI_SEL, PARK/STOP, partial boundaries, internal-RAM
   word-alignment window discrimination, QMP responsiveness with a
   running vCPU across many quanta, non-cumulative walk bound,
   repeated RESTART streaming, system reset).

Files touched: `hw/dma/esp_gdma.c`, `hw/dma/esp32s3_gdma.c`,
`include/hw/dma/esp_gdma.h`, `tests/qtest/meson.build`,
`tests/qtest/esp32s3-gdma-test.c`. GPL-2.0-or-later; upstream headers
preserved.

Behavior contract (short form; full evidence in
`build-runtime-state/gdma-2026-10-07/` and
`build-runtime-state/gdma-2026-10-07-fix/`):

- Descriptors are re-fetched from guest memory following TRM 3.4.6 rules;
  a failed check raises `GDMA_x_DSCR_ERR` and halts the channel until
  RST (CONF0 bit 0, 1→0) or a fresh START — never an infinite host loop
  and never a host assert.
- A chain that still has work at a quantum boundary (256 descriptor
  fetches) stays armed — `GDMA_xLINK.PARK` reads 0 — and continues
  through a bottom half; valid circular streaming is supported, cursors
  are (descriptor, byte) precise. Walk bounds never accumulate: per
  quantum slice, per pump call, per RESTART.
- Memory-to-memory (MEM_TRANS_EN, both links (RE)STARTed) completes with
  OUT DONE|EOF (+TOTAL_EOF only at true list end) and IN DONE|SUC_EOF;
  RX writeback always clears owner and records received length; TX
  writeback clears owner only with OUT_AUTO_WRBACK.
- Peripheral handshake hooks: a non-M2M channel arms its chain at LINK
  START/RESTART; the future peripheral pumps data through
  `esp_gdma_read_channel` / `esp_gdma_write_channel` after locating the
  channel via `esp_gdma_get_channel_periph` (peripheral must be valid
  for the target AND the channel armed for that direction). No data is
  invented when no peripheral model exists.

Known boundary: UART/SPI/I2S/LCD-CAM/AES/SHA/ADC/RMT peripheral models
that would drive real handshakes do not exist yet (AES/SHA in-tree
models keep working through the same pump API); PSRAM (external RAM
window) is not mapped — transfers there fail at the bus with DSCR_ERR.
Stacking: the series currently applies on pristine 40edccac; it will be
regenerated on top of the coreclk (H-CORE-01) series once that lands
(both touch esp_gdma.c/esp_gdma.h), per coordinator instruction.

Verification: `meson test qtest-xtensa/esp32s3-gdma-test` 1/1 (16/16
cases), BootSmokeTest 3/3 with the lane-built
`qemu-system-xtensa` (sha256 f7d760793e8d1183a9002dac499081996460d9f1747c6c2482590e3941e5f6dd).
