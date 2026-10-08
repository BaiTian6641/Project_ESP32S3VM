# ADC-01: ESP32-S3 SENS RTC ADC controller, APB_SARADC and oneshot profile

Prototype lane implementing the real ESP32-S3 ADC oneshot measurement path,
re-derived from the ESP32-S3 TRM v1.8 chapter 39 and cross-checked against
the pinned ESP-IDF v6.1.0 (fff9895c) sources (`components/soc/esp32s3/
register/soc/sens_{reg,struct}.h`, `apb_saradc_{reg,struct}.h`,
`components/esp_hal_ana_conv/esp32s3/include/hal/adc_ll.h`,
`adc_oneshot_hal.c`, `components/esp_adc/adc_oneshot.c`,
`components/esp_hw_support/port/esp32s3/sar_periph_ctrl.c`).  The official
base machine has **no** SENS / APB_SARADC model; the recovered-fork
`esp32s3_sens.c` / `esp32s3_apb_saradc.c` stubs (unconditional DONE, fixed
midscale, fake temperature/touch) were used as read-only structural
reference only and their behavior was explicitly NOT copied.

Status: **SOURCE_READY (ADC-01 build-ready)**. Independent compilation,
all 14 real ADC QTests, ordinary DIVIDER/isolated STEP/FLOAT and all three
copied boot profiles passed on the same final immutable binary. The real
FLOAT failure was repaired at its timerlist lifecycle root: one main-loop
pause BH, reset epochs and typed physical-source revisions preserve
no-DONE/data truth and prevent stale pauses. No automatic resume occurs.
`source-map.json` records actual raw hashes and proof receipts. This does
not qualify the complete eAD graph, future buses, ADC-02..05 or hardware.
License: GPL-2.0-or-later.

## Files

| Prototype file | Destination in the QEMU tree |
|---|---|
| `copies/esp32s3_adc_provider.h` | `include/hw/misc/esp32s3_adc_provider.h` |
| `copies/esp32s3_sens.h` | `include/hw/misc/esp32s3_sens.h` |
| `copies/esp32s3_sens.c` | `hw/misc/esp32s3_sens.c` |
| `copies/esp32s3_apb_saradc.h` | `include/hw/misc/esp32s3_apb_saradc.h` |
| `copies/esp32s3_apb_saradc.c` | `hw/misc/esp32s3_apb_saradc.c` |
| `copies/esp32s3_adc_net_provider.c` | `hw/adc/esp32s3_adc_net_provider.c` |
| `copies/esp32s3-adc-test.c` | `tests/qtest/esp32s3-adc-test.c` |

`integration-native-foundations-adc.patch` is the canonical U3 integration
delta. Apply the exact current26 prefix (without coreclk0007), clock0010..0014,
GPIO e1a45cdb, radio0005 d1d4966b, this ADC delta, then eAD; install canonical
copies after all patches. Full raw hashes and target scopes are in
`source-map.json`. The ADC delta includes QAPI object registration, build
wiring, real SENS-to-REGI2C named signals, and retirement of radio's partial
SENS MMIO and obsolete handlers. REGI2C sources are never ADC copies.

eAD supplies the canonical `hw/adc/net-dc.c` and header, compiled exactly
once for both runtime electrical sampling and the ADC test provider. The
real eAD provider must be instantiated and linked to the typed
`sample-provider` property before SENS realization; no string-id/lazy lookup
or zero-source fallback is permitted.

## Device contract

### SENS (`esp32s3.sens`, MMIO 0x60008800, 0x200)

Single owner of the SENS block (the regi2c lane's SENS sub-regions are
retired in favor of this device; per parent coordination there is exactly
one SENS owner in the integrated tree).  Register file per TRM ch.39 with
TRM reset defaults (READERn_CTRL div=2/int_en/gated, MEAS2_CTRL1 waits
7/2/2, SARDATE 0x02101180).  ADC1/ADC2 RTC-controller oneshot path:

- Start: rising edge of `MEASn_START_SAR` (IDF `adc_oneshot_ll_start`
  writes 0 then 1), accepted only with `MEASn_START_FORCE=1` (SW control;
  ULP-owned starts are ignored with a diagnostic) and, for ADC1, with
  `SAR1_DIG_FORCE=0`.  A start while busy cancels and re-arms.  A new start
  clears the previous DONE (no unconditional DONE, ever).
