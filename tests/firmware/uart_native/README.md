# Ordinary native UART firmware (IDF 6.1 and pinned Arduino 3.3.12)

This fixture uses `uart_driver_install`, `uart_param_config`, `uart_set_pin`,
`uart_write_bytes`, `uart_read_bytes`, ordinary UART driver APIs, and the pinned
ordinary `driver/uhci.h` controller, DMA receive/transmit and callback APIs.
It never writes guest MMIO, enables internal loopback, injects console RX into
external circuits, or instantiates an ACK/sample/echo surrogate. Payloads for
UART1 and UART2 are generated independently; each RX is checked against the
other controller's actual transmitted bytes. Source creation is not runtime
qualification. Current exercised results are recorded in the
[UART source map](../../../qemu-extensions/prototypes/uart/source-map.json).

## Physical v3 project inputs

The actual project documents are in
`qemu-extensions/prototypes/uart/firmware/project-*.json`:

| Signal | Physical graph endpoints |
| --- | --- |
| UART1 TX → UART2 RX | GPIO17 → GPIO16 |
| UART2 TX → UART1 RX | GPIO18 → GPIO15 |
| UART1 RTS → UART2 CTS | GPIO11 → GPIO14 |
| UART2 RTS / GPIO override → UART1 CTS | GPIO13 → GPIO12 |
| UART0 external loopback | GPIO4 ↔ GPIO5 |
| Alternate intentionally disconnected RX2 | GPIO21, explicit 10 kΩ pull-up |
| Host stop/resume gate | GPIO10, explicit 1 kΩ pull-down; resumed graph moves resistor to 3.3 V |
| Console (no external peer) | GPIO43 TX, GPIO44 RX, separate singleton nets |

Every disconnected RX has an explicit physical pull-up, not an implicit ideal
idle value. The graph contains only an MCU, ground, a 3.3 V supply, wires and
resistors. UART peers are the real guest-configured peripherals on that MCU.
All graphs preserve the UART0 physical loopback. `absent` preserves crossed
wiring but deliberately does not install/configure UART2; `disconnected`
separates both data wires; `wrong` routes TX to GPIO19/20 instead of RX15/16.
No serial-console input is sent by the runner.
`uhci` uses the same actual crossed MCU pad nets as `connected`; UHCI0 selects
UART1 while UART2 is an independent ordinary UART driver peer.

## Acceptance vectors and evidence

Connected firmware exercises 513-byte full duplex at 9600, 115200 and 921600
baud; 5/6/7/8 data bits; disabled/even/odd parity; 1/1.5/2 stop bits; RX threshold
and timeout IRQs; RX route disconnection and reconnect; driver delete/install
reset and reconnect; physical GPIO-driven CTS stall/resume; queued-byte VM
stop/resume; deliberately mismatched parity, width and stop configuration;
break detection; actual FIFO overload with ordinary RX interrupts disabled;
actual 256-byte driver ring-buffer overload; and exact recovery afterward.
Hardware RTS threshold assertion is also exercised: ordinary RX service is
held, RTS physically blocks the peer's CTS, then enabling RX service resumes
and preserves all 513 bytes.

At `UART_NATIVE_HOST_STOP_READY`, 513 bytes are queued while physical CTS is
blocked. Firmware waits for the real GPIO10 level. The runner stops the VM,
captures the electrical snapshot, replaces only the externally described gate
resistor connection with VDD, captures another snapshot and resumes the VM.
Firmware observes GPIO10, drives GPIO13 low, and checks all 513 received bytes.
This case checks queued-state stop/resume plus CTS losslessness; it does not claim
an independently paused mid-symbol frame. The gate has a ten-second guest
failure timeout. Runner watchdogs are host diagnostics, never guest timeouts.
The ordinary runner defaults to the existing 900-second host limit. Native
electrical simulation resolves each physical UART transition; wall time can be
much longer than guest time. This host diagnostic setting does not extend
driver deadlines or turn a failed firmware result into PASS.


