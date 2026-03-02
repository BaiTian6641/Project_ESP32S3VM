# GUI ESP32-S3 Simulator

Simple Qt-based GUI simulator with integrated QEMU launching.

## Goal
Use QEMU ESP32-S3 simulation as the core and layer flexible external peripheral simulators
(for example I2C display and sensor devices) on top.

## MVP scope (simple start)
1. Basic Serial view (default UART0)
2. Processor status inspection
	 - PC counter
	 - Scalar registers
	 - Vector registers
	 - Memory inspect
	- Real-time live mode
	- PC stepping mode
3. Basic control
	 - Reset
	 - Boot mode
	 - Firmware file loading
	 - SPI flash enable + size selection (2/4/8/16 MB, default 16 MB)
	 - Optional base MAC override
	 - Optional chip revision override

## Current scaffold
- Qt6 Widgets app skeleton
- Main tabs:
	- Serial
	- Processor Status
	- Control
	- Debug
	- Peripherals

## Serial tab settings
- Default profile on startup: `115200 8N1`
- Communication type: UART TTL / RS-232 / RS-485
- Baud rate: preset list + editable custom value
- Data bits: 5 / 6 / 7 / 8
- Parity: None / Even / Odd / Mark / Space
- Stop bits: 1 / 2
- Flow control: None / RTS/CTS / XON/XOFF
- TX line ending: LF / CRLF / CR / None

`Apply Serial Settings` stores and applies configuration in the simulator backend,
and sending data uses the selected line-ending behavior.

## Virtual serial device
- Virtual UART PTY is created when GUI starts (before firmware load) and logs the device path:
	- `[Serial] virtual UART device ready: /dev/pts/<N>`
- Simulator also creates a stable alias:
	- `/tmp/esp32s3-uart -> /dev/pts/<N>`
- External tools/IDEs can use `/tmp/esp32s3-uart` like a USB-serial adapter connection.
- For `esptool`, prefer the real PTY path (`/dev/pts/<N>`) shown in logs for best compatibility.
- After firmware is loaded, GUI Serial tab receives UART output immediately.
- Preferred fixed monitor command:
	- `python3 -m serial.tools.miniterm /tmp/esp32s3-uart 115200 --raw`
- Device exists for the GUI session and remains stable across firmware restart/reset.
- On platforms without Linux PTY support, simulator still runs with integrated GUI serial,
	but external PTY-based tools are unavailable.
- Backend controller with simple QEMU launcher + UART0 stdio streaming
- Peripheral hook format:
	- Schema: peripherals/peripherals.schema.json
	- Example: peripherals/peripherals.example.json

## Current runtime behavior (simple)
- `Load Firmware` starts/restarts QEMU using:
	- ELF (`.elf`): `-M esp32s3[,boot-mode=...][,mac=..][,chip-revision=..] -display none -monitor none -serial stdio -kernel <firmware>`
	- BIN (`.bin`): creates a per-process temporary SPI flash image and starts with
		  `-drive file=<temp>/esp32s3_gui_flash_<pid>.bin,if=mtd,format=raw`
- Serial tab receives QEMU stdout/stderr merged stream.
- Sending text in Serial tab writes to QEMU stdin (UART0 path), except in
	`Download Boot` mode where manual input is blocked to protect the ROM
	downloader stream for `esptool`.
- In `Normal Boot`, incoming UART stream is scanned for ESP ROM sync preamble
	(`0x07 0x07 0x12 0x20`). When detected, simulator auto-switches to
	`Download Boot` and restarts with the current firmware image.
- `Reset` restarts the QEMU process with the last selected firmware.
- `Boot Mode` maps to machine property `boot-mode=flash|download`.
- In Control tab, `Enable SPI Flash` and `SPI Flash Size (MB)` apply on `Load Firmware`.
- In Control tab, `Enable PSRAM`, `PSRAM Size (MB)` and `PSRAM Mode (QSPI/OPI)`
	apply on `Load Firmware`:
	- Adds `-m <size>M` to allocate external PSRAM backing store
	- Adds machine argument `psram-mode=qspi|opi`
- In Control tab, `Base MAC` (format `AA:BB:CC:DD:EE:FF`) and `Chip Revision`
	(encoded as `major*100 + minor`, e.g. `203`) apply on `Load Firmware`.
- Control tab also provides `Copy esptool Command`, which copies a command using
	the current live PTY path (`/dev/pts/<N>` when available) and includes
	`--no-stub` for better QEMU compatibility.
	If PTY is unavailable, command text explains that external flashing port is not available.
	For faster transfers, append `--baud <rate>` (esptool negotiates after sync).
