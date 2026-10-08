# Radio, SIMD and physical reference takeover

Full packages and acceptance are in [radio-simd.md](../plans/radio-simd.md).
Reference tooling is documented in [hardware-reference.md](../hardware-reference.md)
and the prepared operation in [hardware-deployment.md](../hardware-deployment.md).

## Wi-Fi native evidence

Unmodified development-IDF firmware succeeds through NVS/netif/event-loop/Wi-Fi
initialization and mode selection, then stalls in esp_wifi_start during full RF
calibration in both official and recovered QEMU.

Captured ROM PC: **0x40036a46**, caller ram_read_sar2_code.
It polls **0x6000e050 bits 26:24** until 7, following writes:

* 0x6000e060 ← 0x00ff501a
* 0x6000e05c ← 0x0080016a, then 0x0088016a

The public bitfield name was not established. Do not implement unconditional DONE
or intercept esp_wifi_start to produce success. Investigate timed SAR/PWDET,
clock/reset/ownership and calibration behavior with pinned source and authorized
hardware evidence. Trace locations and hashes are retained under ignored fixture
build/runs directories and the reference document.

No native scan/authentication/association/DHCP/socket/security gate has passed.
The recovered virtual-AP/network code is candidate implementation, not proof that
the vendor blob uses the modeled interface correctly.

## Bluetooth native evidence

Official QEMU hits the controller low-power clock assertion
select_src_ret && set_div_ret in btdm_low_power_mode_init, bt.c around line 1757 in
the pinned development source.

The recovered core gets farther, then LoadStorePIFAddrError:
PC **0x40056f60**, EXCVADDR **0x3fc00100**, EXCCAUSE **0x0f**,
around r_rw_rf_init/r_rwip_init/controller task, followed by reboot.

These are distinct failures; do not collapse BLE into the Wi-Fi stall. Normal
controller start, advertising/scanning, GATT, SMP/security, persistence, coexistence
and advanced BLE features remain unqualified. ESP32-S3 is the BLE target; do not
silently substitute an unsupported Classic Bluetooth product capability.

## SIMD evidence and oracle

Ordinary firmware currently emits **84 actual PIE vectors across seven
operation/alias forms**, including 8-bit MAC/add/sub, Q-register move and aliasing
cases. It is a first catalogue, not a full ISA. Assembly initializes and reads
actual Q0–Q7/QACC/ACCX/SAR/SAR_BYTE/FFT/UA/pointer state; scalar expected values do
not fabricate captured hardware state.

Repeated same-image QEMU output agrees. An unsigned 20-bit MAC overflow vector
observed 65014; hardware comparison is needed before deciding whether the emulation
semantics are wrong. Keep raw differing fields/bytes and the original oracle.

Task/interrupt preservation, saturation corners, all remaining opcodes,
loads/stores/alignment/faults, dedicated GPIO, DSP/DL and debugger Q-register
inspection remain later gates. A serial vector report alone is not full chip timing.

## Physical board and preserved data

COM5 read-only ROM/stub discovery identified ESP32-S3 QFN56 revision 0.2, 40 MHz,
USB Serial/JTAG, 16 MiB quad flash and 8 MiB embedded 3.3 V PSRAM.
Secure boot and flash encryption were disabled.

Original full flash:

* build-hardware-host/board-original-16MB.bin
* 16,777,216 bytes
* SHA256 **610b31430392a0d32228d7600c9c377881d5043c103c0c6d535e400a736edbc3**

Backup and deployment identity are private, ignored local artifacts. Do not publish,
upload, include raw backup bytes in prompts/logs or replace the original snapshot.
A Git snapshot does not preserve these ignored files.

Pending user question offered all fixtures, SIMD only or leaving firmware
unchanged. **There is no answer authorizing programming.** Continuing after rate
limits, documentation, subagent permission and COM5 availability are not flash
approval. The latest handoff request also does not authorize flashing.

## Prepared artifacts and exact runner

| Fixture | Merged flash SHA256 |
| --- | --- |
| SIMD | c4263180bd9f6531408707c2ba2d628b98359d82fb012916b2d71a3e20a3caeb |
| Wi-Fi | 73928a983806aacba641aa75cb6897f3b95994d7d48785f3865b9ba7086c21b3 |
| BLE | 435a71cd0daf9f4758f1929eafa032cf9afef3c2232348acb3ec3ad7218396d2 |