- Gate: `SAR_PERI_CLK_GATE_CONF.SARADC_CLK_EN=0` blocks the FSM (no DONE,
  `diag-gate-blocked`).  `SAR_POWER_XPD_SAR.FORCE_XPD_SAR=2'b10` makes every
  sample unpowered (fail closed).  `SAR_PERI_RESET_CONF.SARADC_RESET` resets
  the reader FSMs and results (configuration retained).
- Timing: `RTCADC_SARCLK = RTC_FAST_CLK / SARn_CLK_DIV` (div honored; the
  TRM 5 MHz precision limit is diagnosed, never coerced; div=0 is a guest
  error treated as 1).  Phases XPD → acquisition aperture (4 × sample_cycle
  sar clocks; REGI2C ADC_SAR1_SAMPLE_CYCLE reset default 2) → 12-cycle SAR
  approximation → RSTB → STANDBY (+ `SAR2_WAIT_ARB_CYCLE` for ADC2).
  XPD/RSTB/STANDBY waits: SAR1 from APB_SARADC `FSM_WAIT` (the SENS block
  has no SAR1 wait fields — documented profile), SAR2 from its native
  `SENS_SAR_MEAS2_CTRL1` fields.  Reset profile 8/8/255 at div 2 = 291
  cycles = 29.1 µs to DONE at the RTC_FAST reset value 20 MHz (XTAL/2).