Advanced cases use ordinary `uart_set_mode` for IrDA, RS485 half duplex,
application control, and collision detection; `uart_detect_bitrate_start/stop`
for measured physical edges; and `uart_enable_pattern_det_baud_intr` for AT
pattern IRQ/position, pre/post-idle acceptance/rejection and character-gap
rejection. RS485, autobaud and AT use available ordinary pinned IDF6.1 APIs.
RS485 cases exercise UART digital mode logic, RTS behavior and physical RX/TX
comparison, not an absent differential transceiver or multidrop RS485 cable.

IrDA is **receive-only through ordinary APIs** in this fixture. The S3 mode is
half duplex: IRDA_TX_EN=0 enables receiving, while =1 enables transmitting and
disables receiving. `uart_set_mode(IRDA)` only sets IRDA_EN; the pinned public
UART driver exposes no TX-direction or WCTL setter. Those exact API boundaries
are reported as `irda_tx_direction,irda_wctl_control` unavailable, not patched
with a guest MMIO/LL helper or mislabeled full-duplex UART behavior.

The available IrDA receiver is exercised on UART2 RX16 using a genuine GPIO17
physical source on the existing crossed net. Idle-HIGH/active-LOW zero pulses
occupy sixteenth-cycle ticks8..10 (9th/10th/11th cycles), approximately20µs
starting52µs into a104µs9600-baud bit cell. Ordinary `gpio_set_level` and
`esp_rom_delay_us` emit four8N1 frames00/55/aa/ff while a short critical section
prevents guest-timer preemption. Actual UART RX bytes are read and compared;
UART1's TX pin route and UART2 normal mode/baud are restored afterward.
This is an electrical receive-path case only, not an optical-medium or
ordinary UART IrDA transmitter qualification. Native-register cases separately
cover TX_EN/WCTL, polarity inversion and half-duplex direction changes.

The stop-mismatch case reports observed frame IRQ and byte count. It requires
an error or changed stream; it does not manufacture an error when hardware
accepts a stop sample. UART0 temporarily remaps to GPIO4/5 and performs a
513-byte 57600-baud 8E2 physical loopback without printing onto that circuit.
Before installing or reconfiguring UART0, the fixture drains its hardware TX
FIFO with `uart_wait_tx_idle_polling`; `fflush(stdout)` alone only hands bytes
to the FIFO. A second drain preserves the driver's install-time queue log.
Without those drains, `uart_param_config` reset discarded queued PASS reports.
Results are retained, console parameters and GPIO43/44 restored, then results
are printed. The independent console chardev may mirror binary completed TX
frames during this case; raw `uart.log` preserves them. It is not an external
RX source, and the runner still requires every complete passing vector.

Each API, vector and final result has an explicit PASS/FAIL log. Full-duplex
RX data is printed as actual hex. The runner independently reconstructs both
payloads, checks all required log markers and minimum physical transfer time,
and requires zero failures. It records exact QMP traffic, graph snapshots,
commands and hashes. It never claims success from boot alone.

### Ordinary UHCI/GDMA image

The separate `uhci` firmware profile runs `uart_uhci_native.c`, grounded in the
pinned SDK's `esp_driver_uart/test_apps/uhci/main/test_uhci.c`. UART1 is configured
with `uart_param_config`/`uart_set_pin` and has no competing ordinary UART ISR
driver. UHCI0 selects that real controller. The ordinary UART2 driver supplies
independent actual TX and RX streams through crossed GPIO17/16 and GPIO18/15.
Both directions transfer 513 bytes. Real DMA callbacks copy only transient
actual callback data, record partial descriptor events and EOF/TX completion,
and compare all bytes against the independently transmitted peer stream.