- Processor Status debug controls:
	- `Live`: periodic real-time snapshot refresh
	- `Pause` / `Continue`
	- `Step PC`: single-instruction step request
	- breakpoint add/clear controls
	- memory base address selector for inspect table

- Debug tab controls:
	- Enable/disable QEMU gdbstub
	- Select GDB port (default 1234)
	- Optional wait-for-attach mode (`-S`)
	- Firmware picker + one-click `Start with GDB`
	- Execution control (pause/continue/step)
	- Breakpoint add/clear
	- Exact gdb attach command shown in-tab

## Debug backend path
- On non-Windows builds, uses QMP over per-process UNIX socket
	(`-qmp unix:/tmp/esp32s3_gui_qmp_<pid>.sock,server=on,wait=off`)
- On Windows builds, QMP path is currently disabled in this launcher.
- Optional gdbstub when enabled in Debug tab:
	- `-gdb tcp::<port>`
	- plus `-S` when wait-for-attach is checked

## QEMU binary integration
- GUI auto-discovers `qemu-system-xtensa` from bundled-relative paths and `PATH`.
- You can still override binary path explicitly with environment variable:
	- `ESP32S3_QEMU_BIN=/absolute/path/to/qemu-system-xtensa`

## Performance notes
- QEMU launch uses TCG multithread settings by default:
	- `-accel tcg,thread=multi,tb-size=1024`
- Live debug polling interval is tuned to reduce overhead (500 ms).
- Serial console keeps a bounded number of lines (4000) to avoid UI slowdowns
	during high-throughput logging.
- For highest speed, keep Debug Live updates disabled unless actively inspecting state.
- Snapshot sequence:
	1. `query-cpus-fast` for PC
	2. `human-monitor-command: info registers` for scalar/vector parse
	3. `human-monitor-command: xp /8wx <base>` for memory words

	## GDB quick use
	1. Open Debug tab and click `Start with GDB`.
	2. Select firmware when prompted (or pre-fill firmware path).
	3. Copy the attach command shown in the Debug tab.
	4. In gdb:
		- `target remote 127.0.0.1:1234` (or chosen port)

## Build (Linux)
Prerequisites:
- Qt6 development packages
- CMake 3.21+
- C++17 compiler

Build steps:
1. mkdir -p build
2. cd build
3. cmake ..
4. cmake --build .

Run:
- ./gui_esp32s3_simulator

## Build (Windows)
Prerequisites:
- Qt6 (MSVC kit)
- CMake 3.21+
- Visual Studio C++ toolchain

Build steps (Developer PowerShell):
1. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
2. `cmake --build build --config Release`

Run:
- `build\\Release\\gui_esp32s3_simulator.exe`

## CI
- Linux + Windows GUI build workflow:
	- `.github/workflows/gui-build-linux-windows.yml`

## Project layout
- src/: Qt UI widgets and main window
- backend/: QEMU controller abstraction
- peripherals/: external device hook schema and examples
- docs/: architecture notes

## Notes on peripheral flexibility
The peripherals JSON format is designed to be device-tree-like:
- describe bus hookup (i2c/spi/uart/gpio/custom)
- identify controller and address/chip-select/pins
- point to an external simulator executable and transport

This lets users model and hook devices similarly to real hardware wiring,
while keeping simulation logic in independently developed device programs.

## Peripheral manager runtime (implemented)
- Peripherals tab can load JSON config, start/stop simulators, refresh state,
	and inspect per-device capabilities/state/logs.
- On app startup, GUI auto-discovers and loads a default config when available:
	- `peripherals/peripherals.example.json` (searched via app/workspace relative paths)
	- or explicit override via `ESP32S3_PERIPHERALS_CONFIG=/abs/path/config.json`
- Python simulator self-discovery:
	- If simulator `exec` is a `.py`/`.pyw` script, manager auto-selects Python runtime.
	- Search order: `ESP32S3_PYTHON` -> workspace `.venv` -> system Python.
	- Linux prefers `python3`; Windows prefers `py -3` then `python.exe`.
- Peripheral process lifecycle is bound to QEMU lifecycle:
	- QEMU start -> configured peripherals auto-start
	- QEMU stop/reset/exit -> peripherals auto-stop

