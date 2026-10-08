# Requirements and repository state

## User decisions and authorized scope

Build a closed product similar to Wokwi around **Espressif's official QEMU fork**.
Boot ordinary Arduino and ESP-IDF firmware through modeled hardware. Implement
I2S, SPI, I2C, RMT, GPIO, UART, LCD and camera behavior, internal chip state,
Wi-Fi/Bluetooth, SIMD, external devices and an IBM Carbon circuit UI.

The next phase specifically requires Light/Dark/System appearance, easier
interaction, electrical routing, **analog voltages and ADC**, and broad recent
ESP-IDF compatibility. The user explicitly authorized subagents, detailed planning,
independent plan review and web research. Old GPT-generated documents are historical
input, not proof. Prefer pinned primary sources, TRM/errata, target headers and
actual firmware/reference evidence.

The user offered a physical board on COM5. This authorized preparation/read-only
investigation; the separately asked temporary flash question remains unanswered.
The latest request is documentation for another agent to take over.

## Exact checkout

| Item | Value |
| --- | --- |
| Outer Windows workspace | C:/Users/weyst/Documents/ChatGPT/ESP32S3VM |
| Actual repository | C:/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM |
| WSL repository | /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM |
| Branch | codex/simulator-foundation |
| HEAD | 8cb306fe4709d6eb3a6e5f274f24349befecb21a |
| Remote | git@github.com:BaiTian6641/Project_ESP32S3VM.git |
| Work state | Extensive tracked changes and untracked source/documents; no new commit/push |
| Instructions | No AGENTS.md was found in the earlier repository scan; check again on takeover |

Preserve the user's existing changes. Do not reset, clean, reclone over this tree,
or treat untracked files as disposable. The original gitlinks had no .gitmodules;
that omission was repaired. The historical qemu and esp-idf gitlinks are not the
selected modern baseline.

## Three QEMU lanes

| Lane | Source/build location | Role |
| --- | --- | --- |
| Recovered owner core | qemu; independent build-qemu-source/build-wsl | Commit 33c2bdd17104b3baeea3b788bafd27b2f921c13f; existing cached I2C/stderr bridge and candidate models |
| Pristine official | build-qemu-official-base/build-wsl | Espressif release esp-develop-9.2.2-20260417; ordinary baseline boot |
| New maintained extension | /home/polar/.cache/esp32s3vm/qemu-extension-40edccac4156/build-hostbus | Official base plus tracked, opt-in hostbus/debugger/restore prototype |

Official source commit is **40edccac415693c5130f91c01d84176ae6008566**.
The annotated tag object **6cdce334bfa5541a5e46c860a9b40a4fe5a13aae** is a different
Git object, not the checkout commit. runtime-lock.json records this distinction.

New extension source/patches must stay under tracked qemu-extensions; the Linux
cache is a build destination, not the sole copy of implementation.
build-qemu-official-source is a preserved failed borrowed/partial-clone attempt;
do not confuse it with the successful build-qemu-official-base lane.

## Firmware profiles

| Profile | Source identity and state |
| --- | --- |
| IDF 6.1 | fff9895c82d744c7237be8847347bdd1b07c6643; GCC 15.2.0; actual official-QEMU boot passed |
| IDF 5.5.5 | b774170ff46c393eeb5e495ea37936038d3f4f4f; source/tool environment preparation completed; firmware boot not yet tested |
| Arduino 3.3.12 | 94afccf35fb1e401facddbcf9e13bcf7c76a31d8, underlying IDF 5.5.5; build/native acceptance pending |
| IDF 6.2 development | 25fe69f946311abdaf9ad56591f25fedbc20ac98; installed EIM/GCC 16.1; boot and cached native I2C fixture passed |

Profile locks do not imply blanket peripheral compatibility. Keep source/config,
toolchain, library, ELF and flash hashes with each actual test.

## Delivered foundation

Carbon semantic themes and persisted preference; shared firmware controls;
acknowledged runtime/capability state; bounded QMP commands; unsupported debugger
actions gated; model-backed v3 documents, stable terminal/net IDs, undo, atomic Save,
migration and resource rebasing; dedicated bounded host transport; ordinary boot and
one cached native I2C path; a bounded GPL DC kernel; SIMD/radio reference fixtures;
and a reviewed, mock-tested COM5 deployment/recovery workflow.

The latest BoardWorkspace rewrite connects editing to the v3 model and separates
Save from Apply. Its shared required regression now passes 9/9 with no skips;
visual/product limitations remain explicit. The QEMU hostbus enabled launch is
currently failing. DC is standalone.

## Remaining product gaps

No qualified electrical pin-to-peripheral routing, RC transients or native ADC.
No generic native timed UART1/2, I2C/SPI device path, streaming RMT/I2S/LCD/camera
qualification. Clock/reset/IRQ/GDMA fidelity needs foundational corrections.
Wi-Fi and BLE native initialization currently fail. SIMD hardware comparison and
the full instruction/state inventory remain outstanding. External quad 8MiB PSRAM
with the CPU write-back cache and the GDMA MMU view is qualified on the CORE-04
binary `6dfa7350` (12/12 native memory qtests, 16/16 frozen GDMA cases, 4/4
ordinary IDF 6.1/5.5.5 SDK leaves over three boots each, 3/3 boot controls and the
SPI1 fail-before/pass-after control; evidence in
`build-runtime-state/memory-core04-6dfa7350-2026-10-08`). Other PSRAM densities,
OPI/DDR, security/OTA/soak matrices and the camera-capable consumer tree remain
open.

Do not turn a driver initialization, register readback, GUI wire, Python device
smoke or QOM dependency probe into a full support label.
