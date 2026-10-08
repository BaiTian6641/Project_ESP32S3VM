# ESP32-S3 native I2S prototype

This lane implements the digital I2S0/1 work in I2S-01/02 of
`docs/plans/peripherals-electrical.md`. It is not a full ESP32-S3 support
claim, a hardware qualification, or an ESP32 internal ADC/DAC/LCD/camera model.
The GPL device code remains in the separate QEMU executable; it is not linked
into the Qt application.

## Device and electrical contracts

`copies/esp32s3_i2s.c` and `.h` provide clocked Philips/MSB/PCM-short and TDM
serial shifters, mono/stereo packing, qualified-width boundaries, master/slave
and both shared-clock directions. The source and divider selectors follow the
locked ESP-IDF 6.1 S3 HAL/register headers. Clock inputs come from the common
clock owner; there is no private nominal PLL substitute. External MCLK evolves
from actual resolved input edges, including divided MCLK output and slave
module-clock sampling. A quiet external source holds evolution.

The GPIO owner supplies mux, inversion, output-enable and input-buffer truth.
Every desired output goes through the single native electrical
`esp32s3_electrical_set_matrix_drive` authority. Consumed input is actual
resolved net state, not a GPIO injection, echo, zero-fill provider, or I2S
character stream. Unknown consumed digital input is a dependency pause.
Disabled receivers and nonexistent pads require hardware-semantic handling;
per-pad legacy fallback, ownership dropping and pre-filled valid idle levels
are not acceptable ways to obtain a positive result.

The FIFO has a bounded 256-byte storage profile and demands actual bytes from
the shared GDMA engine. TX/RX rings retain descriptor cursors; RX byte-boundary
segments use the shared continuous `esp_gdma_finish_rx_segment` API rather
than parking/restarting the original list. Source starvation, FIFO overflow,
stop/restart, reset and clock changes evolve through timed state. Documented
hardware slave idle and last-frame replay are classified separately from new
payload, never offered as missing external samples.

S3 `TX_TDM_SKIP_MSK_EN=0` corresponds to an enabled-slot-packed buffer. Setting
it requires a full total-slot DMA stride, with disabled-slot words discarded
from output. Both interpretations have native test cases. The ordinary IDF
fixtures use the packed form and set `skip_mask=false`.

Two reference limitations remain explicit:

* The TRM RX_EOF_NUM prose conflicts with the locked HAL's byte-length
  programming. This model uses the source-pinned HAL byte contract; that
  choice is not independent hardware qualification.
* The published FIFO depth is 64 x 32 bits. The relationship between packed
  storage and width-independent channel occupancy is not proven by the
  extracted data-path figure. Native storage-profile tests do not settle
  silicon occupancy.

## PDM and capabilities

`esp32s3_i2s_pdm.c/.h` implement persistent, allocation-free raw 16-bit packing,
phase/channel selection and documented codec/DAC-style digital line mappings.
Raw physical CLK is WS; the internal BCLK is twice that frequency. Controller
line/converter masks are pinned to the S3 HAL, not inherited from ESP32.

I2S0 PCM2PDM/PDM2PCM conversion remains blocked at genuine enable/update before
FIFO/GDMA consumption. The exact filter recurrences/constants, fixed-point
widths, rounding/saturation, feedback, decimation latency/phase, dither seeds
and reset state are unavailable in the examined primary sources. No generic
sigma-delta or boxcar filter is presented as the silicon converter. I2S1 does
not possess those converters. `pdm-provenance.json` records the locked source,
full vendor TRM identity, Espressif technical issues/tests/examples and errata
research, plus the precise missing properties.

## Explicit powered sample peer

`esp32s3_i2s_peer.c`, `_peer_config.c` and `_peer_service.c` bind the registered
`i2s-sample-peer` to the actual v3 graph. Its canonical component kind is
`device`, its type is `i2s-sample-peer`, quantity `parameters` is empty, and its
required strict configuration lives in `attributes.native_i2s_peer`.

Lowercase roles are `vdd`, `gnd`, `bclk`, `ws`, `din`, `dout`. Power is the
explicit 2.7..3.6 V profile and output branches are 40-ohm push-pull to the
actual rails. DOUT immutable vectors are independent of DIN capture. Master
clock/data publication is atomic; slave changes preserve the current solved
observer frame. Finite source exhaustion and capture overflow are real
reported boundaries, not generated silence or implicit completion.