- Acquisition: the pad voltage is resolved ONLY at the aperture instant
  through the sample provider (below).  Floating / unpowered /
  digitally-owned / unknown samples FAIL CLOSED: FSM back to idle, **no
  DONE, no consumable data** (stale data remains), wiring diagnostic +
  `diag-*` counters; `strict-invalid-sample=true` stops the VM instead.
  Channel selection: `SARn_EN_PAD` (valid with `EN_PAD_FORCE`), lowest set
  bit wins (multiple pads diagnosed), zero pads measures channel 0 with its
  configured attenuation (the behavior IDF self-calibration relies on).
  Per-channel attenuation from `SAR_ATTEN1/2`; `SARn_DATA_INV` inverts the
  12-bit result field; result in `MEASn_DATA_SAR` [11:0] with the ADC2 TRM
  arbiter flags [15:14] (2'b00 valid, 2'b01 not-started, 2'b10 interrupted).
- Live FSM state is exposed (`SAR_SLAVE_ADDR1.MEAS_STATUS`,
  `SAR_READERn_STATUS`, `SAR_MEAS2_CTRL1.CNTL_STATE`); done events
  propagate to APB_SARADC INT_RAW (ADC1/ADC2_DONE) gated by `SARn_INT_EN`.
- ADC2 arbiter (TRM 39.3.8): fair/fixed arbitration grants the RTC
  controller when no PWDET contender exists; `GRANT_FORCE`+`WIFI_FORCE`
  (and `SAR2_RTC_FORCE` clear) deny it — the conversion completes carrying
  the 2'b01 flag and `diag-arbiter-denied`.  Scripted PWDET interruption
  (2'b10) is the ADC-05 extension point; `SENS_SAR_MEAS2_MUX.SAR2_RTC_FORCE`
  masks the arbiter.  The named QOM output `sar2-armed`
  (EN_PAD_FORCE && !RTC_FORCE) feeds the regi2c lane's PWDET sequencer.
- Unsupported surfaces never fabricate results: temperature sensor
  (`SAR_TSENS_CTRL` — READY/OUT never set, dump-out edge published on the
  named output `tsens-dump-out`), touch (scan/done status stays 0), ULP /
  COCPU (interrupt mapper RAW/ENA/ST/CLR + W1TS/W1TC implemented; no
  sources).

### APB_SARADC (`esp32s3.apb_saradc`, MMIO 0x60040000, 0x400)

Full register file with TRM reset defaults; real interrupt mapper
(`INT_RAW` W1S, `INT_ENA`, `INT_ST`, `INT_CLR` W1C, level IRQ →
`ETS_APB_ADC_INTR_SOURCE` 65); `APB_SARADCn_DATA_STATUS` mirrors of the
latest RTC results; `FSM_WAIT` and `ARB_CTRL` consumed by the SENS device;
`DATE` (RO, 0x02101180).  The DIG controller (timer/continuous/DMA, ADC-03)
and threshold monitors / IIR filters (ADC-04) are stored + diagnosed
extension points: they never fabricate conversions, DONE events or data.

### Sample provider (`esp32s3-adc-sample-provider` interface)

The ADC never invents a voltage: `include/hw/adc/esp32s3_adc_provider.h`
defines the QOM interface whose single method resolves
`(unit 0/1, channel 0..9)` → `{voltage_v, validity, sample_ns}` at the
caller-requested aperture instant.  The caller stamps `out->sample_ns`
once before invoking the provider; the provider MUST solve at exactly that
requested instant and MUST echo the stamp back unchanged.  Restamping from
a later clock read is a contract violation (it masks running-clock drift:
under a running non-icount virtual clock, two adjacent `qemu_clock_get_ns`
reads are NOT equal, so any implementation that re-reads the clock and
compares for equality rejects genuine acquisitions — the fixed qtest clock
hides this, and qtest-only PASS is not proof of the timestamp path).
A genuinely stale request (older than the provider's last solved instant)
is rejected `UNKNOWN`; monotonic solver state is never forced backwards.
Validity: `VALID` (known, powered, analog-owned),
`FLOATING`, `UNPOWERED`, `DIGITAL_OWNED`, `UNKNOWN`.  Ownership/validity
classification belongs to the provider implementer (net-solver layer at
parent integration; the qemu-gpio lane's `*_get_drive_snapshot()` register
truth is the recommended DIGITAL_OWNED source; digital `FUN_IE` is
explicitly NOT gating — ADC pads disable digital input by design).
The `sample-provider` class link is typed and set before realization.
Physical source changes use the provider's mutable electrical properties,
never runtime ADC link re-pointing. An absent provider reports UNKNOWN and
fails closed; integrated eAD must prelink its real provider.

Channel map (IDF v6.1.0 `adc_periph.c`): ADC1 ch0–9 = GPIO1–10, ADC2 ch0–9
= GPIO11–20 (GPIO19/20 double as USB D-/D+ — board-use note only).

### Ideal quantization profile (documented; NOT a silicon calibration claim)

`code12 = round(4095 · clamp(v, 0, VDD_A) / (1.1 V · 10^(atten/20)))`,
atten ∈ {0, 2.5, 6, 12} dB, VDD_A = 3.3 V; zero offset/gain error, zero
nonlinearity, noise disabled (exact repeat; seeded noise and calibrated
curves are ADC-02+).  Full-scale: 1.100 / 1.468 / 2.196 / 4.379 V; at
12 dB the VDD_A clamp bounds the code at 3085 (datasheet "limited by VDD_A"
behavior).  Over-range inputs clamp with a diagnostic.

### REGI2C (authoritative radio series, 0x6000E000)

The radio1..5 stack owns REGI2C storage, the getter, 7-bit slave bounds,
configuration readback, fail-closed measurement paths and timed BBPLL.
ADC never copies or shadows those implementations. Its final delta removes
the old SENS force-pair/TCTRL MMIO handlers and registers named signal
consumers driven by the single SENS owner.

SENS uses `esp32s3_regi2c_get_slave_reg(DeviceState *, uint8_t, uint8_t,
uint8_t *, bool *)` through its typed REGI2C link. A false return is invalid
and fails closed; `written=false` means unknown reset configuration and
keeps the external-provider route. Guest-written slave `0x69`, register `7`,
bits `5`/`7` route ADC1/ADC2 to actual internal ground before asking the
external provider. No GPIO1 tie or self-calibration test-provider workaround
is used. Measurement slave `0x6a`, FIFO and TX-DC remain fail closed.

The ordinary IDF oneshot driver's REGI2C transactions (every
`sar_periph_ctrl_adc_oneshot_power_acquire` /
`adc_hal_calibration_init` / `adc_set_hw_calibration_code`: slave 0x69
DREF 0x2[6:4]/0x5[6:4], DTEST 0x7[1:0], ENT_TSENS 0x7[2], ENCAL_REF 0x7[4],
INITIAL_CODE 0x0/0x1/0x3/0x4) run through the genuine ROM engine-word
path (0x6000E000/0x6000E004, BIT24 start / BIT25 busy self-clear) and are
bounded by the modeled I2C wire time. Configuration readback belongs to the
radio model. ADC-02 still requires calibrated transfer/eFuse consistency;
ordinary configuration reads/writes do not qualify calibrated voltage.

### Test-only provider (`hw/adc/esp32s3_adc_net_provider.c`, `adc-dc-provider`)

User-creatable QOM device (no MMIO, no guest hooks) implementing the
provider interface on top of the canonical GPL net-dc kernel: every
sample solves an ACTUAL explicit resistor/rail circuit (rail → Rtop → tap →
Rbot → reference; `float-net` disconnects the tap into an unreferenced
island) and reports the solved tap voltage with its validity.  Properties:
`unit`, `channel`, `rail-mv`, `rail-mv-2`, `source-switch-ns`,
`r-top-ohm`, `r-bot-ohm`, `float-net`, `powered`, `digital-owned`, `next`
(chain providers for multiple channels). Static wiring (`unit`, `channel`,
resistances, `next`) is set at user-creatable `-object` creation before
machine initialization. Physical state is runtime-settable through provider
QOM properties; the SENS class link is not a runtime alias.

## Tests

`tests/qtest/esp32s3-adc-test` (xtensa-softmmu, 14 cases):
reset defaults; no-provider fail-closed; divider 3.3 V/10k/10k → 1.65 V @
6 dB = 3079 with APB data-status mirror + INT_RAW/ENA/ST/W1C/W1S; DONE
staleness (persists until next start); source step before/after the
1600 ns acquisition aperture (post-change 3079 vs pre-change 0); grounded
rail = exact 0; clipped 1.65 V @ 0 dB = 4095; per-channel attenuation
(ch3 0 dB 0.9 V = 3350 vs ch2 6 dB = 3079); start-while-busy cancel/rearm
(DONE exactly one conversion after the second start); RTC-side reset; SARADC
clock gate; analog power gate (external and actual REGI2C internal-ground
routes, including powered repair); ULP/dig ownership diagnostics; ADC2 oneshot
(valid 3350, flag 2'b00) + arbiter deny (flag 2'b01, INT_RAW bit30); open
tap → FLOATING and digital-owned → fail closed with counters.

`tests/firmware/adc_oneshot/` — ordinary IDF 6.1 fixture
(`adc_oneshot_new_unit` / `adc_oneshot_config_channel` / `adc_oneshot_read`
only; no simulator hooks): ADC1 ch2 @6 dB, ADC1 ch3 @0 dB, ADC2 ch0 @0 dB,
3 rounds, printing observed raw values.  Run recipe and observed UART
evidence: `build-runtime-state/adc-2026-10-07/`.  The physical inputs are
`adc-dc-provider` circuits (3.3 V/10k/10k and 1.8 V/10k/10k) linked before
SENS realization; the printed values are whatever the normal driver observes
through the real SENS / APB_SARADC / REGI2C / eFuse paths.

## Explicit boundaries (not modelled here)

- DIG controller continuous mode, timer, pattern table, DMA/FIFO (ADC-03) —
  stored + diagnosed, no fabricated conversions.
- IIR filters and threshold monitors (ADC-04) — thresholds stored, no
  monitor IRQs.
- Calibration eFuse/curve consistency (ADC-02) — the ordinary driver's
  eFuse read + REGI2C initial-code writes run for real, but the ideal
  quantization profile ignores initial codes; calibrated transfer/eFuse
  consistency is not qualified by the configuration-storage getter.
- ADC2/PWDET arbitration with scripted PWDET requests, ADC-183 rev profile
  (DIG ADC2 continuous inoperative on rev 0.0–0.2), radio interactions
  (ADC-05) — extension points only; oneshot ADC2 via the RTC controller and
  the arbiter is the modeled surface.
- Temperature sensor, touch, ULP/COCPU measurements — fail closed by
  contract (no DONE/READY/data), with named outputs for the regi2c lane.
- SENS clock gate only covers the RTC-domain ADC; SYSTEM.PERIP_CLK_EN0
  APB_SARADC_CLK_EN access logging is left to the clock lane.

## Evidence

`build-runtime-state/adc-2026-10-07/` — qtest output, IDF fixture build/run
UART trace, hashes, and command history.

Independent evidence: `build-runtime-state/adc-independent-final-20261007/`.
The preserved pre-safe-pause binary is
`ba6f884b8364adcdb9211cd24e4bc8c9a9ec6006c52d91d8dddba01756809294`,
source-delta identity
`c772291af57584e04020d0d3056e87371491a8ba09586b0641522c03e336bed7`.
`immutable-proof-receipt-final.json` records source bytes, symbols, binary,
QTest and ROM package. `parent-prefix-equality-receipt.json` records 56
unaffected parent-prefix matches, six intentional ADC overlays, zero
mismatches. Original source/application and missing-ROM failures remain
preserved; none is converted into an inherited PASS.

Ordinary same-binary DIVIDER, isolated STEP/FLOAT and three copied boot
profiles have a separate native evidence package. Their final assertions
and mutation-boundary evidence must be read from that receipt, not inferred
from the QTests or from historical lane outputs.

The strict FLOAT failure retains actual QMP-unresponsive status/quit
timeouts and SIGKILL evidence; no VM-paused or repair/restart PASS is
claimed from it. Its root correction defers `vm_stop` outside every ADC
timer/IRQ evaluation. The typed `source_generation(provider, unit, channel)`
method is allocation/solve-free. Real physical changes invalidate pending
unit/channel pause records, while reset and finalization cancel them using
the device epoch. The failed aperture remains no-DONE with stale data.

Final proof: `epoch-final/immutable-proof-receipt.json`, binary
`11967a87cd9a3930a51290c78b55751f92a883dab4e94e6ab503535da9d93a46`,
source-delta identity
`de2d317b36079d5b0231b8661d13b4cff0711c42f56eaf79c4b43b2a5f167590`.
The final native evidence root is
`epoch-final/native/run-vyo9ewao/`; its manifest SHA256 is
`1483e5f5aabdc3e19bc2e6bee160015d7dc5cfb139f248ac977a63ac6e880c45`.

- DIVIDER: ordinary SDK reads 3079/3350/3350 in all three rounds.
- STEP: the fourth actual `adc_oneshot_read` invocation at PC0x4200d6bc
  follows the first complete divider round. QMP changes CH2 rail3300→0
  and reads it back before conversion; later rounds are 0/3350/3350,
  with ADC2CH0 unchanged3350.
- FLOAT: the first ordinary read boundary precedes physical tap disconnect.
  The VM actually pauses and QMP remains responsive; invalid/floating/unknown
  counters are 1/1/0, no DONE/data is invented, and APB mirrors/INT_RAW remain
  unchanged. Physical repair alone leaves the VM paused and the failed
  aperture unchanged. Explicit reset plus continue restores three complete
  divider rounds and clears diagnostics.
- BootSmoke: IDF6.1, IDF5.5.5 and Arduino3.3.12 copied profiles each pass
  all three unmodified boot checks on that same binary.

All original firmware/boot corpus hashes remain unchanged. The ordinary
firmware ELF/image identities are recorded in the source-map. The debugger
wait transport drains UART concurrently rather than blocking firmware behind
chardev backpressure; neither firmware nor deadlines were changed. External
providers model only the three real divider channels, never an extra GPIO1
ground tie or self-calibration workaround.

Canonical raw prefix metadata now uses the original full clock0010/11/12
filenames and raw hashes, with prefix fingerprint
`486cb408a31afae28b1a93896efd0a2909de13300f6fc050c696f10269fa2a92`.
CombinedVerify's
`clock-10-12-raw-metadata-classification/20261007T115213Z/canonical-prefix-equivalence-publication.json`
proves all 62 applied source-file bytes/hashes and all 12 copies equal the
earlier A427 prefix. Only patch mail envelopes differ; no ADC rebuild or
new runtime qualification is claimed. The original11967/A427 proof and
receipt remain immutable history, including the preserved
`ADC-01-source-map-a427-history.json`. Active packaging metadata is in
`ADC-01-canonical-prefix-foundation-receipt.json`.

