# SAR2 / PWDET calibration register semantics (0x6000_e000 analog-I2C-master block)

Date: 2026-10-08. Scope: read-only research consolidating what is publicly
documented vs. ROM-decoded about the registers touched by the ROM
`ram_read_sar2_code` / `rom_pkdet_vol_start` flow that stalls
`esp_wifi_start` full RF calibration at PC `0x40036a46`.

Prior art: `docs/research/radio-prerequisites.md` (§1),
`build-runtime-state/regi2c-2026-10-07/baseline/trace-analysis.md` (ROM
transaction grammar, trace decode), `docs/handoff/06-radio-simd-hardware.md`.

Sources used:

- **TRM**: ESP32-S3 Technical Reference Manual, Version 1.8 (local PDF
  `/home/polar/.cache/esp32s3vm/trm/s3-trm.pdf`, pages cited below are PDF
  page numbers as printed in the footer). Chapter 39 = "On-Chip Sensors and
  Analog Signal Processing"; Chapter 2 = "ULP Coprocessor".
- **IDF**: local esp-idf 5.5.5 checkout
  `/home/polar/.cache/esp32s3vm/idf/idf-5.5.5-b774170ff46c393eeb5e495ea37936038d3f4f4f/source`
  (cited as `IDF:` with file:line; same files exist on GitHub under
  espressif/esp-idf, path `components/...`, so file:line applies to the
  GitHub file of that commit too).
- **ROM decode**: `build-runtime-state/regi2c-2026-10-07/baseline/trace-analysis.md`
  (cited as `[REGI2C-BASE]` §n), based on esp32s3_rev0 ROM (esp-rom-elfs
  20241011) disassembly + MMIO trace of the official QEMU run.

## 1. Which peripheral owns 0x6000_e000 (and therefore 0x6000_e050/5c/60)

Not the SENS (SAR ADC) block. On ESP32-S3 the documented bases are
(TRM Ch.2 §2.5.2.12 note, p. 321; IDF
`components/soc/esp32s3/register/soc/reg_base.h:14-17`):

- `DR_REG_RTCCNTL_BASE` = 0x6000_8000
- `DR_REG_RTCIO_BASE`   = 0x6000_8400
- `DR_REG_SENS_BASE`    = 0x6000_8800
- `DR_REG_RTC_I2C_BASE` = 0x6000_8c00  ← the *legacy RTC I2C master* is here
  (TRM Ch.2 registers 2.14-2.16, pp. 343; IDF C3 equivalent layout in
  `components/soc/esp32c3/register/soc/rtc_i2c_reg.h:14-662`).

The 0x6000_e000 region is the **second, ROM/analog-only I2C master block**
that Espressif calls the REGI2C / `I2C_MST` block in the (small) public
fragments that mention it:

- IDF `components/soc/esp32s3/include/soc/regi2c_defs.h:12`:
  `#define I2C_MST_ANA_CONF0_REG  0x6000E040`; `:17` `ANA_CONFIG_REG
  0x6000E044`; `:25` `ANA_CONFIG2_REG 0x6000E048`. So the block 0x6000_e0xx
  is explicitly an `I2C_MST` (analog I2C master) register block in public
  IDF headers — with only offsets 0x40/0x44/0x48 named.
- IDF `components/hal/esp32s3/include/hal/regi2c_ctrl_ll.h:37-41`
  (`regi2c_ctrl_ll_i2c_sar_periph_enable`) performs the block's ownership
  gating via those registers (see §4).
- The ESP32-S3 TRM v1.8 contains **no chapter or register entry for any
  0x6000_e0xx address** (full-PDF text search for `6000_e`, `I2C_ANA`,
  `ANA_MST`, `PWDET`-register names returned only SENS/RTCCNTL/APB_SARADC
  hits). The PWDET-relevant registers 0x6000_e050/0x5c/0x60/0x80-0x9c are
  therefore **undocumented publicly**; everything about them below comes
  from ROM decode, marked `[UNVERIFIED]` where not corroborated.