Cases cover RX idle EOF, discontinuous three-buffer DMA TX (129 + 257 + 127),
delete/recreate controller reset/reconnect, RX packet-length EOF and RX break
EOF. A 256-byte mounted receive buffer and two descriptor nodes force actual
partial callbacks for the >FIFO stream. The first case also runs the ordinary
UART0 physical GPIO4/5 transfer while UHCI/UART2 transactions are active,
records its invocation time relative to the minimum physical TX duration,
and checks controller separation. There is no guest-MMIO helper, internal
loopback, fake UART callback, or GPIO-to-memory packet shortcut.

Packet separator and escape-sequence configuration are **unavailable through
the pinned ordinary UHCI API**: `driver/uhci.h` has no setters or configuration
fields, and `hal/uhci_hal.h` exposes only initialization/deinitialization.
Firmware reports those exact unavailable cases, rather than claiming the
native-register tests are ordinary API qualification. The SDK driver explicitly
disables separator substitution during controller initialization. Break-EOF
payloads exclude NULL data frames so the deliberate break is the sole EOF
terminator. Failure logs remain explicit. If unfinished RX prevents ordinary
controller deletion, callback/DMA storage is retained rather than freed while
hardware owns it, and the image fails without fabricated completion.

### Existing pinned Arduino wrapper image

`tests/firmware/uart_native/arduino` reuses the existing Arduino-as-IDF-component
infrastructure and selective core configuration from `tests/firmware/arduino_boot`.
It uses the already present Arduino3.3.12 component at
`build-compatibility/arduino-3.3.12/vendor/arduino`, commit
`94afccf35fb1e401facddbcf9e13bcf7c76a31d8`, with the canonical native IDF5.5.5
snapshot, commit `b774170ff46c393eeb5e495ea37936038d3f4f4f`. The existing
Arduino component is copied unchanged into the hashed native source snapshot;
the original stays read-only and compiler includes do not return to DrvFS.
Component-manager resolution is disabled: no source, SDK or registry-dependency
downloads occur in the owned builder. The existing minimal/selective component
convention is retained.

The real `HardwareSerial` globals Serial1/Serial2 perform independent
513-byte full-duplex transfers over the same GPIO17→16 and GPIO18→15 graph
nets at 9600/115200/921600, plus ordinary 7E2 and 8O2 framing. Normal
`begin`, buffer/clock configuration, `write`, `readBytes`, `flush`, and `end`
paths are exercised; each re-begin resets the real controller driver.
Serial0 then performs three separate 513-byte physical GPIO4/5 loopbacks at
all three baud rates. Its actual received streams and timing are retained,
GPIO43/44 console restored, and only then are all checks and RX bytes logged.
The independent console's binary mirror is preserved as raw evidence and is
never used as external RX. No internal loopback or echo surrogate is used.

The runner separately checks Arduino3.3.12/IDF5.5.5 boot identity, the locked
profile commits, all required zero-failure checks, actual full-duplex and
UART0 loopback hex streams, framing durations and actual baud rates.
Freeze verifies both source revisions and records hashes of the existing
Arduino component source tree in immutable `arduino-source-tree.json`.
Any resulting runtime qualification is **separate IDF5.5.5 evidence**, not IDF6.1 Arduino evidence.

## Delegate build, freeze, then run

Prerequisites: WSL/Linux; SDK-owner-prepared canonical native IDF6.1 at
`fff9895c82d744c7237be8847347bdd1b07c6643` and explicit IDF5.5.5 for Arduino,
with their already ready activation scripts and SDK identity manifests;
native Xtensa tools/Python environments; the final separate GPL QEMU executable
built from locked base `40edccac415693c5130f91c01d84176ae6008566`, frozen26
prefix and landed UART/UHCI/GDMA/GPIO/electrical/coreclock dependencies; a final combined
runtime-source JSON describing that executable; public electrical
`project-json`/`snapshot-json` QMP properties; and the UART owner declaring the
model ready for delegated smoke. Do not launch against the pristine baseline.

