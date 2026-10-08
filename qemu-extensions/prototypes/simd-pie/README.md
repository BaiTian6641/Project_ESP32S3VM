# qemu-extensions/prototypes/simd-pie — ESP32-S3 PIE saturation fixes (prototype)

Hardware-arbitrated corrections to the pinned official QEMU ESP32-S3 PIE (SIMD)
translator, `target/xtensa/translate_tie_esp32s3.c` at base commit
`40edccac415693c5130f91c01d84176ae6008566` (Espressif QEMU 9.2.2 lineage).
Ground truth: physical ESP32-S3 rev 0.2 records in
`build-hardware-host/run-2026-10-07-rebaselined/simd/capture/records.json`
(sha256 `e89f3056…`) vs the released official binary run
`tests/firmware/simd_reference/build/runs/grace-official-01`, both with flash
image sha256 `c4263180bd9f6531408707c2ba2d628b98359d82fb012916b2d71a3e20a3caeb`.
License: GPL-2.0-or-later; upstream headers in the patched file are preserved.

## Defects fixed (each patch = one arbitrated defect)

1. `qemu-simd-pie-01-vadds-vsubs-negative-rails.patch` (B1) — `HELPER(vadds_s3)`/
   `HELPER(vsubs_s3)` clamped negative overflow at `-0x7f`/`-0x7fff`/`-0x7fffffff`
   instead of the true signed minimum. Hardware clips to `-0x80`/`-0x8000`/
   `-0x80000000` (52 lanes `0x81→0x80`, 9 lanes `0x01→0x00` in the arbitrated
   set; seqs 17–23, 29–36, 51–59, 79–84 exc. 31/52/56/81). Positive rails
   already match hardware and are untouched. The s32 literal is spelled
   `-0x80000000LL` because unsuffixed `0x80000000` is `unsigned int` in C.
2. `qemu-simd-pie-02-vmulas-u8-qacc-saturate.patch` (B2) — `ee.vmulas.u8.qacc`
   accumulated into a sign-extended 20-bit lane container, so preloaded lanes
   ≥ `0x80000` wrapped (seq 2 lane `0xFFFF5` + `255×255` → observed `0xFDF6`).
   Hardware returns all-`0xFF` packed bytes, i.e. every overflowing 20-bit lane
   saturates at `0xFFFFF` (seqs 2–12). The lane is now reduced modulo 2²⁰ before
   accumulating and clamped at `0xfffff`. The old inert clamp branch
   (`= -0xfffff`, storing packed `0xF0001` = 983041, matching neither wrap nor
   silicon) was **unarbitrated** — no vector in the 84-vector catalogue ever
   reached it; this patch defines it as hardware-saturate semantics so the
   branch and the arbitrated path agree. Reasoning is in the patch commit body.
3. `qemu-simd-pie-03-vmulas-s8-qacc-negative-rail.patch` (B3) —
   `ee.vmulas.s8.qacc` negative 20-bit rail `-0x7ffff` → `-0x80000` (seq 63:
   hw `0x80000` vs QEMU `0x80001`); positive rail `0x7ffff` unchanged.

Deliberately **not** changed: `srcmb` (B4, unarbitrated), `vmulas.*.accx`,
`vsmulas`, the `vmul_u16` 40-bit branch, `load_qacc` itself, debugger files,
and every opcode not covered by the arbitration set.

## Setup (worker-owned WSL checkout; do not reuse the hostbus checkout)

```bash
git -c core.autocrlf=false clone --no-hardlinks --no-checkout \
  build-qemu-official-base /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156
git -C /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156 \
  checkout --detach 40edccac415693c5130f91c01d84176ae6008566
git -C /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156 switch -c codex/simd-pie
```

## Apply (in order)

```bash
cd /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156
for p in qemu-simd-pie-01-vadds-vsubs-negative-rails.patch \
         qemu-simd-pie-02-vmulas-u8-qacc-saturate.patch \
         qemu-simd-pie-03-vmulas-s8-qacc-negative-rail.patch ; do
  git apply "$REPO/qemu-extensions/prototypes/simd-pie/$p"
  git add target/xtensa/translate_tie_esp32s3.c
  git commit -F <(awk '/^---$/{exit} {print}' "$REPO/qemu-extensions/prototypes/simd-pie/$p")
done
```