The documented successor of this block on later chips is **I2C_ANA_MST**
(ESP32-C6/H2/C5/H4/P4: IDF `components/soc/esp32c6/register/soc/i2c_ana_mst_reg.h:16-27`).
Its engine-word grammar is identical to the ROM-decoded S3 engine words:
`I2C_ANA_MST_I2C0_CTRL_REG` (+0x0) / `I2C_ANA_MST_I2C1_CTRL_REG` (+0x4),
`I2C_ANA_MST_I2C0_BUSY` = BIT(25) (`i2c_ana_mst_reg.h:19`), control payload
bits [24:0], plus byte-wide status fields at [31:24] of the CONF registers
(`i2c_ana_mst_reg.h:47-49, 61-63`). This is the strongest public corroboration
of the block's nature. The S3 TRM instead documents the SAR/PWDET *data path*
in the SENS and APB_SARADC blocks (§2.2, §5).

## 2. Register map of the three observed addresses

All three are offsets inside the 0x6000_e000 `I2C_MST` block. Bitfield names
are ROM-derived unless marked with an IDF/TRM citation.

### 2.1 0x6000_e050 — PWDET/SAR2 sequencer status+control

| Bits | Identity | Evidence |
| --- | --- | --- |
| [31:27] | unused / unknown | `[UNVERIFIED]` |
| **[26:24]** | **SAR2/PWDET conversion sequencer state; 3-bit FSM/status. Idle/done encoding = `7`; observed exit condition of `rom_pkdet_vol_start` is `(reg >> 24) & 7 == 7`.** | [REGI2C-BASE] §2.3 (`extui a8, a8, 24, 3; bnei a8, 7`) — poll at PC `0x40036a41`, stall PC `0x40036a46` is the `extui` itself. Public bitfield name: **none exists** — not in IDF S3 headers, not in TRM v1.8. `[UNVERIFIED]`: exact intermediate state encoding (working hypothesis from prior work: busy walks 1..6 during a timed conversion, 7 = idle; cf. C6 byte-wide `I2C_ANA_MST_I2C0_STATUS` default 7 pattern and C3 `RTC_I2C_STATUS` semantics, both only analogies). |
| [23:2] | unknown | `[UNVERIFIED]` |
| [1] | re-trigger/soft-restart of the SAR2 measurement; ROM clears then sets it between the two polls | [REGI2C-BASE] §2.3 (`rom_pkdet_vol_start` step 5); `rom_en_pwdet` clears BIT1 (`0x50 &= ~BIT1`, [REGI2C-BASE] §2.3). Bit name public: none. `[UNVERIFIED]` |
| [0] | unknown | `[UNVERIFIED]` |

The same register is also RMW'd by `rom_i2cmst_reg_init` (step
`0x50 = (*0x50 & ~0x1c) | 0x8`, [REGI2C-BASE] §2.2) — i.e. bits [4:2] are a
configuration written 0b100 during master bring-up. `[UNVERIFIED]` semantics.

### 2.2 0x6000_e05c — PWDET SAR2 burst configuration / start

ROM writes (hardware-readback values in parentheses, from the MMIO trace):

| Bits | Identity | Evidence |
| --- | --- | --- |
| [31:24] | unused; upper half masked out by ROM (`& 0xffff0000` keeps 0xaaaa pre-load from `rom_i2cmst_reg_init`) | [REGI2C-BASE] §2.2-2.3 |
| [23:20] | BIT23 = **PWDET burst start** (rising edge arms/starts the SAR2 measurement); ROM `0x5c \|= 0x00800000` → hardware word `0x0080016a` | [REGI2C-BASE] §2.3; observed write `0x0080016a` (task statement, `docs/handoff/06-radio-simd-hardware.md:14-19`) |
| [19] | second start/qualify bit; ROM sets it right after BIT23 → hardware word `0x0088016a`; cleared together with BIT23 at the end of the flow | [REGI2C-BASE] §2.3; observed write `0x0088016a` |
| [18:16] | unknown | `[UNVERIFIED]` |
| [15:8] | **analog-register address byte** (ROM pre-loads `0x01` for the PWDET conversion) | [REGI2C-BASE] §2.2-2.3 (`0x5c = (.. & 0xffff0000) | 0x016a` = slave 0x6a, reg 0x01). Corroborated by the I2C_ANA_MST grammar (reg byte [15:8], `i2c_ana_mst_reg.h` C6). |
| [7:0] | **analog slave address byte** (`0x6a` = the SAR2/PWDET-special slave; the general SAR ADC slave `0x69` matches IDF `I2C_SAR_ADC` — see `regi2c_saradc.h` usage in `components/esp_hw_support/regi2c_ctrl.c:130-135`) | [REGI2C-BASE] §2.3-2.4 |