The shared SDK owner uses `tools/prepare-idf-wsl.sh`; the UART builder does not
invoke preparation, install tools, fetch sources, or fall back to historical
repository `build-idf-*` directories. Its required public activation exports
are `IDF_PATH`, `IDF_PYTHON_ENV_PATH`, `ESP32S3_FIXTURE_PROFILE`,
`ESP32S3_IDF_BUILD_ROOT`, and `ESP32S3_IDF_METADATA`. Source/profile/commit and
native build filesystem must match the canonical ready contract.

From the repository root, activating each existing pinned SDK tool environment:

```sh
QEMU=/absolute/path/to/final/native/qemu-system-xtensa
RUNTIME_SOURCE=/absolute/path/to/final/combined/runtime-source.json
FROZEN_ROOT="$PWD/build-runtime-state/uart-native-frozen"
EVIDENCE="$PWD/build-runtime-state/uart-native-runs"
mkdir -p "$FROZEN_ROOT"
SDK_CACHE="${ESP32S3_IDF_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/esp32s3vm/idf}"
SDK61_ACTIVATE="$SDK_CACHE/idf-6.1-fff9895c82d744c7237be8847347bdd1b07c6643/activate.sh"
SDK555_ACTIVATE="$SDK_CACHE/idf-5.5.5-b774170ff46c393eeb5e495ea37936038d3f4f4f/activate.sh"
# Both activation scripts/manifests must already be SDK-owner READY.
source "$SDK61_ACTIVATE"
for MODE in connected absent disconnected wrong uhci; do
  bash qemu-extensions/prototypes/uart/firmware/build-idf-fixture.sh \
    "$MODE" "$QEMU" "$RUNTIME_SOURCE" "$FROZEN_ROOT/$MODE"
done
source "$SDK555_ACTIVATE"
bash qemu-extensions/prototypes/uart/firmware/build-idf-fixture.sh \
  arduino "$QEMU" "$RUNTIME_SOURCE" "$FROZEN_ROOT/arduino"
# All ordinary boot images are frozen before any VM launch.
for MODE in connected absent disconnected wrong uhci arduino; do
  python3 qemu-extensions/prototypes/uart/run-native-fixture.py run \
    --qemu "$QEMU" --frozen "$FROZEN_ROOT/$MODE" --evidence "$EVIDENCE"
done
```

`prepare-fixture-source.py` snapshots only owned firmware inputs plus the full
existing Arduino component when selected. Source/SDK identity and all file
hashes determine a preserved native directory:
`$ESP32S3_IDF_BUILD_ROOT/uart_native/MODE-SHA/source`, with Arduino under
`MODE-SHA/arduino`, `source-manifest.json`, and compiler output under
`MODE-SHA/build`. SDKCONFIG is explicitly in that native build directory.
An existing changed or incomplete snapshot is never rewritten. Historical
repository builds and original sources remain untouched.

The builder merges ordinary bootloader/partition/application flash to4MiB and
invokes `freeze` without launching. Freeze requires/captures the canonical
`sdk-manifest.json`, exact compiled `source-manifest.json`, actual snapshot
source files (including Arduino), flash, ELF, sdkconfig, runtime source and
fixture/graph/runner sources. It verifies SDK source/profile/commit and native
artifact provenance, plus the Arduino pin, and locks the executable hash.
Run rejects changed frozen inputs, executable or current runner source before
starting QEMU and uses snapshot-on flash. Use new frozen directories rather
than rewriting prior evidence. Build/runtime success is still separate from
these source identities.

Existing pinned Arduino infrastructure permits the separate IDF5.5.5 wrapper
above. The lock currently has no Arduino-on-IDF6.1 firmware profile, so this
fixture makes no such qualification claim; SDK mismatch is not treated as
making the existing Arduino3.3.12/IDF5.5.5 infrastructure unavailable.

## Fresh QEMU-only ROM download regression