### Validation and traces (implemented)
- Config load performs runtime validation for:
	- required `board`/`board.name` and `devices[]`
	- duplicate `buses[].id` and duplicate `devices[].id`
	- required per-device `id`, `type`, `bus.kind`, `bus.controller`, `simulator.exec`
	- protocol-vs-bus sanity check (`jsonrpc-i2c|spi|uart`)
- Validation errors are shown in Peripherals log and block loading invalid configs.
- Per-device JSON-RPC trace stream is shown in Peripherals log:
	- outbound requests as `[trace][TX]`
	- inbound responses/notifications as `[trace][RX]`

### SPI/UART transport coverage (current)
- SPI simulator support (`jsonrpc-spi-v1`) includes:
	- `spi_transfer` with mode metadata: `single`, `dual`, `quad`
	- frequency field (`frequency_hz`) and byte-level TX/RX payloads
	- reference script: `device-sims/spi_mem_sim.py`
- UART simulator support (`jsonrpc-uart-v1`) includes:
	- `uart_set_line` for baud/data bits/parity/stop bits
	- `uart_tx` request and `uart_rx` notification loopback path
	- reference script: `device-sims/uart_loopback_sim.py`
- Example config now includes I2C + SPI(quad) + UART devices:
	- `peripherals/peripherals.example.json`

### QEMU -> Peripheral bridge event ingestion
- `QemuController` now listens for structured serial lines and forwards them to `PeripheralManager`:
	- `[PERIPH][I2C] { ...json... }` -> `i2c_transfer`
	- `[PERIPH][SPI] { ...json... }` -> `spi_transfer`
	- `[PERIPH][UART] { ...json... }` -> `uart_set_line` (optional) + `uart_tx`
- Matching is performed by `bus.kind` + `bus.controller` (+ address/cs/unit when provided).
- This enables incremental QEMU integration: once firmware/QEMU emits these lines, GUI bridge dispatches to active simulator processes immediately.

### Peripheral -> QEMU bridge response path
- `PeripheralManager` emits structured bridge responses after simulator RPC returns.
- `QemuController` forwards them to serial channel as:
	- `[PERIPH][I2C][RSP] { ...json... }`
	- `[PERIPH][SPI][RSP] { ...json... }`
	- `[PERIPH][UART][RSP] { ...json... }`
- Response payload includes:
	- `ok`, `bus`, `method`, `device_id`, original `request`, and either `result` or `error`.
- This provides an end-to-end request/response loop for QEMU-side bridge handlers.

Example event payloads:
- I2C:
	- `[PERIPH][I2C] {"controller":"i2c0","address":"0x40","ops":[{"dir":"write","data":[227]},{"dir":"read","len":3}],"clock_hz":100000,"repeated_start":true}`
- SPI (quad):
	- `[PERIPH][SPI] {"controller":"spi2","chip_select":0,"mode":"quad","frequency_hz":20000000,"tx":[159],"rx_len":3}`
- UART:
	- `[PERIPH][UART] {"controller":"uart1","unit":1,"baud":115200,"data_bits":8,"parity":"none","stop_bits":1,"data":[72,105]}`

### Bridge smoke verification
- Run:
	- `cd device-sims && python3 smoke_bridge_check.py`
- Covers:
	- I2C transfer behavior (`ssd1306_sim.py`, `sht21_sim.py`)
	- SPI single/dual/quad transfer behavior (`spi_mem_sim.py`)
	- UART line config + TX loopback (`uart_loopback_sim.py`)
	- Presence of expected devices in `peripherals/peripherals.example.json`

### Automated bridge contract test (Qt)
- Test target: `gui_esp32s3_bridge_contract_test`
- Verifies:
	- parsing `[PERIPH][I2C|SPI|UART]` request lines into typed controller signals
	- formatting/emission of `[PERIPH][BUS][RSP]` response lines
- Run locally:
	- `cmake --build build --config Release -j 4`
	- `ctest --test-dir build --output-on-failure -C Release`

### Arduino ESP32-S3 bridge tester program
- Added Arduino sketch for end-to-end bridge testing:
	- `tests/arduino/esp32s3_bridge_tester/esp32s3_bridge_tester.ino`
- The sketch emits `[PERIPH][I2C]`, `[PERIPH][SPI]`, `[PERIPH][UART]` request lines,
	parses `[PERIPH][...][RSP]` responses, and prints pass/fail counters.
- Setup/usage notes:
	- `tests/arduino/README.md`
- Arduino CLI build script:
	- `tests/arduino/build_esp32s3_bridge_tester.sh`
- CI compile workflow:
	- `.github/workflows/arduino-bridge-sketch.yml`

## Peripheral system roadmap

