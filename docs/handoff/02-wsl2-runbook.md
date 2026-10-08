# Windows and WSL2 operations

## Environment and path boundaries

Host shell is PowerShell. Distro is Ubuntu WSL2; normal Linux user is polar.
Use Linux GCC/CMake/Ninja/Qt/ESP-IDF and WSLg. COM5 hardware tooling is a separate
Windows environment. Never interpret COM5 as /dev/ttyS5 without an explicitly
designed passthrough workflow.

~~~powershell
Set-Location C:/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM
wsl -l -v
wsl -d Ubuntu -- bash -lc 'whoami; uname -a'
~~~

Then enter WSL or prefix Linux commands with wsl -d Ubuntu -- bash -lc:

~~~bash
repo=/mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM
cd "$repo"
~~~

Commands below are Linux commands unless explicitly labeled PowerShell.
For the Codex execution tool, WSL calls have needed sandbox_permissions
require_escalated with prefix_rule ["wsl"]. Give a concrete task reason. An automatic
review denial is not user consent; preserve unaffected work and explain a real
remaining block. Managed Windows Python may need escalation to resolve its base
interpreter; WSL mock tests are a useful alternative.

Do not set HOME/home/CODEX_HOME as task variables. Quote paths. Do not use eval for
EIM exports, interpolate JSON as shell escaping, mix shells for recursive file
operations, or delete caches to recover a build without checking exact targets.

## Inspect before resuming

~~~bash
git status --short
git branch --show-current
git rev-parse HEAD
ps -C git -o pid,etime,args
ps -C ninja -o pid,etime,args
ps -C qemu-system-xtensa -o pid,etime,args
~~~

Linux rg is currently unavailable; Windows rg works. Reach for rg first, then
use grep/find or native shell tools when unavailable. Logs are under
build-runtime-state. An old tool session ID is not proof that a job is running.
Do not blindly retry a job or terminate another agent's compiler/device process.

## GUI build and test lanes

~~~bash
cmake -S gui-esp32s3-simulator -B gui-esp32s3-simulator/build-wsl +  -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build gui-esp32s3-simulator/build-wsl -j 6
~~~

One integration owner uses this shared build. Workers use separate /tmp build
directories or separately generated moc files.

Preferred one-command required lane from PowerShell:

~~~powershell
wsl -d Ubuntu -- bash /mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM/tools/validate-required-wsl.sh
~~~

Keep assignments/exports inside a saved Bash script or an interactive WSL shell.
An inline Windows-to-WSL bash -lc call lost $repo expansion during this session;
the saved script removed the extra quoting/expansion layer. For simple calls use
wsl --cd PATH -- env KEY=value EXECUTABLE ARGS without an additional shell.

The historical full 9-target lane used the recovered core and both development
fixtures; set all three variables so native tests do not silently skip:

~~~bash
export ESP32S3_QEMU_BIN="$repo/build-qemu-source/build-wsl/qemu-system-xtensa"
export ESP32S3_BOOT_FIRMWARE="$repo/tests/firmware/boot_smoke/build/boot-smoke.merged.bin"
export ESP32S3_I2C_FIRMWARE="$repo/tests/firmware/i2c_sensor/build/i2c-sensor.merged.bin"
ctest --test-dir gui-esp32s3-simulator/build-wsl --output-on-failure
python3 gui-esp32s3-simulator/device-sims/smoke_bridge_check.py
~~~

An unset fixture makes local optional native tests skip. A release/required lane
must not count those skips as a pass. Official QEMU does not have the recovered
cached I2C bridge, so do not run that lane against an official binary and infer
generic bus support.

Separate official stable boot:

~~~bash
export ESP32S3_QEMU_BIN="$repo/build-qemu-official-base/build-wsl/qemu-system-xtensa"
export ESP32S3_BOOT_FIRMWARE="$repo/tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.merged.bin"
./gui-esp32s3-simulator/build-wsl/gui_esp32s3_boot_smoke_test
~~~

Qt tests with widgets need QT_QPA_PLATFORM=offscreen in headless runs. WSLg is used
for interactive display. Preview the actual application after the shared build:

~~~bash
bash tools/run-wsl.sh --theme light --screenshot "$repo/gui-esp32s3-simulator/build-wsl/handoff-light.png"
bash tools/run-wsl.sh --theme dark --screenshot "$repo/gui-esp32s3-simulator/build-wsl/handoff-dark.png"
~~~

