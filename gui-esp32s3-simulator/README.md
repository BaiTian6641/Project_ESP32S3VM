# GUI ESP32-S3 Simulator

Simple Qt-based GUI simulator project for Linux.

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

## Virtual serial device (Linux)
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
- Backend controller with simple QEMU launcher + UART0 stdio streaming
- Peripheral hook format:
	- Schema: peripherals/peripherals.schema.json
	- Example: peripherals/peripherals.example.json

## Current runtime behavior (simple)
- `Load Firmware` starts/restarts QEMU using:
	- ELF (`.elf`): `-M esp32s3[,boot-mode=...][,mac=..][,chip-revision=..] -display none -monitor none -serial stdio -kernel <firmware>`
	- BIN (`.bin`): creates a temporary SPI flash image and starts with
	  `-drive file=/tmp/esp32s3_gui_flash.bin,if=mtd,format=raw`
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
- Uses QMP over UNIX socket (`-qmp unix:/tmp/esp32s3_gui_qmp.sock,server=on,wait=off`)
- Optional gdbstub when enabled in Debug tab:
	- `-gdb tcp::<port>`
	- plus `-S` when wait-for-attach is checked
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
