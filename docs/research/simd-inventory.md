# H-SIMD-04 — ESP32-S3 PIE (SIMD) opcode/state inventory plan

Status: research inventory. Facts and citations only; no implementation promises.
Scope sources: pinned official QEMU `build-qemu-official-base/target/xtensa/` (9.2.2 lineage),
recovered fork `qemu/target/xtensa/`, local TRM extract
`docs/Esp32-s3_technical_reference_manual_en.md` (v1.2), fixture
`tests/firmware/simd_reference/`, hardware arbitration data
`build-runtime-state/simd-hw-compare-2026-10-07.{md,json}` (2026-10-07).

Arbitration ground truth used below: physical ESP32-S3 rev 0.2 (COM5) vs official
Espressif QEMU 9.2.2, same flash image sha256 `c4263180…`, 84 vector records per
side, `before` states byte-identical, 38/84 `after` states diverge
(`build-runtime-state/simd-hw-compare-2026-10-07.md`, "Result" table).

---

## (a) Complete opcode-class inventory

The entire PIE decoder in the pinned official base is
`build-qemu-official-base/target/xtensa/translate_tie_esp32s3.c` (6197 lines). It defines
`static const XtensaOpcodeOps tie_ops[]` at :4279–6192, exported as
`xtensa_tie_opcodes` (:6194–6197): **258 opcode entries, no duplicate names**.
The esp32s3 core registers it third in `esp32s3_opcode_translators[]` after core and FPU
tables (`core-esp32s3.c:22–28`). Every entry carries `.coprocessor = 0x0`
(e.g. :4466, :5920), so no CPENABLE gate is generated (see §(d)/context).
Line numbers below are `.name` entry lines in that file.