Public register name for 0x6000_e05c: **none** (`[UNVERIFIED]` field names;
byte-slice positions are corroborated by the C6 I2C_ANA_MST word grammar).

### 2.3 0x6000_e060 — analog-master / PWDET SAR2 config word

| Bits | Identity | Evidence |
| --- | --- | --- |
| [31:16] | config bytes; ROM `rom_i2cmst_reg_init` writes `0xff38`-class bytes (`=(&0xff0000ff)|0x00ff3800`); pwdet path RMWs bits [4:3] set, [5] cleared; final hardware value `0x00FF501A` | [REGI2C-BASE] §2.2-2.3; observed write `0x00ff501a` |
| [15:8] | unknown | `[UNVERIFIED]` |
| [7:0] | unknown; RMW'd (`|= 0x18`, `&= ~0x20`) by `rom_pwdet_sar2_init` before the 0x5c start | [REGI2C-BASE] §2.3 |

Related block registers decoded from the same trace (context; not in the
task's address list): engine words `0x6000_e000/0x004` (func [27:26], BUSY
BIT25, write BIT24, data [23:16], reg [15:8], slave [7:0]; identical to C6
`I2C_ANA_MST_I2Cn_CTRL` grammar, `i2c_ana_mst_reg.h:16-42`); master-select
`0x44`/`0x48`; SCL divider `[7:0]` of 0x58 (`[UNVERIFIED]` name); and the
**PWDET sample FIFO `0x6000_e080..0x6000_e09c`** — 8 words, each a 13-bit
code (`extui ..., 0, 13`), read by `rom_read_sar_dout` (0x40036aa4) and
consumed by `rom_read_sar2_code` (0x40036ac8), which sums word 0 only and
returns `sum >> 2` ([REGI2C-BASE] §2.3). No APB_SARADC (0x6004_0000) access
occurs anywhere in the flow ([REGI2C-BASE] §3.4) — the SAR2 PWDET data path
is entirely inside this block.

## 3. What "bits [26:24] = 7" encodes (completion semantics)

Putting the pieces together — each cited, the synthesis itself marked
`[UNVERIFIED]` where it rests on decode rather than documentation:

1. `rom_read_sar2_code` performs 4 iterations of
   (`rom_pkdet_vol_start` → `rom_read_sar_dout`), i.e. 4 PWDET measurement
   bursts ([REGI2C-BASE] §2.3). Each burst is armed by 0x5c BIT23+BIT19 and
   is complete only when the block's sequencer state 0x50[26:24] returns to
   `7`; then BIT1 is toggled and the poll repeats once more before BIT23/19
   are cleared.
2. Public documentation of what the sequencer measures: TRM Ch. 39 §39.3.1
   (p. 1465-1466) — ESP32-S3 has an internal **"Power/Peak Detect Controller
   (PWDET controller), designed to monitor RF power … only for RF internal
   use"**, one of the two controllers (with the RTC ADC2 controller) that
   can own SAR ADC2 via the **ADC2 arbiter** (§39.3.8, pp. 1474-1475).
   PWDET conversions carry 2-bit validity flags in the top bits of the
   sample: `2'b10` interrupted, `2'b01` not started, `2'b00` valid
   (§39.3.8 p. 1475) — i.e. the arbiter can preempt a PWDET conversion, and
   a real FSM *must* be modeled, not a constant.
3. Therefore 0x50[26:24] is best read as the **PWDET↔SAR2 measurement
   sequencer state inside the analog-I2C-master block** (request → arbiter
   grant → RTCADC_SARCLK conversion → FIFO load → idle), with `7` the
   idle/done value. The "busy walks 1..6" intermediate encoding is
   `[UNVERIFIED]`; the only hard facts are: value 7 exits the loop, and the
   loop is entered immediately after the 0x5c BIT23/19 start bits, and
   re-entered after the BIT1 toggle.