### Phase 1: Runtime core (MVP)
- Add `PeripheralManager` in backend:
	- load `peripherals/*.json`
	- validate against `peripherals.schema.json`
	- spawn/monitor simulator processes
	- bind lifecycle to QEMU start/stop/reset
- Define one stable transport first: JSON-RPC over `stdio`
- Deliverable: GUI can start/stop peripheral processes and show health/log status.

### Phase 2: I2C adapter first
- Implement I2C transaction bridge path (addressed read/write, repeated-start, timeout).
- Add at least one reference simulated device (e.g. `tmp102` or `ssd1306`).
- Deliverable: ESP-IDF I2C sample app can exchange data with peripheral simulator.

### Phase 3: Peripheral Manager tab
- Add dedicated GUI tab:
	- config file picker
	- validate button with error location
	- per-device status (running/error/restarting)
	- per-device log panel
- Deliverable: complete no-code flow for loading a peripheral config and running it.

### Phase 4: SPI + UART adapters
- Add SPI bridge (chip-select scoped transaction model).
- Add UART bridge (byte-stream + optional framing helper).
- Reuse the same JSON-RPC process protocol used in I2C phase.

### Phase 5: Reliability and tooling
- Add retries/reconnect policy, startup ordering, and timeout policy.
- Add trace capture (bus transaction timeline) and validation diagnostics.
- Add known-good examples + smoke tests for CI.

## Peripheral modeling language choice

### Recommended default: Python
- Best for fast iteration and easy device behavior scripting.
- Strong ecosystem for protocol/device helpers and quick debugging.
- Easy for users to author new peripherals without recompiling.

### When to use Rust/C++ instead
- Use Rust/C++ for performance-critical simulators (high-rate SPI streams, heavy DSP, strict timing).
- Use Rust/C++ when strong type safety, lower latency, or native binary distribution is required.

### Practical decision
- Start with Python as the first-class peripheral scripting language.
- Keep transport language-agnostic (`exec` + args + JSON-RPC), so teams can migrate selected devices to Rust/C++ later without changing GUI architecture.

## Device sub-control panel plan (comprehensive)

Goal: each configured peripheral has its own interactive panel in GUI, so users can observe and drive device behavior without changing firmware code.

### 1) Target UX
- Add a `Peripherals` tab with two regions:
	- Left: device list (`id`, type, bus, status)
	- Right: selected device sub-control panel (dynamic by device capability)
- Device cards/panels are generated from capability metadata provided by peripheral simulator process.
- Keep one panel per device instance (for example `screen0`, `temp0`, `imu0`).

### 2) Capability model (from simulator)
- On startup, GUI sends `get_capabilities` JSON-RPC request to each simulator process.
- Simulator replies with a declarative UI schema, for example:
	- display viewport (width/height/pixel format)
	- writable parameters (temperature, humidity, thresholds, flags)
	- supported input sources (manual, CSV playback, scripted waveform)
	- telemetry channels (current value, last update time, quality/state)
- GUI uses this schema to render controls and validate values before sending commands.

### 3) Panel types (first-class)

#### 3.1 Display devices (SSD1306/ST77xx/etc.)
- Built-in display widget in sub-panel:
	- live framebuffer preview
	- zoom (1x/2x/4x)
	- optional grayscale/palette modes
	- frame rate and update counter
- Controls:
	- clear/freeze frame
	- capture PNG snapshot
	- optional record GIF/MP4 (later phase)
- Transport:
	- simulator emits `frame_update` events (full frame or dirty-rect diff)
	- GUI acknowledges/render timing for back-pressure safety.

#### 3.2 Scalar sensors (temp/light/pressure/etc.)
- Manual mode:
	- editable value field(s)
	- rate/step controls
	- apply-now button
- CSV mode:
	- file picker
	- column mapping (timestamp/value/unit/quality)
	- playback controls (play/pause/stop/loop/speed)
	- seek slider + current sample index
- Optional waveform mode (phase 2): sine/noise/ramp generators.

#### 3.3 Complex sensors (IMU/GNSS/etc.)
- Multi-channel table + mini-plot widgets.
- Per-channel manual override and CSV mapping.
- Preset profiles (stationary, walking, rotating, custom trace).

### 4) CSV ingestion strategy
- Support two time bases:
	- absolute timestamp (ISO8601/epoch)
	- relative seconds from start
- Required parser features:
	- delimiter auto-detect (comma/semicolon/tab)
	- header detection and manual mapping fallback
	- missing value policy (hold/zero/skip)
	- unit conversion hook (for example `°F -> °C`)