The public child `/machine/soc/i2s-sample-peers` exposes `status-json`,
`capture-request-json` and `capture-json`. Capture windows return actual DIN
records, at most 1024 per request. Optional scoped transition windows expose
payload-free actual controller/peer activation epochs, FIFO/source identity,
complete emitted word/phase boundaries and explicit retention/loss budgets.
LOAD is not COMPLETE, an attempt count is not a captured DOUT sample, and a
getter does not advance the hardware. Independent reference attribution uses
these actual transitions, not payload-prefix matching or discarded startup
mismatches. All physical startup DIN events remain in the evidence.

`record-directory` produces exclusive actual RX binary streams. Their existing
16-byte header and 32-byte sample records remain unchanged. Audio presentation
is not their source and cannot qualify missing samples.

## Source preparation and identity

`source-map.json` and `prepare.py` describe the additive recipe from official
base commit `40edccac415693c5130f91c01d84176ae6008566` and frozen26 fingerprint
`dbe2a8a5ad4e07a607e76ab751e77ac85fe3bf30974359bd25a69354499f9b32`.
Owner clock/GPIO/radio/ADC/electrical foundations precede the shared SPI/GDMA/
UART hook overlays and the I2S copies plus `0001` machine and `0002` test
wiring. Canonical shared kernels compile once; obsolete ADC REGI2C/kernel
copies are not duplicated.

The archived raw UART input identity is SHA256
`2626ab0cad1955d02bc20cb9a190bc782a388eea0fac452b3524f15b4ab7b9fa`.
Historical normalized-text hashes are not alternative runtime identities.
Explicit owner-approved raw context handling is recorded; source bytes are
not silently normalized. Immutable input snapshots and prepared target
ledgers, not compiled caches or mutable vendor worktrees, establish source
identity.

Historical I2S packs keep the **entire** previously accepted electrical/GPIO
pack coherent. They cannot qualify a newer parent foundation. Original GPIO
inputs must be restored byte-for-byte before a separately authorized additive
correction is appended. Any changed runtime source/header requires its full
compiled dependency closure and a fresh native binary identity.

The reviewed I2S-only source snapshot is
`qemu-i2s-40edccac4156-dbe2a8a5ad4e07a6-runtime-58fc-eb50`. Its prepared receipt
is SHA256 `d80153ea2514f8aa4570ac9ebf7ed5a7b5a2cb09e226cfe56289709e34db29f1`
and its archived source map is
`7c0b5e7c5d0209f5fb0bd87fc00bc15398421ff9d80ca77e38836b4777d6c3d1`.
It preserves 92 of 95 historical targets, changing only the reviewed I2S
controller source/header and 66-case native test source. It is an immutable
historical differential input, not a current aggregate source qualification.

CombinedVerify has prepared the corrected 40-patch/29-copy parent source
`a45abaa18d905226`, recorded in
`build-runtime-state/runtime-a45abaa18d905226-source.json` (SHA256
`fe8a1d314b35bb7304e84d4e88dc46ae6fedf0aa0ebad245b210433ed27915c4`).
It orders GPIO `722b9e31…` after the pad corrections/tests and includes
`prototypes/coreclk/0016-system-capture-reset-entry-cause-gate-S3-reset.patch`,
SHA256 `f1a3c4986080edb56fe679b451427157015761ca1f68b5142eaea9f878521898`.
This is the reset-entry-cause/domain transaction, not the historical
SYSTEM-storage work labelled `0016` in the receipt's evidence boundary.
Main still requires NativeNet's renewed coherent freeze with this exact
ordering and current hashes before any I2S overlay derivation or native
verification. The previous FC61/c6fc receipts do not meet that gate.
Both ordinary fixtures and the seven MCLK/four changed native cases must use
the same fresh I2S binary derived after release; historical `19e9…` and local
historical packs cannot substitute.

## Fixtures and exercised evidence

The normal IDF wire fixture is `tests/firmware/i2s_native`: 124 configurations,
including widths, both controllers/clock roles, sparse TDM, raw PDM, DMA
starvation/restart and capability/frame-limit checks. The external fixture is
one normal IDF ELF with 68 ordinary UART-selected cases; it does not need 68
full firmware builds. The SELECT/CASE/READY/Apply/G lifecycle and public
capture/export contracts are documented under `newpeer/`.

Actual corrected firmware builds succeeded:

| Artifact | SHA256 |
| --- | --- |
| Wire 4 MiB flash, `firmware-fixed-tdm/i2s-native.merged.bin` | `bb78309eed975628ac07bf5c9f2f2d76cb2e88dbbc7c8e39fcb6f87677c3581a` |
| Wire ELF | `fbe7c3b6a4ae45928a5308a6eaa4fadab1627fe8e548ebb5e833f8cd14daa789` |
| Interactive peer 4 MiB flash | `7286ef19e03b606d3614e3d57bb5e532b76f0669220b1b15589cca9d7913d91b` |
| Interactive peer ELF | `083a5944dd170eb4f99646eb37e44f368addb7a628b6a3ffffa1b64a1697f02c` |

Earlier wire `965df…` and peer `2bed…` builds used a mismatched TDM skip flag.
Their bytes/logs remain preserved, but they are withdrawn for full TDM
runtime qualification. Fresh writable flash copies are required for launches;
the original corpus images are never used as writable disks.

Subagent checks exercised the final raw helper with normal and ASan/UBSan
builds and 225288 independent edge comparisons. Fifty-seven synthetic
parser/math/launcher tests passed at the transition all-land gate; affected
canonical graph/launcher checks also passed. These are not native physical
transport or converter/hardware proof.

The actual native binary
`19e9ca676b5a94c5c9bb6e525a065c9b946f9f61d477c322b4876ed6daf65d96`
was linked from an accepted historical pack. Its first native runs exposed
host harness defects (paused virtual timers, non-quiescent graph Apply,
unnamed IRQ observation, premature input-enable and invalid component kind).
All failures are retained. The last completed historical differential suite
receipt had 55 passes and 11 not-passed cases: seven MCLK cases awaited the
changed core, two slave attribution cases exposed a double FIFO pop on the
first falling edge, one raw-slave fixture sampled the wrong physical phase,
and one retention test timed out without a DUT assertion. Changed-scope
execution is verifier-owned. This is not an all-66 pass or current-foundation
qualification.
An actual MCLK model-wake deadline restart/double-schedule defect was repaired
with a phase-preserving effective clock/route key. The verifier rebuilt the
controller, peer service, embedding SoC and test dependency closure and linked
historical runtime binary
`db80596831514581ddccf38aaf196ee98fef58549bf7464751051e0cb500dff3`
from receipt `5365392ee13864e8eac3423b88ccc379dbd53b9e3a0bdc2bdec21183075e6af3`.
`native-runtime-bf4f-compile/result.json` qualifies that build only. Initial
clock launches failed before DUT execution because of the native ROM resource
root; those startup failures do not establish clock correctness. Actual
metrology remains required, not inherited `19e9…` results or shifted timing
oracles. This intermediate build predates the final slave-acquisition and
raw/retention test changes in `runtime-58fc-eb50`.

Ordinary wire boot reached the real ROM POWERON/SPI_FLASH_BOOT banner, then
paused on GPIO UNKNOWN before any I2S case or RX sample. The peer batch retained
13 completed pre-SELECT failures and one partial case; no I2S dataplane was
qualified. The exact enabled pad/IE/mux/pull cause must be resolved with real
circuitry or a reviewed hardware-semantic GPIO correction. Permissive input
fallback is forbidden. Failure evidence is under
`build-runtime-state/i2s-native-2026-10-07` and the verifier's native cache.

## Qualification boundary

| Row | Current evidence boundary |
| --- | --- |
| Standard/TDM digital implementation | Source implemented; substantial historical native cases exercised; full accepted suite still required. |
| Raw PDM | Mathematical helper verified; historical native paths exercised; ordinary peer/ring proof still required. |
| FIFO/GDMA/clock/reset | Actual differential native cases exist; no blanket current-foundation pass. |
| Registered external 68-case streams | Corrected ELF, graph and transition mapper ready; ordinary exact bidirectional samples/cadence not yet qualified. |
| I2S0 PCM/PDM converters | Exact arithmetic/reference prerequisite blocked; genuine enable fails closed. |
| I2S1 converters | Unsupported controller capability. |
| Hardware reference | Unqualified; no new hardware operations performed. |

Only actual verifier receipts may promote these rows. Guest checksums,
ISR-only cadence, source attempts, a linked binary, synthetic tests, and host
playback are insufficient substitutes for physical sample/cadence evidence.