Frozen file/ELF/manifest/board metadata is in
build-hardware-host/deployment-manifest.json. All images use an ordinary
15-second startup grace and UART0 plus secondary USB Serial/JTAG console.

PowerShell preparation-only check:

~~~powershell
./build-hardware-host/Scripts/python.exe tools/run-hardware-reference.py --selection all
~~~

Default execution opens no port. The explicit physical form
--execute --selection simd|all --output NEW_DIR is described for a later approved
operator; do not issue it now.

## Recovery invariants already implemented

tools/run-hardware-reference.py uses pinned public esptool **5.4.0** and a single
identified open ROM/stub handle per protected transaction. Structured MAC hash,
chip/revision/crystal/security and capacity checks precede mutation. Stub upload
must retain the same serial handle. Automatic write retries that reopen COM5 are
disabled with WRITE_FLASH_ATTEMPTS=1.

Original flash verification and first write remain quiescent on one handle.
The immutable backup/image bytes are hashed once before opening hardware; esptool
receives those bytes, not reopened file paths. Immediately before write_flash a
mutation checkpoint makes recovery mandatory.

Failed preflight/original verification BEFORE that checkpoint must not restore a
stale backup. A partial write/capture cancellation AFTER it must restore the full
16 MiB snapshot and verify on one identified quiescent handle before resetting
the original firmware. Restoration failure remains an active recovery task.

Fourteen deployment mocks pass, including both cases above, identity reassignment,
changed handles, security, partial write and immutable bytes. No real write has
executed. These safety tests do not prove physical restoration.

## Durable acquisition and integrity

hardware-reference.py creates a sibling <capture>.journal before opening serial,
with frozen manifest, identity metadata and fsynced bounded raw.partial.log.
The journal remains labeled incomplete/unqualified even on normal completion.
Forced kill/cancel/read/fsync failures preserve whatever durable bytes exist.
Final acquisition verifies journal identity/raw/manifest hashes and writes the
normal fresh capture directory. Complete begin/count/ordered-record/end parsing
and artifact identity remain required.

Fourteen mocked acquisition/parser tests pass on Windows/WSL, including an actual
forced-kill subprocess with entirely mocked serial. Current frozen acquisition
tool SHA256:
**f7cfed114acd2a2733783001d80b4206a23eb9f4d37fd8623c7e0df3ee4cdab6**.
Refresh tool provenance when preparing a future approved deployment; fixture bytes
remain unchanged. Parent runner currently captures 45 seconds with a 55-second
subprocess deadline; standalone reference guidance recommends at least 60 seconds.
Choose/record the intended deadline explicitly in a future approval/run review.

## Fine-grained next tasks

| ID | Work | Acceptance/dependency |
| --- | --- | --- |
| H-REF-01 | Freeze capture/deployment tool provenance with fixture hashes | Same byte identity recorded; all host mocks still pass |
| H-SIMD-01 | Obtain explicit selection before physical work | Trusted human authorization retained; no inferred consent |
| H-SIMD-02 | Execute approved capture and verified restoration | Complete 84 records and restoration proof; failure retains recovery obligation |
| H-SIMD-03 | Compare exact same-image state bytes | Every mismatch retained; no altered oracle or discarded saturation case |
| H-SIMD-04 | Expand opcode/state/fault/context catalogue | Full decoded inventory maps to actual vectors and native/hardware gates |
| H-RADIO-01 | Repeat native discovery on stable IDF profiles | New source/blob/config hashes and precise first failure per Wi-Fi/BLE |
| H-RADIO-02 | Model demonstrated clock/power/SAR/RF prerequisites | Native controller start without symbol/API interception or constant DONE |
| H-RADIO-03 | Build deterministic peer/data/security/persistence gates | Separate Wi-Fi/BLE milestones; exact bytes/events and negative paths verified |

Radio work depends on the reviewed clock/IRQ/GDMA/power/crypto/analog prerequisites.
Do not promise full radio fidelity from open-network or host-only HCI behavior.