(The exact scripted run, including per-patch `git apply --check`, is recorded in
`build-runtime-state/simd-fix-2026-10-07/scripts/apply-patches.sh`; commits
`82d1eb3`, `d576f0a`, `095488e` on branch `codex/simd-pie`.)

## Build

```bash
cd /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156
mkdir -p build-simd && cd build-simd
bash ../configure --target-list=xtensa-softmmu --enable-gcrypt --enable-slirp \
    --disable-docs --disable-werror --disable-user --disable-tools \
    --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
    --disable-opengl --disable-capstone
ninja -j "${BUILD_JOBS:-4}" qemu-system-xtensa
```

## Test — fixture capture + hardware comparison

No QEMU-side helper is needed: `tools/hardware-reference.py --qemu` launches
QEMU with the merged flash and parses the framed fixture records (identical
invocation shape to the recorded grace-official-01 metadata).

```bash
python3 tools/hardware-reference.py --qemu \
  --qemu-bin /home/polar/.cache/esp32s3vm/qemu-simd-40edccac4156/build-simd/qemu-system-xtensa \
  --firmware tests/firmware/simd_reference/build/simd_reference.merged.bin \
  --manifest tests/firmware/simd_reference/build/reference-manifest.json \
  --output build-runtime-state/simd-fix-2026-10-07/captures/<new-dir> --seconds 60

python3 qemu-extensions/prototypes/simd-pie/compare-captures.py \
  --hw build-hardware-host/run-2026-10-07-rebaselined/simd/capture/records.json \
  --qemu <capture>/capture/records.json \
  --baseline tests/firmware/simd_reference/build/runs/grace-official-01/capture/records.json \
  --output <new-report>.json --markdown <new-report>.md
```

### Capture integrity and comparison roles

Each `records.json` input must remain beside its original `raw.log`,
`metadata.json` and `manifest.json`. The comparator reuses the acquisition
parser: a complete successful batch, unchanged raw/manifest hashes, matching
record content and identical compiled fixture/firmware identity are required.
Loose vector lists, missing ends, duplicate records and unsuccessful/changed
captures cannot qualify merely because their surviving values match.

Comparison report schema 2 labels lane values `reference` and `candidate`;
the top-level roles are hardware and QEMU. Inputs, vector seed and captured
before/after state are compared. Baseline regressions are only formerly
matching vectors that became mismatches, not pre-existing differences.

The three regressions in `tests/simd_capture_compare_test.py` reproduced six
failures before this correction and now pass; the existing 14 acquisition
tests remain green. Recomparison of the preserved combined-v3 capture still
matches hardware **84/84**, with no capture/firmware artifacts rewritten.
Evidence: `build-runtime-state/simd-comparer-review-2026-10-07/20261007T054817Z/`.


Expected result: **84/84 vectors byte-exact vs hardware** (`before` and `after`
fields), zero regressions vs the pre-fix records. The comparator exits nonzero
on any mismatch and refuses to overwrite existing report files.

## Evidence

`build-runtime-state/simd-fix-2026-10-07/`: before/after comparison JSON+MD,
captures (`captures/baseline-01`, `captures/fixed-01`), build/capture logs,
binary SHA256 records, and the exact scripts under `scripts/`.

## Known boundary

A pass proves only what the 84-vector `simd_reference` fixture exercises (7
instruction forms, base encodings). Unarbitrated saturation corners elsewhere
in the translator (srcmb rails, accx MACs, 40-bit signed rails, u16 MAC) are
unchanged and remain pending hardware oracles. Boot-level sanity is covered by
the `gui_esp32s3_boot_smoke_test` IDF 6.1 run against this binary (see
`build-runtime-state/simd-fix-2026-10-07/boot-smoke-*`).