| Class | Opcodes (variants) | QEMU translator file:line (entry) | Fixture-covered? | State touched |
| --- | --- | --- | --- | --- |
| Vector load 128 | `ee.vld.128.ip/.xp` | :4311, :4318 (`translate_vld_128_s3` :737) | Exercised by fixture prologue only (not a test target) | Q[i] |
| Vector load 64 L/H | `ee.vld.l.64.ip/.xp`, `ee.vld.h.64.ip/.xp` | :4282, :4296, :4289, :4303 (`translate_vld_64_s3`) | No | Q[i].u64[lo/hi] |
| Unaligned-support load | `ee.ld.128.usar.ip/.xp` | :4326, :4333 (`translate_ld_usar_128_s3` :810) | No | Q[i], SAR_BYTE (set at :841–842), AR |
| QACC load + widen | `ee.ldqa.[u8/u16/s8/s16].128.ip/.xp` | :4341–:4390 (`translate_ldqa_128_s3`) | No | QACC_L, QACC_H |
| Broadcast load | `ee.vldbc.8/16/32` + `.ip/.xp` forms | :4398–:4455 (`translate_vldbc_s3`) | No | Q[i], AR |
| Halfword broadcast load | `ee.vldhbc.16.incp` | :4463 (`translate_vldhbc_s3`) | No | Q[i], AR |
| Vector store 128/64 | `ee.vst.128.ip/.xp`, `ee.vst.l/h.64.ip/.xp` | :4498, :4505, :4470–:4491 (`translate_vst_128_s3`, `translate_vst_64_s3`) | Exercised by snapshot macro only | memory ← Q[i] |
| F-reg load/store | `ee.ldf/stf.[64/128].[ip/xp]` (8) | :4513–:4562 | No | FR (f0–f3 / fu/fv), AR |
| Zero | `ee.zero.qacc`, `ee.zero.accx`, `ee.zero.q` | :4570, :4577, :4584 (`HELPER(zero_s3)` :418) | No | QACC_L+QACC_H / ACCX / Q[x] |
| WUR user regs | `wur.accx_0/1`, `wur.qacc_l_0..4`, `wur.qacc_h_0..4`, `wur.gpio_out`, `wur.sar_byte`, `wur.fft_bit_width`, `wur.ua_state_0..3` | :4592–:4718 (`HELPER(wur_s3)` :438–…) | Yes — all except gpio_out (fixture prologue; not a test target) | ACCX, ACCQ[0]=QACC_L, ACCQ[1]=QACC_H (:482–483 SAR_BYTE etc.), gpio_out, SAR_BYTE, fft_width, UA_STATE |
| RUR user regs | `rur.…` same 19 registers | :4726–:4852 (`HELPER(rur_s3)` :512–…) | Yes (snapshot macro) | reads same state |
| Q→QACC move | `ee.mov.[u8/s8/u16/s16].qacc` | :4861–:4882 (`HELPER(mov_qacc_s3)`) | No | QACC_L, QACC_H |
| Immediate | `ee.movi.32.a`, `ee.movi.32.q` | :4890, :4897 (`HELPER(movi_a_s3)`, `HELPER(movi_q_s3)` :1325–1334) | No | AR / Q[i].u32[sel] |
| Zip/unzip | `ee.vzip.8/16/32`, `ee.vunzip.8/16/32` | :4905–:4940 (`HELPER(vzip_s3)`/`HELPER(vunzip_s3)` :1354/1398) | No | Q[qs0], Q[qs1] |
| Saturating add (signed) | `ee.vadds.s8/s16/s32` + `.ld.incp` + `.st.incp` | :4950–:5008 (`HELPER(vadds_s3)` :1459) | **s8 base form only** (12 vectors; one alias form); rails arbitrated vs hw | Q[qz]; AR on .incp |
| Saturating sub (signed) | `ee.vsubs.s8/s16/s32` + `.ld.incp` + `.st.incp` | :5017–:5075 (`HELPER(vsubs_s3)` :1516) | **s8 base form only** (12 vectors); rails arbitrated | Q[qz] |
| Vector multiply | `ee.vmul.[s8/s16/u8/u16]` + `.ld.incp` + `.st.incp` | :5083–:5162 (`HELPER(vmul_s3)` :1677) | No | Q[qz], SAR (arg) |
| Complex multiply | `ee.cmul.s16` + `.ld.incp` + `.st.incp` | :5171–:5185 (`HELPER(cmul_s3)` :1750) | No | Q[qz] |
| MAC → ACCX | `ee.vmulas.[s8/u8/s16/u16].accx` + `.ld.ip` + `.ld.xp` + `.ld.ip.qup` + `.ld.xp.qup` (20) | :5194–:5331 (`HELPER(vmulas_accx_s3)` :1839) | No | ACCX, QACC (QUP align via SAR_BYTE :2007), Q[u], AR |
| MAC → QACC | `ee.vmulas.[s8/u8/s16/u16].qacc` + `.ld.ip/.ld.xp/.ldbc.incp/.ldbc.incp.qup/.ld.ip.qup/.ld.xp.qup` (28) | :5339–:5534 (`HELPER(vmulas_qacc_s3)` :1946) | **s8 and u8 base forms only** (24 vectors); both arbitrated vs hw | ACCQ[0]/ACCQ[1] (QACC_L/H), Q[u] on QUP, AR |
| Vector×scalar MAC | `ee.vsmulas.s8/s16.qacc` + `.ld.incp` | :5542–:5564 (`HELPER(vsmulas_s3)` :2075) | No | QACC_L/H |
| QACC shift-read | `ee.srcmb.s8/s16.qacc` | :5573, :5580 (`HELPER(srcmb_qacc_s3)` :2150) | No | Q[qu] ← shifted QACC |
| ReLU | `ee.vrelu.s8/s16`, `ee.vprelu.s8/s16` | :5588–:5610 (`HELPER(vrelu_s3)` :2228, `HELPER(vprelu_s3)` :2274) | No | Q[qz]; AR regs ax/ay |
| Min/max | `ee.vmax.s8/s16/s32`, `ee.vmin.s8/s16/s32` + `.ld.incp` + `.st.incp` (18) | :5619–:5744 (`HELPER(vmax_s3)` :2322, `HELPER(vmin_s3)` :2401) | No | Q[qz] |
| Compare | `ee.vcmp.eq/lt/gt .s8/s16/s32` (9) | :5753–:5811 (`HELPER(vcmp_s3)` :2480) | No | Q[qz] (mask lanes) |
| Bitwise logic | `ee.orq`, `ee.xorq`, `ee.andq`, `ee.notq` | :5820–:5844 (`HELPER(bw_logic_s3)` :2623) | No | Q[qa] |
| Shift/concat | `ee.src.q` (+`.qup/.ld.xp/.ld.ip`), `ee.slci.2q`, `ee.srci.2q`, `ee.slcxxp.2q`, `ee.srcxxp.2q`, `ee.srcq.128.st.incp`, `ee.vsr.32`, `ee.vsl.32` | :5853–:5932 (`HELPER(src_q_s3)` :2846 uses SAR_BYTE :2847; `HELPER(srcq_64_rd_s3)`; `HELPER(vsx32_s3)`) | No | Q[qa], SAR (vsr/vsl), SAR_BYTE (QUP/srcq), AR, memory |
| FFT butterfly | `ee.fft.r2bf.s16[.st.incp]` | :5942, :5950 (`HELPER(r2bf_s3)`, `HELPER(r2bf_st_low/high_s3)`) | No | Q[qa0/qa1], AR, memory |
| FFT complex mul | `ee.fft.cmul.s16.ld.xp`, `ee.fft.cmul.s16.st.xp` | :5958, :5966 (`HELPER(fft_cmul_ld_s3)`, `…_st_low/high_s3`) | No | Q[qz/qx/qy], SAR_BYTE (:3688), AR, memory |
| Bit reverse | `ee.bitrev` | :5975 (`HELPER(bitrev_s3)`; `reverse()` helper) | No | Q[qa], AR; controlled by fft_width |
| FFT real accumulate | `ee.fft.ams.s16.ld.incp`, `.st.incp`, `.ld.incp.uaup`, `.ld.r32.decp`, `ee.fft.vst.r32.decp` | :5984–:6016 (`HELPER(fft_ams_s16*)`) | No | Q*, UA_STATE (uaup), ACCQ temp (`temp`, `temp_asm`), SAR_BYTE, AR, memory |
| GPIO port ops | `ee.wr_mask_gpio_out`, `ee.set_bit_gpio_out`, `ee.clr_bit_gpio_out`, `ee.get_gpio_in` | :6026–:6050 (`HELPER(wr_mask_gpio_out_s3)`) | No | gpio_out (8-bit), AR |
| Q↔AR move | `ld.qr`, `st.qr`, `mv.qr` | :6058, :6066, :6074 (`HELPER(mv_qr_s3)` :974) | **mv.qr only** (12 vectors; all matched hw) | Q[i].u32[sel] / Q[qv]←Q[qx] |
| ACCX load/shift | `ee.ld.accx.ip`, `ee.srs.accx` | :6082, :6090 (`HELPER(ld_accx_s3)`, `HELPER(srs_accx_s3)`) | No | ACCX (64-bit host container for 40-bit), AR, memory |
| QACC segment load/store | `ee.ld/st.qacc_h/l.[h.32/l.128].ip` (8) | :6098–:6148 (`HELPER(ld/st_qacc_x_*_s3)`) | No | QACC_H/L, AR, memory |
| ACCX store | `ee.st.accx.ip` | :6156 (`HELPER(st_accx_s3)` — zero-extends 40-bit to 64) | No | memory ← ACCX |
| UA state load/store | `ee.ld/st.ua_state.ip` | :6164, :6171 | No | UA_STATE (128-bit), AR, memory |
| Indexed Q segment | `ee.ldxq.32`, `ee.stxq.32` | :6179, :6186 (`HELPER(ldstxq_addr_s3)` — offset = `sign_extend(qs.s16[sel8]) * 4 - 4`) | No | Q[qu/qv].u32[sel4], AR |