The screenshot option exits after about 1.5 seconds. Inspect both images; passing
unit tests does not establish visual usability or accessibility.
Latest inspected copies are retained in docs/handoff/assets/light.png and dark.png.

## QEMU source/build lanes

~~~bash
bash tools/build-qemu-official-wsl.sh
bash tools/build-qemu-wsl.sh
~~~

The first builds a pristine official lane; the second builds the recovered owner
core separately. Neither is the new hostbus extension.

For the tracked prototype:

~~~bash
bash tools/prepare-qemu-extension-wsl.sh
bash tools/build-qemu-hostbus-prototype-wsl.sh
~~~

The guarded apply script checks pinned HEAD, exact copy/patch paths, previous
applied file hashes and unrelated edits. It excludes only its own generated
build-hostbus directory. Read a refusal; do not remove safety checks to make it run.
Initial and repeated build provenance is in:

* build-runtime-state/extension-checkout.json
* build-runtime-state/hostbus-extension-source.json
* build-runtime-state/hostbus-runtime.json
* build-runtime-state/hostbus-build.log and hostbus-build-reviewed.log

The QAPI registration patch is prepared and in the apply metadata, but has not
been applied to the cache or tested at this snapshot. Use a new evidence output
directory when rerunning the enabled harness.

## ESP-IDF activation and fixture builds

New fixture builds default to locked **IDF 6.1**, commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. Full IDF source, recursive submodules,
toolchain/Python environment, and build output must be on native Linux disk.
Moving only `-B` while reading vendor headers under `/mnt/c` is not this contract.

~~~bash
bash tools/prepare-idf-wsl.sh idf-6.1
sdk="$HOME/.cache/esp32s3vm/idf/idf-6.1-fff9895c82d744c7237be8847347bdd1b07c6643"
source "$sdk/activate.sh"
export ESP32S3_QEMU_BIN="$repo/build-qemu-official-base/build-wsl/qemu-system-xtensa"
bash tools/build-boot-fixture.sh boot_smoke
~~~

The preparer prints its activation path. `ESP32S3_IDF_CACHE` overrides the cache
root (otherwise `${XDG_CACHE_HOME:-$HOME/.cache}/esp32s3vm/idf`).
`IDF_TOOLS_PATH` defaults to native `$HOME/.espressif`; a release-specific valid
environment is reused without reinstalling it. Non-native source, tools, Python
environment, or build roots are rejected. Installed-master working trees are
never a version fallback: only independent native Git objects with authoritative
origin may be reused, followed by an explicit locked-commit fetch/checkout.

Canonical activation exports:

| Variable | Meaning |
| --- | --- |
| `IDF_PATH` | `$sdk/source`, the complete locked SDK |
| `ESP32S3_FIXTURE_PROFILE` | `idf-6.1` (or explicitly prepared `idf-5.5.5`) |
| `ESP32S3_IDF_BUILD_ROOT` | `$sdk/builds`, native build output root |
| `ESP32S3_IDF_METADATA` | `$sdk/sdk-manifest.json`, origin/commit/requirements/tool manifest/gitlinks |
| `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` | Native release-selected toolcache and Python environment |

For the explicitly pinned 5.5.5 compatibility lane, prepare `idf-5.5.5` and
source its generated `idf-5.5.5-b774170ff46c393eeb5e495ea37936038d3f4f4f/activate.sh`.
The SDK helper refuses mismatched existing commits/origins rather than resetting
them. Historical `build-idf-*` checkouts and frozen boot/Arduino/hardware images
remain unchanged.

`build-boot-fixture.sh` consumes this activation (or `ESP32S3_IDF_ENV` selecting
a compatible native activation), checks the requested lock, snapshots authored
boot/sensor inputs under `$ESP32S3_IDF_BUILD_ROOT/<fixture>/<source-hash>/source`,
and writes `source-manifest.json`. Its separate native `build` directory pins
`SDKCONFIG` and `IDF_TARGET=esp32s3`; it produces ELF and merged 4 MiB flash,
records runtime identity, prints the image path, and runs the real controller
test. It does not overwrite repository fixture `build*` directories.

