# Prepared hardware reference deployment — 2026-10-07

Status: the user approved all three fixtures, then approved re-baselining to the
current flash and explicitly waived recovery of the previous firmware. The physical
run completed with all three acquisitions returning zero. The tools remain
preparation-only unless invoked with `--execute`.

## Identified board and preserved image

COM5 ROM queries identify ESP32-S3 QFN56 revision 0.2, 40 MHz crystal, USB
Serial/JTAG, 16 MiB quad flash and 8 MiB embedded 3.3 V PSRAM. Secure boot and
flash encryption are disabled. No eFuse writes were performed.

The initial complete backup, `build-hardware-host/board-original-16MB.bin`
(16,777,216 bytes, SHA256
`610b31430392a0d32228d7600c9c377881d5043c103c0c6d535e400a736edbc3`),
is preserved unchanged. The first deployment attempt refused to program: current
flash differed in three 4 KiB sectors, two in NVS and one in SPIFFS. Firmware,
bootloader and partition table bytes matched. No write or restoration occurred
in that attempt.

After the user's re-baseline decision, a read-only acquisition created
`build-hardware-host/flash-current-2026-10-07.bin`, also 16,777,216 bytes, SHA256
`ff42a83ec8bf8c7fb8949a0a83001789948473ddc603831c2fe58bbfcf0c3d38`.
The deployment manifest now uses this immutable image for pre-write verification
and optional recovery. Neither backup was overwritten. These ignored local images
may contain private firmware/settings; never upload them automatically.

The SIMD image is `tests/firmware/simd_reference/build/simd_reference.merged.bin`,
SHA256 `c4263180bd9f6531408707c2ba2d628b98359d82fb012916b2d71a3e20a3caeb`.
It executes 84 real PIE vectors from seven operation/alias forms, starts after an
ordinary 15-second application delay to allow console acquisition, emits actual
register/accumulator state, then idles. No GPIO sensor/actuator experiment is part
of this fixture. UART0 primary plus USB Serial/JTAG secondary is configured.

Wi-Fi and BLE images are separate `radio_init/build/wifi` and `build/ble` products.
Wi-Fi SHA256: `73928a983806aacba641aa75cb6897f3b95994d7d48785f3865b9ba7086c21b3`.
BLE SHA256: `435a71cd0daf9f4758f1929eafa032cf9afef3c2232348acb3ec3ad7218396d2`.
Both have the same ordinary 15-second console-acquisition grace.
They exercise stock ESP-IDF initialization/start/stop/deinitialization and report
stage results. They do not request a configured external Wi-Fi network or change
eFuses. Native QEMU Wi-Fi and BLE failures are documented in
[hardware-reference.md](hardware-reference.md); these are hardware comparison inputs,
not support claims.

## Exact action sequence after explicit approval

1. Recheck each image, ELF/manifest and backup hash. Keep the full backup unchanged.
2. Temporarily flash the approved 4 MiB merged fixture to offset `0x0` on COM5
   with local pinned esptool 5.4.0; normal erase/program affects that image region.
   Preserve its flash mode/frequency/size headers. Do not use `--force`, mass erase,
   eFuse burning, permanent security modes or calibration bypasses.
3. Reset and capture the configured console with explicit port, firmware manifest,
   controlled DTR/RTS and a new output directory. The SIMD startup grace gives time
   to open USB. Capture integrity requires begin/count/ordered records/end and
   matching artifact identity; partial output or reboot is a failure, not a pass.
4. Compare complete SIMD bytes with the same image in official QEMU. Retain all
   differences and raw state; do not discard mismatches or redefine the oracle.
   For radio fixtures, compare actual native stage outcomes and reset reasons.
5. By default, a `finally` recovery path restores the complete frozen 16 MiB image,
   verifies it and resets to the recovered firmware. A failed restoration remains
   an active recovery requirement. `--no-restore` is an explicit operator waiver:
   it leaves the last programmed fixture, records `restoration: waived`, and never
   suppresses write/capture exceptions or cancellation.

The prepared `tools/run-hardware-reference.py` defaults to a dry run. Its hash/
path/security/board gates and recovery controller are implemented; seventeen mock tests
cover default recovery, explicit waiver, pre-mutation failures, partially failed
write, cancellation, restoration error, board
reassignment, security and changed serial handles. The pinned public esptool API
retains one identified ROM/stub connection for original verification and the first
write, and another for restoration plus verification. No application reset occurs
between those protected operations; identity is rechecked before each mutation.
Automatic esptool write retries that reopen COM5 are disabled. The exact image and
backup bytes are read once, hashed and held immutably before opening the port;
programming and restoration do not reopen their file paths. Recovery becomes
mandatory immediately before a programming call unless explicitly waived, so a
rejected original-flash verification cannot restore an obsolete backup. Interrupted
programming triggers identified restoration and verification by default; under a
waiver the exception remains a failure and the known flash state is recorded.
A real run uses `--execute --selection simd|all --output NEW_DIR`
only after explicit human approval. It rechecks board identity/security and verifies
the original flash still matches the backup before any write.

The physical write/reset/capture/restore workflow will execute only
within the user's approved selection. Identifying the board or permitting a serial
read does not authorize replacing its firmware.

## Observed physical run

Executed with the pinned Windows Python/esptool 5.4.0 environment:

```powershell
./build-hardware-host/Scripts/python.exe tools/run-hardware-reference.py `
  --execute --selection all --no-restore `
  --output build-hardware-host/run-2026-10-07-rebaselined
```

The first-write verification passed against the re-baselined flash while the board
was quiescent. SIMD, Wi-Fi and BLE were programmed in that order; each 45-second
capture completed within its 55-second subprocess deadline. The board was left
with the BLE fixture, as authorized. Restoration was **waived, not verified**.

Results and private acquisition artifacts:
`build-hardware-host/run-2026-10-07-rebaselined/result.json` and
`{simd,wifi,ble}/capture/`. SIMD yielded all 84 vectors plus begin/end records;
both radio fixtures completed their ordinary initialization/deinitialization
stages on hardware. This does not establish QEMU radio support. Same-image SIMD
comparison and the verified translator corrections are described in
[the SIMD inventory](research/simd-inventory.md) and
`qemu-extensions/prototypes/simd-pie/README.md`.