Cross-checks:
- Local TRM chapter 1 "Processor Instruction Extensions (PIE)" lists the same groups:
  read/write/arithmetic/comparison/shift/FFT/RUR-WUR sections
  (`docs/Esp32-s3_technical_reference_manual_en.md:886–930`, read semantics :1005–1014,
  write semantics :1039–1044, zero/mov :1066–1072, MAC :1152–1173, other :1202–1205).
- Public Espressif reference (esp-dl repo, accessed 2026-10-07):
  https://github.com/espressif/esp-dl/blob/master/tools/agents/skills/esp32s3-pie-simd/references/pie_instruction_set.md
  — same register file (8×QR 128-bit; SAR 6-bit, SAR_BYTE 4-bit, ACCX 40-bit, QACC_H/L
  160-bit, FFT_BIT_WIDTH 4-bit, UA_STATE 128-bit), same classes, and per-instruction
  encodings (e.g. EE.VLD.128.IP, EE.LD.128.USAR.IP operation `SAR_BYTE[3:0] = as[3:0]`).
- Recovered fork `qemu/target/xtensa/translate_tie_esp32s3.c` has 246 entries (vs 258);
  the divergence between the two trees is not inventoried further here (see §(e)).

---

## (b) Saturation / overflow risks, with arbitration vectors