- Runtime behavior:
	- deterministic playback clock (simulation-time based)
	- drift-safe scheduling for long traces
	- clear error reporting at row/column granularity.

### 5) Backend architecture changes
- Add `PeripheralManager` core services:
	- process lifecycle manager
	- JSON-RPC request/response routing
	- event stream fan-out to GUI
	- per-device state cache
- Add `DevicePanelFactory` in GUI layer:
	- builds panel widgets from capability schema
	- binds controls to RPC commands
	- subscribes to telemetry/frame updates
- Add optional `CsvPlaybackEngine` utility:
	- shared by all sensor-like panels
	- supports scrub/replay/speed/loop.

### 6) Protocol additions (JSON-RPC)
- Required methods:
	- `get_capabilities`
	- `set_parameter`
	- `start_csv_playback`
	- `pause_csv_playback`
	- `seek_csv_playback`
	- `stop_csv_playback`
	- `get_state`
- Event notifications:
	- `state_changed`
	- `telemetry`
	- `frame_update` (display devices)
	- `error`
	- `playback_finished`

### 7) Reliability and safety
- Per-device watchdog/timeout and auto-restart policy.
- Soft-fail behavior: one faulty simulator must not break others.
- Back-pressure control for high-rate events (drop/coalesce policy).
- Persistent panel state (last CSV file, speed, manual values) per device id.

### 8) Phased delivery plan

#### Phase A (MVP)
- Peripherals tab + device list
- one dynamic panel type for scalar sensor
- manual set value + CSV playback basic controls
- basic telemetry text view

#### Phase B
- display panel with live framebuffer
- frame diff updates + snapshot export
- CSV column mapping UI improvements

#### Phase C
- multi-channel sensor panel templates (IMU style)
- plots and presets
- protocol tracing and replay export

#### Phase D
- profile import/export (full peripheral scenario)
- headless batch playback mode for CI regression tests

### 9) Acceptance criteria
- User can select any configured device and get an interactive sub-panel.
- Display devices show live rendered content in GUI.
- Sensor devices support both manual override and CSV playback with timeline controls.
- Peripheral process failure is isolated and clearly visible in status/logs.

## Implemented device simulators (first slice)

The following Python peripheral simulators are now available:
- `device-sims/ssd1306_sim.py`
- `device-sims/sht21_sim.py`
- `device-sims/gpio_test_sim.py`
- shared JSON-RPC stdio server helper: `device-sims/jsonrpc_stdio.py`

### SSD1306 simulator
- Transport: JSON-RPC over `stdio`
- Bus support: I2C (`0x3C`/`0x3D`)
- Implemented behavior:
	- command handling for page/column addressing
	- display on/off, invert, contrast
	- GDDRAM writes via data stream
	- `frame_update` notification after framebuffer changes
	- transaction-coherent I2C frame flush at end of transfer for stable UI updates

### GPIO test simulator
- Transport: JSON-RPC over `stdio`
- Purpose: quick GPIO interaction testing in GUI
- Controls:
	- Inputs: button, switch
	- Outputs: LED
	- RGB LED via PWM channels: `pwm_r`, `pwm_g`, `pwm_b`

### SHT21 simulator
- Transport: JSON-RPC over `stdio`
- Bus support: I2C (`0x40` by default)
- Implemented behavior:
	- temperature/humidity measurement commands (`0xE3/0xF3`, `0xE5/0xF5`)
	- user register read/write (`0xE7`, `0xE6`)
	- soft reset (`0xFE`)
	- manual parameter mode (`temperature_c`, `humidity_rh`)
	- CSV playback (`start_csv_playback`, pause/seek/stop)
- Included sample trace: `device-sims/sht21_sample.csv`

### JSON-RPC methods currently implemented
- Common:
	- `ping`
	- `get_capabilities`
	- `get_state`
	- `set_parameter`
- Bus-facing:
	- `i2c_write`
	- `i2c_read`
	- `i2c_transfer` (preferred realistic transaction model)
- Playback (SHT21):
	- `start_csv_playback`
	- `pause_csv_playback`
	- `seek_csv_playback`
	- `stop_csv_playback`

### Device-owned subpanels (capability schema)
The Peripherals tab subpanel is now defined by each virtual device via
`get_capabilities.result.panel`. If `panel` is present, GUI renders controls
with `SchemaDevicePanel`; otherwise it falls back to a generic JSON view.

Example `get_capabilities` payload fragment:

```json
{
	"panel": {
		"kind": "sensor",
		"title": "SHT21 Sensor",
		"description": "Environment source for test scenarios.",
		"controls": [
			{
				"name": "temperature",
				"label": "Temperature",
				"type": "float",
				"min": -40,
				"max": 125,
				"step": 0.1,
				"unit": "°C",
				"section": "Environment",
				"description": "Injected ambient temperature.",
				"writable": true
			},
			{
				"name": "humidity",
				"label": "Humidity",
				"type": "float",
				"min": 0,
				"max": 100,
				"step": 0.1,
				"unit": "%",
				"section": "Environment",
				"writable": true
			},
			{
				"name": "reset_stats",
				"label": "Reset Statistics",
				"type": "action",
				"rpc_method": "reset_stats",
				"rpc_params": {"scope": "all"},
				"section": "Operations"
			}
		]
	}
}
```

Supported `controls[].type` values:
- `bool`
- `int`
- `float` / `double`
- `enum` (requires `options`)
- `string`
- `action`

Useful control fields:
- required: `name`, `type`
- optional display: `label`, `description`, `section`, `unit`, `placeholder`
- optional numeric constraints: `min`, `max`, `step`, `decimals`
- behavior: `writable` (false means read-only)
- action RPC: `rpc_method`, `rpc_params`

Optional dynamic view blocks:
- `display`: for `kind: "display"`; supports `state_key`, `fallback_state_key`, `layout`, `encoding`
- `metrics`: live summary cards with `label`, `state_path`, optional `decimals`, `unit`, `true_text`, `false_text`
- `scripts`: read-only text/JSON panes with `title`, `state_path`, optional `min_height`

Script-owned panel container (optional):
- `script.enabled`: when true, GUI forwards subpanel UI events to device script RPC
- `script.event_method`: RPC method name for UI events (default `panel_event`)
- `script.state_method`: RPC method name for state sync from GUI (optional)
- `script.runtime_state_key`: where GUI stores script runtime model in state (default `panel_runtime`)
- `script.fallback_set_parameter`: when true, GUI also sends normal `set_parameter` for compatibility

Panel script RPC contract:
- GUI -> script `panel_event`: `{ event, control, value, rpc_method?, rpc_params? }`
- GUI -> script `panel_state`: `{ state, panel }` (optional, if `state_method` configured)
- Script -> GUI response can include:
	- `panel_state` (stored in `state.panel_runtime` by default)
	- `state_patch` (merged into top-level device state)
- Script can also push notifications: `panel_update` or `panel_state` with params object.

### Notifications currently emitted
- `frame_update` (SSD1306)
- `telemetry` (SHT21)
- `playback_finished` (SHT21)

## Realistic communication model (hardware-like)

To mimic real-world wiring and protocol behavior, use transaction-level methods
instead of split write/read calls whenever possible.

### Preferred I2C API: `i2c_transfer`
- Single request can represent a full bus transaction sequence:
	- START
	- address phase
	- write and/or read segments
	- optional repeated-start
	- STOP
- Supports per-op acknowledgment reporting (`acked_bytes`) and read payloads.

Example request:

```json
{
	"jsonrpc": "2.0",
	"id": 10,
	"method": "i2c_transfer",
	"params": {
		"address": "0x40",
		"clock_hz": 100000,
		"repeated_start": true,
		"ops": [
			{"dir": "write", "data": [227]},
			{"dir": "read", "len": 3, "nack_last": true}
		]
	}
}
```

Example response:

```json
{
	"jsonrpc": "2.0",
	"id": 10,
	"result": {
		"ack": true,
		"ops": [
			{"dir": "write", "written": 1, "acked_bytes": [true]},
			{"dir": "read", "data": [111, 56, 0], "nack_last": true}
		],
		"timing": {"clock_hz": 100000, "repeated_start": true}
	}
}
```

## Device-tree-like hookup style

`peripherals/peripherals.schema.json` now supports DT-inspired fields:
- top-level `buses[]` registry (`id`, `kind`, `controller`, `clock_hz`, pins/electrical)
- per-device `compatible`, `status`, `bus_ref`, `reg`, optional GPIO/interrupt properties
- legacy `bus` object remains supported for backward compatibility.

This keeps the model close to how real ESP32-S3 boards wire peripherals on I2C/SPI/UART buses.

### Example configuration
- `peripherals/peripherals.example.json` now references these Python simulators directly via:
	- `exec: "python3"`
	- `args: ["./device-sims/<sim>.py", ...]`