There is **no public name for this field**; a public IDF/TRM field-name
claim of "done" for 0x6000_e050 would be fabricated. (Contrast the
*documented* SENS ADC2 done bit, `SENS_MEAS2_DONE_SAR` bit 16 of
0x6000_8830, IDF `sens_reg.h:364-369`, TRM Register 39.15 p. 1490 — a
different register that the PWDET path does *not* use, because PWDET never
goes through the RTC ADC2 controller's software interface.)

## 4. Prerequisite clock/reset/ownership chain before the poll can exit

Ordered chain observed in ROM + documented in IDF/TRM (each step gates the
next; skipping any is a plausible reason for a real FSM to never reach 7):

1. **RTC-periph clock + deasserted reset for SARADC and the analog I2C**
   — `SENS_SAR_PERI_CLK_GATE_CONF_REG` 0x6000_8904: `SENS_SARADC_CLK_EN`
   BIT30, `SENS_RTC_I2C_CLK_EN` BIT27 (IDF
   `components/soc/esp32s3/register/soc/sens_reg.h:1407-1431`; TRM
   Register 39.37, p. 1499); `SENS_SAR_PERI_RESET_CONF_REG` 0x6000_8908:
   `SENS_SARADC_RESET` BIT30, `SENS_RTC_I2C_RESET` BIT27 (IDF
   `sens_reg.h:1433-1439`; TRM Register 39.38, p. 1499). The MMIO trace
   shows writes to 0x60008904/0x60008908 earlier in boot
   ([REGI2C-BASE] §3.5 — previously filed as "unrelated ADC1/tsens-class";
   they are in fact this gate/reset step).
2. **SAR ADC clock enable + power-up** — `SENS_SAR_POWER_XPD_SAR_REG`
   0x6000_883c: `SENS_SARCLK_EN` BIT31, `SENS_FORCE_XPD_SAR` [30:29]
   (2 = force power up) (IDF `sens_reg.h:399-411`; TRM Register 39.18,
   p. 1491). Clock source for the RTC ADC2/PWDET side is **RTC_FAST_CLK**,
   divided to `RTCADC_SARCLK` by `RTC_SAR_DIV` ≥ 2, ≤ 5 MHz (TRM §39.3.3
   p. 1467); the **arbiter runs on APB_CLK** and must not be starved
   (§39.3.8 note, p. 1475).
3. **Analog-I2C-master bus ownership for the SAR slave** — IDF
   `components/hal/esp32s3/include/hal/regi2c_ctrl_ll.h:37-41`
   (`regi2c_ctrl_ll_i2c_sar_periph_enable`): clear `I2C_SAR_M` BIT18 in
   `ANA_CONFIG_REG` 0x6000_e044, set `ANA_SAR_CFG2_M` BIT16 in
   `ANA_CONFIG2_REG` 0x6000_e048 (macros at
   `components/soc/esp32s3/include/soc/regi2c_defs.h:17-28`). The ROM
   equivalent is `rom_i2c_paral_set_read` 0x400355d8 writing
   `~(mask_a|mask_b)` to 0x6000_e044 ([REGI2C-BASE] §2.1). Master engine
   selection also RMWs 0x6000_e048 (`rom_i2c_paral_set_mst0`).
4. **ADC2 arbiter must grant PWDET (or the RTC force must be released)** —
   `SENS_SAR2_RTC_FORCE` BIT31 of `SENS_SAR_MEAS2_MUX_REG` 0x6000_8834 (IDF
   `sens_reg.h:377-389`, which also names `SENS_SAR2_PWDET_CCT` [30:28] on
   the same register; TRM Register 39.16 p. 1490: "In sleep, force to use
   RTC to control ADC", and §39.3.8 p. 1475: setting it masks the arbiter
   and all signals except the RTC controllers). The ROM
   `rom_pwdet_sar2_init` (0x40036470) clears BIT31 and writes 0x40000000
   into 0x6000_8834 — i.e. sets `SENS_SAR2_PWDET_CCT[30:28] = 0b100`
   ([REGI2C-BASE] §2.3). On the digital side the arbiter can be forced via
   `APB_SARADC_ADC_ARB_WIFI_FORCE`/`_RTC_FORCE`/`_GRANT_FORCE` in
   `APB_SARADC_APB_ADC_ARB_CTRL_REG` (TRM Register 39.63, p. 1509), but the
   PWDET flow never touches APB_SARADC ([REGI2C-BASE] §3.4) — PWDET access
   is granted by the arbiter autonomously (default fixed priority gives
   PWDET priority 2 vs RTC 1, TRM Register 39.63).
5. **PWDET burst armed** — the 0x6000_e05c BIT23+BIT19 rising edge
   (§2.2 above) after `rom_pwdet_sar2_init`'s config writes (0x6000_e060
   RMW, `0x6000_8830` `SENS_SAR2_EN_PAD_FORCE` set + pad bitmap cleared,
   IDF `sens_reg.h:339-375`, TRM Register 39.15 p. 1490).