All quotes below are from the **pinned official base**
`build-qemu-official-base/target/xtensa/translate_tie_esp32s3.c`. The recovered fork
`qemu/target/xtensa/translate_tie_esp32s3.c` carries byte-identical expressions at the
same line numbers (verified for :1466–1468, :1476–1477, :1959–1963, :1971–1974, :1995–1998).

### B1. Signed negative-saturation rail off-by-one (hardware-arbitrated WRONG)

`HELPER(vadds_s3)` :1459; s8 branch :1462–1470:

```c
int16_t result = (int16_t)tie->Q[qx].s8[i] + (int16_t)tie->Q[qy].s8[i];
if (result > 0x7f) result = 0x7f;
if (result < -0x7f) result = -0x7f;      // :1468 — rail at -127, must be -128
```

Same pattern: s16 :1476–1477 (`-0x7fff`), s32 :1485–1486 (`-0x7fffffff`);
`HELPER(vsubs_s3)` :1516 — s8 :1524–1525, s16 :1533–1534, s32 :1542–1543;
`HELPER(vmulas_qacc_s3)` :1946 — s8 QACC lanes :1959–1960 and :1962–1963 (`±0x7ffff`),
s16 QACC lanes :1983–1987 (`±0x7fffffffff`);
`HELPER(vmulas_accx_s3)` :1839 — ACCX :1848–1851 (`0x7fffffffff`);
`HELPER(vsmulas_s3)` :2075 — :2089–2090, :2093–2094.

Arbitration (38-vector set, `simd-hw-compare-2026-10-07.md`):
- `ee.vadds.s8` seq 17–23 (7 vectors) and `ee.vadds.s8.alias_q0` seq 29–36 exc. 31 (7),
  `ee.vsubs.s8` seq 51–59 exc. 52, 56 (7): QEMU negative lanes `0x81` (−127) where
  hardware clips to `0x80` (−128) — 52 lanes total, transition `0x81→0x80`.
- `ee.vadds.s16.alias_q1` seq 79–84 exc. 81 (5): QEMU `0x8001` (LE bytes `01 80`) vs
  hardware `0x8000`; per-byte transition `0x01→0x00` (9 lanes; e.g. compare JSON
  `index 78–83`, field `after.q`, bytes 16/18/20/28/30).
- Positive rails agree (`0x7f`/`0x7fff`/`0x7ffff` all matched hardware where exercised).

### B2. Unsigned 20-bit MAC accumulation (`ee.vmulas.u8.qacc`) — the "suspicious unsigned MAC saturation expression"

`HELPER(vmulas_qacc_s3)` :1946, u8 branch :1968–1977:

```c
q_req_l.u20[i] += (uint32_t)tie->Q[qx].u8[i]*(uint32_t)tie->Q[qy].u8[i];
if (q_req_l.u20[i] > 0xfffff) q_req_l.u20[i] = -0xfffff;   // :1972
q_req_h.u20[i] += (uint32_t)tie->Q[qx].u8[i + 8]*(uint32_t)tie->Q[qy].u8[i + 8];
if (q_req_h.u20[i] > 0xfffff) q_req_h.u20[i] = -0xfffff;   // :1974
```

Findings (all facts from source + recorded captures):

1. The clamp assigns **negative** `0xfffff`; stored into `uint32_t u20[]` that becomes
   `0xFFFF0001`, whose low-20-bit packed form is `0xF0001` (983041). If this branch ever
   fires, the result matches neither wrap nor hardware saturation. **No vector in the 84
   exercised it** (see U1 in §(e)).
2. The clamp is *inert* on the captured overflow path: 20-bit lanes are loaded via a
   sign-extending arithmetic shift (`load_qacc`, :171–186: `… << 12; dest = dest >> 12;`),
   so a raw lane ≥ `0x80000` becomes a large `uint32` (e.g. raw `0xFFFF5` → `0xFFFFFFF5`);
   adding `255×255 = 65025` wraps mod 2³² back below `0x100000`, the `> 0xfffff` test is
   false, and the wrapped value is stored.
