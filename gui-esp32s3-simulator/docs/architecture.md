# GUI ESP32-S3 Simulator Architecture (Simple MVP)

## Target stack
- Linux
- Qt6 Widgets frontend
- QEMU ESP32-S3 backend runtime

## MVP UI requirements implemented
1. Basic Serial panel (default UART0)
2. Processor Status panel
   - PC
   - Scalar registers
   - Vector registers
   - Memory inspect table
   - Live refresh mode
   - Pause / Continue / Step PC
   - Breakpoint add / clear
3. Basic Control panel
   - Reset
   - Boot mode
   - Firmware file loading
4. Dedicated Debug panel
   - GDB enable/disable
   - GDB port and wait-for-attach
   - Pause / Continue / Step PC
   - Breakpoint add / clear

## Runtime split
- Frontend (Qt): view and user interaction
- Backend controller: QEMU process control + QMP/monitor integration point
- Peripheral hooks: external device simulators described by JSON config

## GDB support review status
- Codebase support is present for Xtensa/ESP32-S3 gdbstub:
   - Xtensa target includes gdbstub source and hooks for read/write registers.
   - ESP32-S3 core includes a dedicated gdb register map configuration.
   - Built qemu-system-xtensa exposes runtime options: -s, -S, -gdb.
- Practical note from local smoke run:
   - Launching esp32s3 without a full firmware/flash setup currently segfaults in this environment.
   - This crash reproduces both with and without -gdb flags, so it is not a gdbstub-specific failure.

## GUI GDB integration status
- Implemented in backend launch path:
   - Optional `-gdb tcp::<port>`
   - Optional `-S` for wait mode
- Exposed in Debug tab for user configuration at runtime.
- One-click flow available:
   - Pick firmware in Debug tab
   - Start QEMU with gdbstub enabled
   - Show exact attach command string to user

## Next integration steps
- Serial panel is wired to QEMU stdio (UART0 simple path)
- Harden QMP snapshot polling and command error handling
- Improve register parser fidelity for Xtensa vector naming/format
- Implement real strap-level boot mode wiring
- Parse peripherals.example.json and spawn simulator processes
- Route bus transactions between QEMU and external simulators