SPI/I2C/I2S/LCD_CAM/Pulse/UART/GPIO/ADC owners consume the same activation and
metadata. Their authored source/variant runners remain owner-controlled.
Every new IDF invocation must use explicit `-DIDF_TARGET=esp32s3`, native
`-B` and `SDKCONFIG`; if fixture/component sources are copied, record a frozen
source-hash manifest rather than silently copying them. Do not recompile a full
SDK per runtime variant when a single parameterized owner fixture suffices.

Independent source/identity evidence and actual firmware commands/results are
under `build-runtime-state/native-sdk-2026-10-07/`. The 6.1 source identity review
verified all **28** recursive submodules with the canonical runtime-manifest Git
parser. `owner-adoption.json` records the live consumer handoff; SDK identity
alone is not a firmware or peripheral qualification.

The independent ordinary **SPI connected** fixture proof used the full native
6.1 SDK: preparation **152.22 s**, actual compile/link **142.00 s**, 4 MiB merge
**14.66 s**, then official-QEMU boot **6.23 s**. UART captured
`main_task: Calling app_main()`, `SPI_NATIVE_BOOT profile=connected`, and the
fixture's capability gate. This qualifies the SDK path and provider-independent
startup, **not SPI transaction behavior** on the pristine official runtime.
`firmware-verification.json` retains exact argv, source snapshots, toolchain,
ELF/image hashes, duration records, and every unsuccessful transport attempt.

Recorded successful build/merge recipe (these are retained proof paths; new owner
runs allocate a new frozen-source workspace rather than overwriting evidence):

~~~bash
source "$sdk/activate.sh"
proof="$HOME/.cache/esp32s3vm/native-sdk-verification-20261007/run-094615-ddaf5442"
b="$ESP32S3_IDF_BUILD_ROOT/run-094615-ddaf5442"
python "$IDF_PATH/tools/idf.py" -C "$proof/source" -B "$b" \
  -DIDF_TARGET=esp32s3 -DSDKCONFIG="$b/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$proof/source/sdkconfig.defaults" build
(cd "$b" && python -m esptool --chip esp32s3 merge-bin \
  --output "$proof/flash-4MB.bin" --fill-flash-size 4MB @flash_args)
~~~

The successful QEMU run used an evidence-local writable flash copy and
`-serial file:<native-ext4-UART-log>`; the UART log was copied into repository
evidence after QEMU terminated. Earlier stdio attempts had no captured UART
markers, and a concurrent DrvFS UART-file read failed with errno 61; those
attempts remain recorded, not silently counted as passes.
The initial whole-DrvFS SDK transfer itself timed out after **1800.19 s** before
any compiler invocation. Its log/staging remain preserved. The final preparer
fetches the locked source directly onto native disk instead. This does **not**
establish DrvFS as the sole cause of all historical SPI/I2C/I2S watchdog failures.

**Historical hardware/reference exception:** `tools/build-reference-fixtures.sh`
and `tools/run-hardware-reference.py` are intentionally unchanged archival
physical-capture recipes. They are **not invoked by source-only continuation**
and are not the canonical SDK/build path for new live fixtures. Their captured
development-IDF source/image/manifests and original output paths remain
authoritative for those old golden captures. Do not invoke the historical build
helper to overwrite them; no new physical captures are authorized here.

## DC and host-only checks

~~~bash
cmake -S qemu-extensions/electrical -B build-electrical-dc -G Ninja
cmake --build build-electrical-dc
ctest --test-dir build-electrical-dc --output-on-failure
python3 tools/verify-plan.py --self-test
python3 tests/runtime_manifest_test.py
python3 tests/hardware_reference_test.py
python3 tests/hardware_deployment_test.py
~~~

The hardware tests use mocks; these commands do not authorize a physical run.
Windows mock commands use build-hardware-host/Scripts/python.exe. Physical
deployment, if later explicitly approved, must use that pinned esptool 5.4.0
environment; IDF 5.5's esptool 4.12.0 is not the deployment environment.

## Recovering context or rate limits

Preserve logs and the current tree. Query live state and inspect the newest
checkpoint before restarting. Completed process handles include 59596, 8191 and
91019 and 27782; do not poll or treat them as active jobs. A worker's displayed reset time
without a timezone is not a reliable scheduling promise. Do not burst retries.

The original chat's 30-minute heartbeat remains ACTIVE. On ownership transfer,
make the responsible chat/coordinator explicit so two integrations do not operate
the same checkout/build/hardware. This packet itself does not pause that automation.