3. The 65014 vector: lane wraps to `0xFDF6 = 65014`; the QEMU `after.qacc` is the
   repeating 5-byte pattern `f6 fd 60 df 0f` (= two packed 20-bit lanes `0xFDF6`),
   `records.json` seq 2–12; described in `docs/hardware-reference.md:253`
   ("Encoded initial lane 1048565, input bytes 255 and 255, observed result 65014") and
   `docs/handoff/06-radio-simd-hardware.md:52–54`.
   **Hardware arbitrates saturation**: for `ee.vmulas.u8.qacc` seq 2–12 (11 vectors) the
   physical chip returns all-`0xFF` bytes, i.e. every 20-bit lane saturates at
   `0xFFFFF` (compare JSON `index 1`, seq 2, field `after.qacc`: qemu `f6/fd/60/df/0f` …,
   hw `ff` on all 40 bytes).
4. Arbitration vector of record for B2: **any of seq 2–12; canonical = seq 2
   (`op0-case1`)**: input QACC bytes `f5ff5f…` (raw lane `0xFFFF5`), inputs `ff×ff`,
   expected lane `0xFFFFF`, QEMU-observed lane `0xFDF6` (65014).

### B3. Signed 20-bit negative rail in MAC (`ee.vmulas.s8.qacc`) — hardware-arbitrated

Seq 63 (`op5-case2`): QEMU `after.qacc` repeating group `ff ff 17 00 80`:
positive-overflow lanes at `+0x7FFFF` (agrees with hardware) but negative lanes at
`0x80001` = `−0x7FFFF` (the `:1960/:1963` rail). Hardware returns `0x80000` = `−0x80000`
(compare JSON `index 62`, field `after.qacc`, bytes 2/7/12/17/22/27/32/37: qemu `17`,
hw `07`, bytes 3–4 unchanged `00 80`). So the hardware negative rail for 20-bit signed
MAC lanes is `−2¹⁹`, one below QEMU's `−2¹⁹+1` — the same off-by-one class as B1,
consistent with fixing `:1960/:1963` (and `:1984/:1987` for 40-bit) to the true minimum.

### B4. Internal inconsistency: `srcmb` negative rail uses `0x80`

`HELPER(srcmb_qacc_s3)` :2150; the 8-bit shift-out path saturates with two different
conventions in the same loop (:2161–2172): `> 0x7f → 0x7f` but `< -0x7f → 0x80`
(:2169–2171), while vadds/vsubs write `−0x7f` (:1468). One of the two conventions is
wrong relative to silicon; no fixture vector arbitrates it yet (§(c) Group A5).

### B5. Provenance caveat

The arbitrated QEMU side is the released official 9.2.2 binary (commit `40edccac…` per
compare report). The local `build-qemu-official-base` snapshot carries the expressions
quoted above; whether that snapshot is bit-identical to the `40edccac…` build is not
established in this repo (see §(e) U1).

Errata check (requested): the official ESP32-S3 v0.2 errata list contains eight items —
ANALOG-160, CACHE-126, LCD-239, USBOTG-4289, RMT-176, RTC-126, ADC-183, TOUCH-100 —
**none mention PIE/SIMD/instruction extensions**
(https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32s3/_tags/v0-2.html,
accessed 2026-10-07). The observed divergences are therefore not explained by a
published rev 0.2 erratum. QEMU reports chip revision 0 vs hardware 0.2
(compare md, "Interpretation and caveats").

---

## (c) Required expanded fixture plan

Current fixture state (`tests/firmware/simd_reference/`): `main/pie_reference.S`
dispatches exactly 7 forms (`pie_reference.S:96–117`): `ee.vmulas.u8.qacc` (op0),
`ee.vadds.s8` (op1), `ee.vadds.s8 q0,q0,q1` alias (op2), `mv.qr` (op3), `ee.vsubs.s8`
(op4), `ee.vmulas.s8.qacc` (op5), `ee.vadds.s16 q1,q0,q1` alias (op6); 12 deterministic
vectors each (`main/simd_reference.c`: `CASES 12`, `OPERATIONS 7`, seeds from
`0x5a17c3e9`), 84 records (`build/reference-manifest.json`: `expected_records: 84`,
case ids `op0-case0…op6-case11`). Snapshot state per vector: Q0–Q7, QACC_L/H via
RUR/WUR words, SAR, SAR_BYTE, FFT_BIT_WIDTH, ACCX_0/1, UA_STATE_0..3, AR deltas
(`pie_reference.S` snapshot macro :9–72; layout asserted in `simd_reference.c`
`pie_state`).

