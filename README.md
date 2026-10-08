# ESP32S3VM

A local ESP32-S3 circuit simulator built around Espressif's maintained QEMU fork.
The product goal is normal Arduino/ESP-IDF firmware, configurable external devices,
full peripheral behavior, radio simulation, and a Carbon-style circuit studio.

**Current verified delivery is the boot/runtime and circuit-editor foundation.**
It is not yet a complete replacement for hardware or Wokwi. See the
[current evidence and remaining work](docs/current-status.md); older generated
documents are historical proposals rather than acceptance evidence.

The next phase is specified in the [researched development plan](docs/development-plan.md),
with detailed [UX](docs/plans/ux.md), [electrical/peripheral](docs/plans/peripherals-electrical.md)
and [radio/SIMD](docs/plans/radio-simd.md) workstreams. It includes analog voltages
and ADC, recent ESP-IDF qualification, and COM5 hardware-reference preparation.
The [review log](docs/plan-review.md) records corrections and validation boundaries.
Check its task traceability with `python3 tools/verify-plan.py --self-test`.
Latest implementation and continuation state, including unresolved test/review
findings, is recorded in [the checkpoint](docs/implementation-checkpoint.md).

For another agent taking over, start with the
[fine-grained handoff packet](docs/handoff/README.md). It includes exact WSL2
operations, source/build lanes, ownership, contracts, 32 immediate tasks, evidence,
hardware boundaries and ready-to-use worker prompts.

## Start in WSL2

Use Ubuntu WSL2 with Qt6, CMake, Ninja, Python, and the locked ESP-IDF profile:

```bash
sudo apt-get install qt6-base-dev cmake ninja-build g++
cd /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM
bash tools/build-wsl.sh
bash tools/prepare-idf-wsl.sh idf-6.1
source "$HOME/.cache/esp32s3vm/idf/idf-6.1-fff9895c82d744c7237be8847347bdd1b07c6643/activate.sh"
bash tools/build-boot-fixture.sh
bash tools/run-wsl.sh
```

WSLg displays the native studio on Windows. `run-wsl.sh` discovers an official
Espressif QEMU binary installed by ESP-IDF/EIM; override it with
`ESP32S3_QEMU_BIN=/absolute/path/qemu-system-xtensa`. Firmware supplied via the
`--firmware` option uses a 4 MB flash profile; the Control panel supports other sizes.

Select the native image path printed by `build-boot-fixture.sh` in the studio, or
pass that path to `run-wsl.sh --firmware`. The SDK's **full source and submodules**,
release-selected toolchain/Python environment, and new build output live on native
Linux disk, not `/mnt/c`. `ESP32S3_IDF_BUILD_ROOT` names the profile's build root;
boot/sensor fixture inputs are content-addressed snapshots with a source manifest.
The helper records ELF/image/runtime identities and runs the **real** QEMU
controller test. Existing repository fixture builds remain historical, unchanged.

Use `prepare-idf-wsl.sh idf-5.5.5` and its generated activation for that explicit
locked compatibility profile. `ESP32S3_IDF_ENV` may select a prepared activation
script; it must match the requested lock and native-disk requirements. There is
no automatic installed-master fallback. See the
[SDK runbook](docs/handoff/02-wsl2-runbook.md#esp-idf-activation-and-fixture-builds)
for cache overrides, normal peripheral build commands, and evidence boundaries.
Without `ESP32S3_BOOT_FIRMWARE`, CTest explicitly skips that integration test;
the launcher and circuit tests still run.

The Circuit tab supports component creation/removal, dragging, GPIO assignment,
shared I2C/SPI signal editing, and saving/reloading circuit JSON. The existing
Peripherals tab provides display, sensor, UART and GPIO control panels. Ordinary
firmware needs a merged image: bootloader at `0x0`, partition table at `0x8000`,
and application at its partition offset. An application `.bin` alone is rejected.
ELF loading is a debugging path and does not prove ROM/flash boot compatibility.

## QEMU source and extensions

The original repository recorded `qemu` and `esp-idf` gitlinks but omitted
`.gitmodules`. This is repaired. The QEMU gitlink points to the owner's custom
fork at `33c2bdd17104b3baeea3b788bafd27b2f921c13f`, which contains extra device models
and a legacy GUI bus bridge. It is preserved separately from the official
Espressif runtime baseline.

```bash
git submodule update --init qemu
sudo apt-get install libglib2.0-dev libpixman-1-dev libgcrypt20-dev libslirp-dev
bash tools/build-qemu-wsl.sh
ESP32S3_QEMU_BIN="$PWD/build-qemu-source/build-wsl/qemu-system-xtensa" bash tools/run-wsl.sh
```

The extension build creates a separate LF checkout in `build-qemu-source`, sharing
Git objects with the recovered source. It leaves the Windows checkout untouched.
Only initialize `esp-idf` if you want the historical reference checkout; new
firmware fixtures use the locked native SDK prepared above, not that gitlink.

To compile and exercise the normal ESP-IDF I2C sensor driver with a live host model:

```bash
ESP32S3_QEMU_BIN="$PWD/build-qemu-source/build-wsl/qemu-system-xtensa" \
    bash tools/build-boot-fixture.sh i2c_sensor
```

This verifies sensor CRC, a missing-address NACK, and a host-controlled temperature
change reaching firmware. It depends on the extension's existing response-map
mechanism and does not establish complete I2C timing behavior.

The launcher probes actual machine properties and native bridge capability.
Official QEMU can boot the fixture, while external device processes run independently
until a compatible QEMU bus extension is available. The custom bridge currently
routes by controller/address; GPIO wiring in circuit JSON is metadata, not an
electrical net solver. The UI exposes this distinction.

Firmware UART is reserved for guest communication. The old serial JSON bridge
tester requires the explicit `ESP32S3_SERIAL_BRIDGE=1` compatibility setting.
Production bus traffic needs the separate transport described in
[the architecture](docs/runtime-architecture.md).

## Product licensing boundary

QEMU remains a separate executable. QEMU and distributed modifications retain
their [GPLv2 obligations](https://www.qemu.org/docs/master/about/license.html).
The product UI, project format, orchestration, and independently developed device
services have separate licensing decisions; no proprietary license is imposed
on recovered code or third-party dependencies by this change.