6. Only then does 0x50[26:24] walk to 7 and the FIFO 0x6000_e080..0x9c
   hold the 8×13-bit PWDET samples.

Also relevant as errata context: the S3 **DIG ADC2 controller is broken and
deleted from the TRM** (§39.3.1 note, p. 1466, pointing to the ESP32-S3
Series SoC Errata) — one more reason all ADC2/PWDET traffic runs through the
RTC/PWDET/arbiter path and the undocumented analog-master block rather than
the digital ADC2 controller.

## 5. Model implications (for the QEMU S3 device model)

The stall is not a missing "DONE" constant; it is a missing **analog I2C
master + PWDET sequencer model**. Concretely, the model must:

- Implement a 0x6000_e000 block (analog I2C master, `I2C_MST`) that *stores*
  0x00-0x9c so ROM read-modify-writes evolve exactly as on silicon
  (hardware readback sequence 0x016a → 0x0080016a → 0x0088016a depends on
  stored state; the official-tree catch-all discards writes, which is the
  immediate hang mechanism — [REGI2C-BASE] §3.1-3.3).
- Model the two engine words (0x00/0x04) as the C6-`I2C_ANA_MST`-grammar
  devices: BIT24/func writes start a transaction, BIT25 busy self-clears
  after a wire time (APB 80 MHz / SCL divider 100 × 37 bits ≈ 46.25 µs),
  read data latches into [23:16].
- Model 0x50[26:24] as a **timed sequencer state machine**: idle 7; on the
  0x5c BIT23(+BIT19) rising edge (which itself should be gated on the SENS
  prerequisite state: 0x8904 clock-enable bits, 0x883c XPD/clk, 0x8834
  arbiter/PWDET_CCT config), spend a bounded virtual-time PWDET conversion
  (8 samples × 13 bits on the modeled RTCADC_SARCLK) walking the state
  bits, then return to 7 and fill the 0x80..0x9c FIFO with non-degenerate
  deterministic codes; the 0x50 BIT1 toggle re-runs one burst. Exit value 7
  must be *produced* by the sequencer, never hardwired, so a wrong
  prerequisite ordering (missing clock gate, arbiter forced to RTC, reset
  asserted) legitimately hangs, exactly like silicon.
- Keep SENS 0x6000_8830/0x8834 (and the 0x8904/0x8908 gate/reset words)
  as stored registers participating in that gating, and leave all
  non-evidenced offsets on log-unimplemented/catch-all as before
  ([REGI2C-BASE] §4 contract).

The 0x6000_e040 `I2C_MST_ANA_CONF0` BIT24 (`I2C_MST_BBPLL_CAL_DONE`) is the
*other*, already-modeled completion bit in this block; the PWDET field at
0x50 is deliberately **not** that bit and must not reuse its shortcut.

## 6. Unresolved / [UNVERIFIED] ledger

- Official name and full bitfield map of 0x6000_e050, 0x6000_e05c,
  0x6000_e060, 0x6000_e058, 0x6000_e080-0x9c (no IDF header, no TRM v1.8
  entry; C6/H2 `i2c_ana_mst_reg.h` is an analogy, not the same chip).
- Meaning of 0x50 bit 0, bits [4:2] config (ROM writes 0b100), bits [23:2].
- Intermediate state encoding of 0x50[26:24] (only "7 = done/idle" and the
  entry/exit points are proven).
- True silicon 13-bit PWDET sample values (deterministic mid-scale codes
  are a model choice, not hardware evidence).
- Whether the two "engines" (0x00/0x04) both participate in the PWDET
  burst or only engine 0 (ROM writes both in parallel for general REGI2C
  traffic).