Coverage gap summary: **251 of 258 translator entries are not test targets**; the 7
covered forms use only base (no-address-update) variants, SAR_BYTE/FFT/UA/gpio state is
always zero, and no fault, alignment, or context case exists.

Required expansions (each item = one fixture case class with its exact expected
observable; "oracle" = byte-compare QEMU vs hardware via the existing capture runner):

- **A. Arbitrated rail corrections (highest priority — golden data already exists)**
  - A1 `vadds/vsubs.s8`: lane pairs `0x7F+0x01` and `0x80+0xFF`. Observable:
    `after.q` lane bytes exactly `7f` / `80`. (QEMU today: `81`.)
  - A2 `vadds/vsubs.s16`: lanes `0x7FFF+1`, `0x8000+(-1)`. Observable: `7fff` / `8000`.
  - A3 `vadds/vsubs.s32`: observable `7fffffff` / `80000000`.
  - A4 `vmulas.u8.qacc` overflow, two sub-cases: (i) seq-2 state (raw lane `0xFFFF5`,
    inputs `ff×ff`) → expected lane `0xFFFFF`; (ii) raw lane `0x80000` + `0xFE01` →
    expected `0xFFFFF` (this sub-case is what forces the `:1972/:1974` clamp branch;
    current source would produce `0xF0001`, the captured binary wraps — oracle decides).
  - A5 `vmulas.s8.qacc` negative rail: seq-63 state → expected lane `0x80000`
    (QEMU today `0x80001`).
  - A6 `srcmb.s8.qacc` shift-out rails (B4): observable Q bytes for in-range,
    `>+0x7f`, `<−0x7f` shifted lanes — hardware oracle pending.
- **B. Untouched opcode classes (decode + one canonical + one edge vector each)**
  observable: full `pie_state` before/after digests + per-lane expected values from the
  TRM description; QEMU-vs-hardware byte compare.
  - B1 `vmul.[s8/s16/u8/u16]` + `.ld.incp`/`.st.incp` (16) — includes SAR-shifted forms.
  - B2 `cmul.s16` (3), `vsmulas.*` (4), `vrelu/vprelu` (4), `vmax/vmin` (18), `vcmp` (9),
    `orq/xorq/andq/notq` (4), `vzip/vunzip` (6), `mov.*.qacc` (4), `movi` (2), `zero` (3).
  - B3 `vmulas.*.accx` (20) with ACCX rails: observable `RUR.ACCX_0/1` after sums beyond
    `±0x7FFFFFFFFF` and `+0xFFFFFFFFFF` — hardware oracle pending (U2).
  - B4 shift/concat family: `src.q`(+qup/ld.xp/ld.ip), `slci/srci/slcxxp/srcxxp.2q`,
    `srcq.128.st.incp`, `vsr/vsl.32` (12) — observable Q and SAR/SAR_BYTE interactions
    (vsr/vsl use SAR per TRM :1651/:1656).
  - B5 loads/stores: `vld/vst 64/128` (10), `vldbc` (9), `vldhbc` (1), `ldqa` (8),
    `ld.128.usar` (2), `ldf/stf` (8), `ld/st.qr`, QACC/ACCX/UA_STATE EE-level
    load/store (12), `ldxq/stxq` (2), `ld/st.accx.ip`, `srs.accx` — observable: memory
    window bytes, destination register/accumulator bytes, AR deltas.
  - B6 FFT family (9): `r2bf` ×2, `fft.cmul` ×2, `bitrev`, `fft.ams` ×4 with nonzero
    FFT_BIT_WIDTH and nonzero SAR_BYTE; observable: full Q/QACC/UA_STATE/temp-dependent
    state digests across a minimal 4-point butterfly — oracle pending (U6).
  - B7 GPIO ops (4): `wr_mask/set_bit/clr_bit/get_gpio_in` — observable: `rur.gpio_out`
    byte and inbound sample; requires QEMU GPIO wiring check (gpio_out is machine-visible
    only if wired; state is 8-bit, `cpu_esp32s3.h:28`).