`qemu-extensions/prototypes/uart/run-rom-download.py` is separate from firmware
execution. It starts a fresh VM using the **same frozen native QEMU executable**
and `-global driver=esp32s3.gpio,property=strap_mode,value=0`, with no external
electrical project. The public project getter must report its real no-project
state before continuing the ROM. Console/download UART0 uses an independent
TCP chardev. Both TCP endpoints are process-created localhost sockets; no COM
port, hardware adapter or external device connection is accepted.

The existing pinned esptool5.4.0/pyserial3.5 environment is required. The APIs
are grounded in installed tool source: `ESP32S3ROM.connect(mode="no-reset",
attempts=1, detecting=True)`, `mem_begin`, `mem_block`, and `check_command`.
The standard connection procedure sends up to five identical SYNC attempts;
chip detection, register/security commands, flash/eFuse operations and hardware
reset are disabled. Freeze uses the installed official S3 stub text
metadata solely as an inert RAM payload and documented IRAM address. The
stub text is never executed. It captures payload/source hashes and metadata
before any VM launch; no package/source download or installation is performed.

The runner waits for the complete CRLF-terminated actual ROM download banner.
Raw banner bytes are retained as evidence but excluded from the tool stream,
preventing a relay/input-flush race. It requires eight ordinary ROM SYNC replies
(not stub synchronization), then performs MEM_BEGIN, all
checksum-protected MEM_DATA blocks (>128-byte data is mandatory), and checked
MEM_END with `(no_execute=1, entry=0)`. It uses ordinary `check_command` for
MEM_END because the library's `mem_finish` suppresses missing ROM ACK errors;
this regression requires the actual ACK instead of silently accepting it.

The TCP relay records actual bytes, never creates an ACK or echo. Only the
initial captured banner is withheld; protocol bytes forward unmodified.
Evidence contains both raw serial directions, all strict
SLIP-decoded ROM status replies, sent payload/checksum records, QMP traffic,
the command, source/executable hashes and snapshots. Unknown/malformed SLIP
input fails rather than being converted into a successful response. A writable
flash copy lives only under a fresh evidence directory; memory-only protocol
must leave its hash unchanged. No guest RAM readback, MMIO helper, flash write
or payload execution is used as qualification. The result is specifically ROM
checksum/status-ACK traversal, not downloaded-RAM readback or stub execution.

After final model/source readiness, all ordinary images are frozen, and the
existing pinned esptool5.4.0 tool environment is selected:

```sh
ROM_FROZEN="$FROZEN_ROOT/rom-download"
source "$SDK61_ACTIVATE"
ROM_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
"$ROM_PYTHON" qemu-extensions/prototypes/uart/run-rom-download.py freeze \
  --boot-frozen "$FROZEN_ROOT/connected" --output "$ROM_FROZEN"
# Freeze copies/hashes inputs only; no QEMU launch occurs above.
"$ROM_PYTHON" qemu-extensions/prototypes/uart/run-rom-download.py run \
  --qemu "$QEMU" --frozen "$ROM_FROZEN" --evidence "$EVIDENCE" \
  --watchdog-seconds 60
```

The existing Linux IDF6.1 environment is recorded in
`tests/firmware/boot_smoke/build-idf-6.1/CMakeCache.txt` (PYTHON path), and its
previous `log/idf_py_stdout_output_923383` records esptool5.4.0 at lines1087/1303.
No old build/test was rerun to obtain that identity. The prepared host package
lock also records esptool5.4.0 and pyserial3.5 in
`build-hardware-host/installed-packages.txt`; reading that existing source is
not a hardware operation. Consume the canonical native IDF6.1 activation's
Python environment above after the Arduino IDF5.5.5 build; do not fall back to
the historical SDK checkout or silently download/rewrite dependencies.
No ROM regression or baseline comparison was launched by this
author; fresh ROM behavior remains pending actual delegated verification.

