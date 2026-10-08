# radio-bringup — BLE controller bring-up + REGI2C/SAR2 sequencing on the pinned official QEMU base (M8)

Status: **prototype-unqualified**. Lanes: native BLE controller bring-up slice
(RadioBringup) + REGI2C/SAR2 analog-master lane (Regi2cWorker).
Date: 2026-10-07. Owners: radio-bringup worker, regi2c-sar worker.

## What this is

Three small, reviewable patches against the pristine official QEMU base
(`40edccac415693c5130f91c01d84176ae6008566`, esp-develop 9.2.2 lineage). No
firmware hooks, no symbol interception, no unconditional DONE bits, no
fabricated measurement data.

| Patch | Core paths | Behavior |
| --- | --- | --- |
| `0001-hw-xtensa-esp32s3_clk-store-SYSTEM_BT_LPCK-divider-r.patch` (sha256 `0a1d32af…`) | `include/hw/xtensa/esp32s3_clk.h`, `hw/xtensa/esp32s3_clk.c` | `SYSTEM_BT_LPCK_DIV_INT/FRAC` (0x600C0028/2C) stored R/W with IDF-documented reset defaults and field masking. |
| `0002-hw-xtensa-esp32s3-map-undocumented-BT-modem-EM-RAM-w.patch` (sha256 `ea36fdc3…`) | `hw/xtensa/esp32s3.c` | Bounded 64 KiB RAM window at `0x3FC00000` (undocumented BT modem/EM), not aliased to DRAM. |
| `0003-hw-misc-esp32s3_regi2c-model-REGI2C-analog-I2C-maste.patch` (sha256 `845217d8…`, **FROZEN**) | `hw/misc/esp32s3_regi2c.c`, `include/hw/misc/esp32s3_regi2c{,_defs}.h`, `hw/xtensa/esp32s3.c`, `hw/misc/meson.build` | **SEQUENCING-ONLY, FAIL-CLOSED** model of the undocumented REGI2C/I2C_MST block `0x6000E000-0x6000E0FF`: timed parallel engine words (+0x00/+0x04, BIT25 busy self-clears at divider-derived I2C wire time), analog-slave **config register file** (ROM grammar is read-modify-write everywhere, including the bootloader's early-boot RMW of slave `0x69` reg `7`), SAR2/PWDT phase sequencer (`+0x50/0x5c/0x60`, FIFO `+0x80..+0x9c`, armed by the SENS force pair), TX-DC timed op (`+0x4c`), TSENS contract (`0x60008850`). **Measurement data (SAR2 codes, TSENS sample, TX-DC done/flags) requires a configured measurement provider** (QOM link `meas-provider`, interface `esp32s3-regi2c-meas-provider`); without one the device reports the exact dependency once and **pauses the VM before firmware consumption** (main-BH `vm_stop(RUN_STATE_PAUSED)`; no DONE+0, no filler values, no automatic resume). The 0x6000E000 window maps at overlap **priority 1** (owning model outranks the superseded coreclk 0007 bridge). `regi2c_wifi_analog_modeled=false` — no analog/radio data model exists. |
| `0004-hw-misc-esp32s3_regi2c-model-BBPLL_CAL_DONE-0x40-BIT.patch` (sha256 `b7224516…`, **FROZEN**) | `hw/misc/esp32s3_regi2c.c`, `include/hw/misc/esp32s3_regi2c{,_defs}.h` | **BBPLL_CAL_DONE (0x6000E040 BIT24) as timed engine status**, superseding coreclk's temporary 0007 bridge: the `clk_ll_bbpll_calibration_start` pattern (`STOP_FORCE_LOW` set / `STOP_FORCE_HIGH` clear, two RMW steps) clears BIT24 and starts a modeled 40 µs calibration window; `calibration_stop` leaves it set until the next start; reset leaves it clear. Completion is engine STATUS, not data (no provider needed). From-reset combined-profile trace: start (`←0x0`, `←0x8`) → BBPLL config RMWs (slave 0x66) → `calibration_stop` RMW **reads back the modeled DONE=1** (`←0x1000000`); boot smoke 3/3 without 0007. |
| `0005-regi2c-config-accessor-and-slave-range.patch` (sha256 `d1d4966b…`, **ADDITIVE — apply after frozen 0004**) | `hw/misc/esp32s3_regi2c.c`, `include/hw/misc/esp32s3_regi2c{,_defs}.h` | (1) Canonical typed getter `esp32s3_regi2c_get_slave_reg(dev, slave, reg, &value, &written)` exposing ONLY the analog-slave CONFIG register file for the ADC lane's 0005 consumption (typed QOM link to `TYPE_ESP32S3_REGI2C` set before realize; consumers gate on `*written` — never-written storage 0 is UNKNOWN reset state, not a programmed bit; false return = fail closed). (2) Guest-reachable index fix: `REGI2C_ENG_SLAVE_OF` masks the slave byte to the 7-bit I2C address space before indexing the 128-row register file (review MUST-FIX; was OOB for slave bytes ≥ 0x80). No runtime behavior change for evidenced firmware paths (boot smoke 3/3; wifi unchanged single SAR2 fail-closed pause; 0 unimplemented/guest-error lines). |

**STACKING / FREEZE**: radio-bringup 0001..0004 are **frozen inputs** (exact
bytes, sha256s above — restored byte-exact after an accidental regeneration;
provenance authoritative in the runtime-db source records). The getter +
7-bit slave-index fix are published **additively** as 0005. Combined apply
order is `git am --keep-cr`: coreclk 0001..0007, then radio-bringup
0001..0004 (frozen), then radio-bringup 0005 (gdma series disjoint; ADC
SENS-retirement patch lands in its own folder). Pipeline simulation applies
clean and is tree-identical to the verified tested build
(`f5b98dde3238f5afe76f6bc38a7f0f1b73ac34c7aa297d051880c4e1cbe99fce`); see
`build-runtime-state/regi2c-2026-10-07-stacked/README-evidence.md`.

## Why (evidence summary)

* **LPCK readback contract** — blob verifies LPCLK config by write-then-READBACK
  (`bt.c:1749-1757` assert). Official QEMU answered reads with 0 → assert.
* **EM RAM window** — `r_rf_em_init` ROM-`memcpy`s 40 bytes to `0x3FC00000+0x100`;
  official QEMU raised `LoadStorePIFAddrError` (EXCCAUSE 0x0f).
* **REGI2C/SAR2 transaction grammar** — decoded from `esp32s3_rev0_rom.elf`
  (rom_i2c_paral_write/read, rom_pwdet_sar2_init, rom_pkdet_vol_start,
  rom_read_sar_dout, rom_i2cmst_reg_init) plus ordered MMIO write traces of
  both fixtures (226 writes each, identical): the ROM polls
  `0x6000E050[26:24] == 7` at PC `0x40036a46` after the `+0x5c` BIT23/BIT19
  SAR2 trigger, and `rom_read_sar_dout` drains 8×13-bit codes from
  `0x6000E080..0x9C`. Full decode: `build-runtime-state/regi2c-2026-10-07/baseline/trace-analysis.md`.
* **Consumer side** — `libphy.a:phy_pwdet.o`: `ram_read_sar2_code` consumes
  FIFO word[1]; `pwdet_tone_start` reuses the `0x50` BIT1 toggle + poll;
  references come from `phy_param` (runtime cal data), never the SAR2 window.
  Slave `0x69` reg `7` = the IDF-documented `I2C_SARADC_DTEST_RTC/ENT_TSENS/
  ENCAL_REF` config word, RMW'd by `regi2c_saradc_enable()` and by the
  **bootloader during early boot** (rev B runs prove the boot-time read).

## Stage comparison (radio_init fixtures, rev B = qualified default)

| Stage | Official base | 0001+0002 | **0003 rev B (fail-closed)** | Hardware rev 0.2 |
| --- | --- | --- | --- | --- |
| BLE `esp_bt_controller_init` | assert `:1757` | ok | **ok** (full `BLE_INIT` banner, `result ok`) | ok |
| BLE `esp_bt_controller_enable` | not reached | stall at REGI2C poll `0x40036a46` | enters full cal; **VM pauses at the first measurement consumption** (SAR2/PWDT conversion #1) with one precise diagnostic | ok |
| Wi-Fi `esp_wifi_start` | stall at `0x40036a46` | identical stall | all 12 sequencing records to `esp_wifi_start` enter; **pauses at SAR2 conversion #1** | ok |
| Boot smoke (IDF 6.1) | 3/3 | 3/3 | **3/3** (bootloader config RMWs served from the modeled slave register file) | n/a |
| Unimplemented/guest-error lines | — | 0 | **0** | n/a |

The unqualified exploratory rev A runs (fail-open: zeros latched with UNKNOWN
logging; firmware advanced into TX-DC/TSENS and stalled at `ram_iq_est_enable`
polling `0x60006174` BIT16 in the unmodeled modem window `0x60006000-0x60007fff`)
are preserved **for discovery only** in
`build-runtime-state/regi2c-2026-10-07/exploratory/EXPLORATORY-UNQUALIFIED.md`.

## Measurement unknowns / instruments needed (for the parent)

Real sample capture requires a hardware MMIO **read** trace during the same
calibration flow (the rebaselined capture has stage records only):
1. SAR2/PWDT codes: reads of `0x6000E080..0x9C` during full cal (consumer:
   `rom_read_sar2_code`, averages 4×, `libphy` uses word[1]).
2. Analog I2C read-data bytes per (slave, reg), from reset through cal
   (`0x6000E000/0x004` read latch values; slaves 0x61-0x6b observed).
3. TSENS raw counts + `+0x58`/`0x8850` config during TSENS conversion.
4. `+0x4c` TX-DC DONE/direction-flag readbacks during TX-DC cal.
5. `+0x50[26:24]` busy encodings during the pkdet flow.

## Reproduce

```bash
git clone --no-hardlinks --no-checkout <repo>/build-qemu-official-base \
    /home/polar/.cache/esp32s3vm/qemu-radio-40edccac4156
cd /home/polar/.cache/esp32s3vm/qemu-radio-40edccac4156
git checkout --detach 40edccac415693c5130f91c01d84176ae6008566
git switch -c codex/regi2c-sar
git am <this directory>/0001-*.patch <this directory>/0002-*.patch <this directory>/0003-*.patch
mkdir build-radio && cd build-radio
bash ../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp \
    --disable-docs --disable-werror --disable-user --disable-tools \
    --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
    --disable-opengl --disable-capstone
ninja -j 4 qemu-system-xtensa
# expected binary sha256 c18956965022315b9c9c42a6f19f3030628f7e148563ee92528467311774f4ed
```

Fixture run (`tools/hardware-reference.py --qemu`, same invocation as before,
outputs under `build-runtime-state/regi2c-2026-10-07/fixed/*-revb-01`).
Expect: sequencing stages pass, then one
`esp32s3.regi2c: measurement/data dependency unavailable ...` line and a paused
VM (explicit `cont` resumes; it fails again at the same dependency unless a
measurement provider is attached).

## Known boundaries / UNKNOWNs

* REGI2C `+0x50/0x54/0x58/0x5c/0x60/0xc4` true field maps; `[26:24]` busy
  encodings UNKNOWN (idle 7 evidenced); `+0x58[7:0]` divider semantics UNKNOWN.
* Analog slave register file reset defaults UNKNOWN (modeled 0; only
  write-then-read-back semantics evidenced). **Never-written config reads are
  storage semantics** — they return the modeled reset state (0), they are not
  measured values; the measurement surfaces (SAR2 codes / TSENS sample /
  TX-DC done+flags) stay fail-closed behind `meas-provider`. No
  strict-mode flag exists; if one is ever added it will be a documented
  property, never a silent behavior change.
* SAR2 codes / TSENS counts / TX-DC flags: UNKNOWN analog values — fail-closed
  behind `meas-provider`.
* `pll_cal exceeds 2ms` timeouts + `0x60006174` IQ-done flag: unmodeled modem
  window (next bring-up slice, discovery evidence unqualified).
* `0x60031078` / `0x60042000` persistence: still catch-all.
* Register storage unmigrated, like every other register in this machine model.

## Evidence

`build-runtime-state/regi2c-2026-10-07/` — baseline grammar traces, ROM
disassembly excerpts, rev A exploratory captures (unqualified), rev B captures
(`fixed/{wifi,ble}-regi2c-revb-0{1,2}`), boot-smoke logs, `evidence.md` with
exact commands and SHA256s.

License: patches touch GPL-2.0-or-later QEMU sources and preserve upstream
headers/copyright.