- **C. Address / alignment semantics** (hardware forces alignment; low bits ignored —
  TRM :624, :856; esp-dl reference "force alignment", so there are **no alignment
  faults** to test, only masking + USAR capture):
  - C1 128-bit access with `as[3:0]≠0`: observable = data read from `{as[31:4],4'b0}`,
    `as` incremented by imm/ad only; compare AR delta and Q bytes.
  - C2 `ee.ld.128.usar.*` SAR_BYTE capture: `as=0x…A` → observable `RUR.SAR_BYTE == 0xA`,
    then `ee.src.q` concat result matches the unaligned window (esp-dl reference USAR
    operation).
  - C3 All address-update encodings (`ip/xp/incp/dec16/ldbc_inc1`, increments +1/+16/
    imm/ad): observable = recorded `ar_deltas` words.
  - C4 QUP align variants with SAR_BYTE = 1..15 (`HELPER(vmulas_qup_s3)` :2005–2016
    byte-rotate): observable = Q[qu] bytes.
  - C5 `ee.ldxq.32` offset corners: `sign_extend(qs.s16[sel8])*4 − 4` at s16 ±max
    (`HELPER(ldstxq_addr_s3)` :4236–4240, translate-side comment :4201).
- **D. Context / fault / reset**
  - D1 Task-switch preservation: two FreeRTOS tasks interleave WUR-loaded MAC chains;
    observable: per-task QACC/ACCX digest equal to single-task golden run (QEMU does no
    automatic PIE save/restore — PIE is not modeled as a gated coprocessor; see below).
  - D2 Interrupt-in-PIE-sequence: ISR performs PIE ops; observable: interrupted task's
    post-ISR digest unchanged.
  - D3 CPENABLE gating: execute `EE.*` with `CPENABLE=0`. QEMU fact: every PIE entry has
    `.coprocessor = 0x0`, and `gen_check_cpenable` only fires for nonzero masks
    (`translate.c:319–327`, `:929`, `:1025–1027`), so QEMU raises no
    Coprocessor0Disabled exception (contrast FPU `.coprocessor = 0x1`,
    `translate.c:6627+`). Observable to arbitrate: hardware exception behavior (U3).
  - D4 Post-reset state: QEMU allocates `env->ext` with `qemu_memalign` and never clears
    it (`hw/xtensa/esp32s3.c:505`; `xtensa_cpu_reset_hold` `target/xtensa/cpu.c:96–136`
    does not touch `ext`), so first-read PIE state is uninitialized host memory.
    Observable to arbitrate: hardware post-reset `RUR.*` values (U4).
  - D5 Snapshot/migration: `vmstate_xtensa_cpu` is `unmigratable = 1`
    (`target/xtensa/cpu.c:215–218`) — no VM snapshot path preserves PIE state. Required
    observable once supported: savevm/restore round-trip digest equality.
  - D6 Undecoded/illegal PIE encodings: observable = illegal-instruction exception
    parity QEMU vs hardware.

---

## (d) Debugger Q-register exposure gap

Regmap already declares all PIE registers for esp32s3
(`build-qemu-official-base/target/xtensa/core-esp32s3/gdb-config.inc.c`):
`gpio_out` :207; `accx_0/1` :242–243; `qacc_h_0..4` :244–248; `qacc_l_0..4` :249–253;
`sar_byte` :254; `fft_bit_width` :255; `ua_state_0..3` :256–259; `q0..q7` 128-bit
:260–267+; plus 40/160-bit `accx`, `qacc_h`, `qacc_l` states :486–491.

The gdbstub does not serve them as PIE state
(`build-qemu-official-base/target/xtensa/gdbstub.c`):

1. **User-reg group** (accx_0/1, qacc_*_0..4, sar_byte, fft_bit_width, ua_state_0..3,
   gpio_out — gdb type UserReg): `xtensa_cpu_gdb_read_register` returns
   `env->uregs[reg->targno & 0xff]` (:96–98) and write stores into the same array
   (:131–133). The TIE implementation never touches `env->uregs` — `HELPER(wur_s3)`
   (:438–…) and `HELPER(rur_s3)` (:512–…) read/write `env->ext` fields directly. GDB
   therefore reads stale zeros and writes into a shadow array the guest never sees.
   This is why the Debug panel shows "unavailable".
2. **q0–q7** (TieRegfile, 128-bit): the TieRegfile case only handles sizes 4/8 (FPU
   regs) — 16-byte reads fall to `default:` → `LOG_UNIMP` + zeroes (:108–119); writes
   return size without effect (:160–170).
3. **accx / qacc_h / qacc_l** (40/160-bit TieState, :486–491 of gdb-config): excluded
   from `num_regs` by `xtensa_count_regs` (:57–64 skips TieState/Mapped/Unmapped), so
   they are unreachable in the g/G packet at any index; the switch has no TieState case
   anyway.

Concrete files to change later (facts, not promises):
- `target/xtensa/gdbstub.c` — extend `xtensa_cpu_gdb_read_register` /
  `xtensa_cpu_gdb_write_register`: (i) TieRegfile size-16 case → `((CPUXtensaEsp32s3State*)env->ext)->Q[i]`
  via the existing `cpu_vec_ptr` accessor (`translate_tie_esp32s3.c:210–214`, would need
  a non-static shared accessor or duplicate mapping); (ii) PIE-aware UserReg handling
  that maps the PIE UR ids to `env->ext` fields, mirroring the `HELPER(wur_s3)` /
  `HELPER(rur_s3)` switch (`translate_tie_esp32s3.c:438–560`) so GDB writes land in real
  state; (iii) decide/serve TieState 40/160-bit entries (pack/unpack against
  `ACCQ[0/1]` 20-byte blobs; note `xtensa_count_regs` interaction).
- No regmap change required — `core-esp32s3/gdb-config.inc.c` is already complete.
- PIE state lives only in `env->ext` accessed through helpers at runtime (not cached in
  TCG globals), so gdb writes take effect without TB invalidation; verify empirically.
- Existing debug aid: `HELPER(dump_all_s3)` printf dump (`translate_tie_esp32s3.c:334–391`).

---

## (e) Explicit UNKNOWN list

- **U1**: Whether the arbitrated QEMU 9.2.2 binary (commit `40edccac…`, compare md) is
  source-identical to the local `build-qemu-official-base` snapshot. The `-0xfffff`
  clamp branch (:1972/:1974) has **no arbitrating vector**; the 983041 (`0xF0001`)
  outcome it predicts was never observed in the 84-vector capture.
- **U2**: Hardware ACCX saturation rails (±`0x7FFFFFFFFF`, `+0xFFFFFFFFFF`) — never
  exercised by the fixture.
- **U3**: Hardware behavior of `EE.*` with `CPENABLE=0` (exception vs silent execute);
  QEMU provably raises none (§(c) D3).
- **U4**: Hardware post-reset values of Q/QACC/ACCX/SAR_BYTE/UA_STATE/FFT_BIT_WIDTH;
  QEMU leaves them uninitialized (§(c) D4).
- **U5**: TRM saturation-rail specification text: the local TRM extract renders the
  per-instruction description tables poorly (`docs/Esp32-s3_technical_reference_manual_en.md`
  table rows are truncated, e.g. :1007 vs :1009 for LDQA u16/s16 widen wording — the
  s16 row says "sign-extend it to 40-bit" from "1-byte", an apparent extraction/source
  typo). A clean TRM 1.6.x read is needed before treating rails as *specified* rather
  than *arbitrated*.
- **U6**: FFT/UA internal state semantics (`temp`, `temp_asm`, `fft_width` ranges,
  UA_STATE bit layout) — no fixture coverage, no hardware arbitration.
- **U7**: Recovered fork `qemu/target/xtensa/` lineage: 246 vs 258 opcode entries and
  5831 vs 6197 lines vs pinned base; which tree matches which released QEMU is not
  established here.
- **U8**: Whether ESP-IDF FreeRTOS saves PIE state on context switch on real silicon
  (RTOS port sources not present in this repo).
- **U9**: `gpio_out` width inconsistency: gdb-config declares 8-bit (:207) matching
  `uint8_t gpio_out` (`cpu_esp32s3.h:28`), but the TRM special-register table
  (:739–744) does not list GPIO_OUT at all.
- **U10**: Saturation behavior inside `HELPER(vmul_s3)` (:1677) and `HELPER(cmul_s3)`
  (:1750) — not audited line-by-line in this pass; `vmul` takes a `sar` argument whose
  semantics (shift-then-saturate?) are unverified.
- **U11**: `ee.vldhbc.16.incp` has no `par[]` and uses `translate_vldhbc_s3` with
  hardcoded behavior (:4463–4467) — halfword-broadcast index/step semantics unverified
  against hardware.
