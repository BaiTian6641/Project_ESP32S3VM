#!/usr/bin/env python3
"""SPI NOR Flash simulator -- models Winbond W25Q128JV behaviour.

Implements the standard SPI NOR flash command set used by ESP-IDF
spi_flash drivers, ESP32-S3 bootloader, and Arduino SPI flash libraries.

Key features:
  - Status Register 1/2/3 with WIP (Write-In-Progress) busy simulation
  - Sector Erase (0x20), 32K Block Erase (0x52), 64K Block Erase (0xD8), Chip Erase (0xC7/0x60)
  - Page Program (0x02) with 256-byte page boundary wrapping & AND-only semantics
  - Write Enable / Disable (0x06 / 0x04) with auto-clear after program/erase
  - Standard Read (0x03), Fast Read (0x0B), Dual (0x3B), Quad (0x6B) output reads
  - JEDEC ID (0x9F), Manufacturer/Device ID (0x90), Unique ID (0x4B)
  - Power Down (0xB9) / Release Power Down (0xAB)
  - SFDP Read (0x5A) -- returns basic JEDEC header for driver discovery
  - Busy timing simulation: page program ~0.7ms, sector erase ~50ms, etc.
"""
import argparse
import struct
from typing import Any, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class SpiFlashSimulator:
    """Models Winbond W25Q128JV (128Mbit / 16MB) NOR flash per datasheet."""

    # JEDEC ID: Winbond W25Q128JV
    MANUFACTURER_ID = 0xEF
    MEMORY_TYPE = 0x40
    CAPACITY_ID = 0x18   # 128Mbit
    DEVICE_ID = 0x17     # W25Q128

    # Geometry
    PAGE_SIZE = 256
    SECTOR_SIZE = 4096        # 4KB
    BLOCK_32K = 32768         # 32KB
    BLOCK_64K = 65536         # 64KB

    # Status register bit masks
    SR1_BUSY = 0x01           # WIP - Write In Progress (read-only)
    SR1_WEL = 0x02            # Write Enable Latch (read-only)
    SR1_BP0 = 0x04            # Block Protect 0
    SR1_BP1 = 0x08            # Block Protect 1
    SR1_BP2 = 0x10            # Block Protect 2
    SR1_TB = 0x20             # Top/Bottom protect
    SR1_SEC = 0x40            # Sector protect
    SR1_SRP0 = 0x80           # Status Register Protect 0

    SR2_SRP1 = 0x01
    SR2_QE = 0x02             # Quad Enable
    SR2_LB1 = 0x08
    SR2_LB2 = 0x10
    SR2_LB3 = 0x20
    SR2_CMP = 0x40
    SR2_SUS = 0x80            # Suspend Status (read-only)

    SR3_WPS = 0x04
    SR3_DRV0 = 0x20
    SR3_DRV1 = 0x40

    # Typical busy durations (ms) -- from W25Q128JV datasheet
    BUSY_PAGE_PROGRAM_MS = 1       # typ 0.7ms, max 3ms
    BUSY_SECTOR_ERASE_MS = 50     # typ 45ms, max 400ms
    BUSY_BLOCK32_ERASE_MS = 120   # typ 120ms, max 1600ms
    BUSY_BLOCK64_ERASE_MS = 150   # typ 150ms, max 2000ms
    BUSY_CHIP_ERASE_MS = 20000    # typ 20s, max 100s

    # SFDP basic header (minimal for JEDEC discovery)
    _SFDP_HEADER = bytes([
        0x53, 0x46, 0x44, 0x50,  # "SFDP" signature
        0x06,                     # Minor revision
        0x01,                     # Major revision
        0x01,                     # Number of parameter headers - 1
        0xFF,                     # Reserved
        # Parameter Header 0 (JEDEC Basic Flash Parameter)
        0x00,                     # JEDEC ID LSB
        0x06,                     # Minor revision
        0x01,                     # Major revision
        0x10,                     # Length in DWORDs (16)
        0x30, 0x00, 0x00,        # Parameter table pointer
        0xFF,                     # Reserved
    ])

    def __init__(self, size_bytes: int = 16 * 1024 * 1024) -> None:
        self.size_bytes = max(4096, int(size_bytes))
        self.mem = bytearray([0xFF] * self.size_bytes)

        # Status registers
        self.sr1: int = 0x00  # BUSY and WEL are dynamic
        self.sr2: int = 0x00
        self.sr3: int = 0x00

        # Internal state
        self._wel: bool = False      # Write Enable Latch
        self._busy: bool = False     # Write-In-Progress
        self._busy_until_ms: int = 0
        self._power_down: bool = False
        self._suspended: bool = False

        # Unique 64-bit ID
        self._unique_id = struct.pack(">Q", 0xDEADBEEF_CAFEBABE)

        # SPI bus settings tracking
        self.last_mode = "single"
        self.last_freq_hz = 20_000_000

    # -- JSON-RPC handler --
    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "spi.flash", "id": params.get("id", "spiflash")},
                "panel": {
                    "kind": "memory.spi",
                    "title": "SPI Flash",
                    "description": "NOR flash model controls and status indicators.",
                    "script": {
                        "enabled": True,
                        "event_method": "panel_event",
                        "state_method": "panel_state",
                        "runtime_state_key": "panel_runtime",
                        "fallback_set_parameter": False,
                    },
                    "metrics": [
                        {"label": "Busy", "state_path": "busy", "true_text": "YES", "false_text": "NO"},
                        {"label": "Power Down", "state_path": "power_down", "true_text": "YES", "false_text": "NO"},
                        {"label": "Mode", "state_path": "last_mode"},
                        {"label": "Clock", "state_path": "last_freq_hz", "unit": "Hz"}
                    ],
                    "scripts": [
                        {"title": "Status Registers", "state_path": "status_registers", "min_height": 120}
                    ],
                    "controls": [
                        {"name": "erase_chip", "label": "Erase Chip", "type": "action",
                         "section": "Operations", "description": "Erase full flash content to 0xFF."},
                        {"name": "write_enable_latch", "label": "WEL", "type": "bool", "writable": False,
                         "section": "Status", "description": "Write Enable Latch status bit."},
                        {"name": "busy", "label": "Busy", "type": "bool", "writable": False,
                         "section": "Status", "description": "Write/erase in progress bit (WIP)."},
                        {"name": "power_down", "label": "Power Down", "type": "bool", "writable": False,
                         "section": "Status", "description": "Deep power-down status."},
                    ],
                },
                "transports": ["spi"],
                "spi": {
                    "modes": ["single", "dual", "quad"],
                    "max_hz": 133_000_000,
                    "cpol": 0,
                    "cpha": 0,
                },
            }

        if method == "get_state":
            return self._state()

        if method == "set_parameter":
            name = str(params.get("name", ""))
            if name == "erase_chip":
                self.mem[:] = b"\xFF" * self.size_bytes
                self._wel = False
                self._busy = False
                return {"ok": True, "state": self._state()}
            raise ValueError(f"Unsupported parameter: {name}")

        if method == "panel_event":
            return self._panel_event(params)

        if method == "panel_state":
            return {"panel_state": self._panel_runtime(params.get("state", {}))}

        if method == "spi_transfer":
            return self._spi_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        """Check if busy period has elapsed."""
        if self._busy and monotonic_ms() >= self._busy_until_ms:
            self._busy = False

    # -- Build status register 1 (dynamic bits merged) --
    def _read_sr1(self) -> int:
        self.tick()
        val = self.sr1 & 0xFC  # Mask out BUSY and WEL bits
        if self._busy:
            val |= self.SR1_BUSY
        if self._wel:
            val |= self.SR1_WEL
        return val

    def _read_sr2(self) -> int:
        val = self.sr2 & 0x7F  # Mask out SUS bit
        if self._suspended:
            val |= self.SR2_SUS
        return val

    # -- State payload --
    def _state(self) -> Dict[str, Any]:
        self.tick()
        return {
            "size_bytes": self.size_bytes,
            "write_enable_latch": self._wel,
            "busy": self._busy,
            "power_down": self._power_down,
            "suspended": self._suspended,
            "status_registers": {
                "sr1": f"0x{self._read_sr1():02X}",
                "sr2": f"0x{self._read_sr2():02X}",
                "sr3": f"0x{self.sr3:02X}",
            },
            "quad_enabled": bool(self.sr2 & self.SR2_QE),
            "last_mode": self.last_mode,
            "last_freq_hz": self.last_freq_hz,
        }

    def _panel_runtime(self, _state_from_gui: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        state = self._state()
        return {
            "summary": {
                "busy": state["busy"],
                "power_down": state["power_down"],
                "mode": state["last_mode"],
                "frequency_hz": state["last_freq_hz"],
            },
            "status_registers": state["status_registers"],
            "script_log": (
                f"mode={state['last_mode']} freq={state['last_freq_hz']}Hz "
                f"busy={int(state['busy'])} pd={int(state['power_down'])}"
            ),
        }

    def _panel_event(self, params: Dict[str, Any]) -> Dict[str, Any]:
        event = str(params.get("event", "")).strip().lower()
        control = str(params.get("control", "")).strip()

        if event in ("action", "set_control") and control == "erase_chip":
            self.mem[:] = b"\xFF" * self.size_bytes
            self._wel = False
            self._busy = False

        return {
            "ok": True,
            "panel_state": self._panel_runtime(),
            "state_patch": self._state(),
        }

    # -- Set busy for a given duration --
    def _set_busy(self, duration_ms: int) -> None:
        self._busy = True
        self._busy_until_ms = monotonic_ms() + duration_ms

    # ============================================================
    #  SPI transfer handler
    # ============================================================
    def _spi_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        # Track bus mode
        mode = str(params.get("mode", "single")).strip().lower()
        if mode not in ("single", "dual", "quad"):
            mode = "single"
        self.last_mode = mode
        freq_hz = int(params.get("frequency_hz", 20_000_000))
        self.last_freq_hz = max(1000, freq_hz)

        raw_tx = params.get("tx", [])
        if not isinstance(raw_tx, list):
            return {
                "ok": False,
                "error": "invalid_tx",
                "rx": [],
                "timing": {"mode": mode, "frequency_hz": self.last_freq_hz},
            }
        tx = [int(x) & 0xFF for x in raw_tx]
        rx_len = max(0, int(params.get("rx_len", 0)))

        if not tx:
            return self._ok_response([], rx_len, mode)

        # Auto-finish any expired busy
        self.tick()

        cmd = tx[0]
        rx = self._execute_command(cmd, tx, rx_len)

        return self._ok_response(rx, rx_len, mode)

    def _ok_response(self, rx: List[int], rx_len: int, mode: str) -> Dict[str, Any]:
        # Pad or trim to requested rx_len
        if len(rx) < rx_len:
            rx.extend([0x00] * (rx_len - len(rx)))
        elif len(rx) > rx_len:
            rx = rx[:rx_len]
        return {
            "ok": True,
            "rx": rx,
            "timing": {"mode": mode, "frequency_hz": self.last_freq_hz},
        }

    def _execute_command(self, cmd: int, tx: List[int], rx_len: int) -> List[int]:
        # ---- Release from Power Down / Device ID (0xAB) ----
        if cmd == 0xAB:
            self._power_down = False
            # Returns device ID after 3 dummy bytes
            if rx_len > 0:
                return [self.DEVICE_ID] * rx_len
            return []

        # ---- Power Down (0xB9) ----
        if cmd == 0xB9:
            self._power_down = True
            return []

        # If in power down, reject all commands except 0xAB
        if self._power_down:
            return [0xFF] * rx_len

        # ---- Read Status Register 1 (0x05) ----
        if cmd == 0x05:
            sr1 = self._read_sr1()
            return [sr1] * max(1, rx_len)

        # ---- Read Status Register 2 (0x35) ----
        if cmd == 0x35:
            sr2 = self._read_sr2()
            return [sr2] * max(1, rx_len)

        # ---- Read Status Register 3 (0x15) ----
        if cmd == 0x15:
            return [self.sr3] * max(1, rx_len)

        # ---- Write Enable (0x06) ----
        if cmd == 0x06:
            if not self._busy:
                self._wel = True
            return []

        # ---- Write Disable (0x04) ----
        if cmd == 0x04:
            self._wel = False
            return []

        # ---- Volatile SR Write Enable (0x50) ----
        if cmd == 0x50:
            # Allows writing status registers without WEL requirement
            # For simplicity, just set WEL
            self._wel = True
            return []

        # ---- Write Status Register 1 (0x01) ----
        if cmd == 0x01:
            if self._busy:
                return []
            if self._wel:
                if len(tx) >= 2:
                    self.sr1 = (tx[1] & 0xFC)  # BUSY/WEL are read-only
                if len(tx) >= 3:
                    self.sr2 = tx[2]
                self._wel = False
            return []

        # ---- Write Status Register 2 (0x31) ----
        if cmd == 0x31:
            if not self._busy and self._wel and len(tx) >= 2:
                self.sr2 = tx[1]
                self._wel = False
            return []

        # ---- Write Status Register 3 (0x11) ----
        if cmd == 0x11:
            if not self._busy and self._wel and len(tx) >= 2:
                self.sr3 = tx[1]
                self._wel = False
            return []

        # ---- JEDEC ID (0x9F) ----
        if cmd == 0x9F:
            jedec = [self.MANUFACTURER_ID, self.MEMORY_TYPE, self.CAPACITY_ID]
            return (jedec * ((rx_len // 3) + 1))[:rx_len] if rx_len else jedec

        # ---- Manufacturer / Device ID (0x90) ----
        if cmd == 0x90:
            # 3 dummy bytes follow cmd, then returns Mfr, DevID
            result = [self.MANUFACTURER_ID, self.DEVICE_ID]
            return (result * ((rx_len // 2) + 1))[:rx_len] if rx_len else result

        # ---- Unique ID (0x4B) ----
        if cmd == 0x4B:
            # 4 dummy bytes after cmd, then 8 bytes of unique ID
            uid = list(self._unique_id)
            return uid[:rx_len] if rx_len else uid

        # ---- SFDP Read (0x5A) ----
        if cmd == 0x5A:
            # tx: [0x5A, A23, A16, A8, dummy]
            addr = 0
            if len(tx) >= 4:
                addr = ((tx[1] << 16) | (tx[2] << 8) | tx[3]) & 0xFFFF
            sfdp = self._SFDP_HEADER
            data = []
            for i in range(rx_len):
                idx = (addr + i) % len(sfdp) if (addr + i) < len(sfdp) else 0xFF
                data.append(sfdp[addr + i] if (addr + i) < len(sfdp) else 0xFF)
            return data

        # ---- Standard Read (0x03) ----
        if cmd == 0x03:
            if self._busy or len(tx) < 4:
                return [0xFF] * rx_len
            addr = self._addr24(tx)
            return self._read_mem(addr, rx_len)

        # ---- Fast Read (0x0B) ----
        if cmd == 0x0B:
            if self._busy or len(tx) < 5:  # cmd + 3 addr + 1 dummy
                return [0xFF] * rx_len
            addr = self._addr24(tx)
            return self._read_mem(addr, rx_len)

        # ---- Fast Read Dual Output (0x3B) ----
        if cmd == 0x3B:
            if self._busy or len(tx) < 5:
                return [0xFF] * rx_len
            addr = self._addr24(tx)
            return self._read_mem(addr, rx_len)

        # ---- Fast Read Quad Output (0x6B) ----
        if cmd == 0x6B:
            if self._busy or len(tx) < 5:
                return [0xFF] * rx_len
            addr = self._addr24(tx)
            return self._read_mem(addr, rx_len)

        # ---- Page Program (0x02) ----
        if cmd == 0x02:
            if self._busy:
                return []
            if len(tx) < 4:
                return []
            if not self._wel:
                return []
            addr = self._addr24(tx)
            payload = tx[4:]
            self._page_program(addr, payload)
            self._wel = False
            self._set_busy(self.BUSY_PAGE_PROGRAM_MS)
            return []

        # ---- Sector Erase 4KB (0x20) ----
        if cmd == 0x20:
            return self._erase_region(tx, self.SECTOR_SIZE, self.BUSY_SECTOR_ERASE_MS)

        # ---- Block Erase 32KB (0x52) ----
        if cmd == 0x52:
            return self._erase_region(tx, self.BLOCK_32K, self.BUSY_BLOCK32_ERASE_MS)

        # ---- Block Erase 64KB (0xD8) ----
        if cmd == 0xD8:
            return self._erase_region(tx, self.BLOCK_64K, self.BUSY_BLOCK64_ERASE_MS)

        # ---- Chip Erase (0xC7 / 0x60) ----
        if cmd in (0xC7, 0x60):
            if self._busy or not self._wel:
                return []
            self.mem[:] = b"\xFF" * self.size_bytes
            self._wel = False
            self._set_busy(self.BUSY_CHIP_ERASE_MS)
            return []

        # ---- Erase / Program Suspend (0x75) ----
        if cmd == 0x75:
            if self._busy:
                self._busy = False
                self._suspended = True
            return []

        # ---- Erase / Program Resume (0x7A) ----
        if cmd == 0x7A:
            if self._suspended:
                self._suspended = False
                # Re-enter busy (simplified: finish immediately)
            return []

        # ---- Enable Reset (0x66) ----
        if cmd == 0x66:
            self._reset_enable = True
            return []

        # ---- Reset Device (0x99) ----
        if cmd == 0x99:
            if getattr(self, '_reset_enable', False):
                self._software_reset()
                self._reset_enable = False
            return []

        # ---- Enter QPI Mode (0x38) -- accept silently ----
        if cmd == 0x38:
            return []

        # ---- Exit QPI Mode (0xFF) -- accept silently ----
        if cmd == 0xFF:
            return []

        # Unknown command -- ignore
        return []


    # -- Memory operations --
    def _read_mem(self, addr: int, length: int) -> List[int]:
        result = []
        for i in range(length):
            result.append(self.mem[(addr + i) % self.size_bytes])
        return result

    def _page_program(self, addr: int, data: List[int]) -> None:
        """Page Program with 256-byte page boundary wrapping.

        Per datasheet: if address + data extends past page boundary,
        it wraps to the beginning of the same page (AND-only semantics).
        """
        page_base = addr & ~(self.PAGE_SIZE - 1)  # Align to page start
        offset = addr & (self.PAGE_SIZE - 1)       # Offset within page

        for i, byte in enumerate(data):
            write_offset = (offset + i) % self.PAGE_SIZE
            mem_addr = page_base + write_offset
            if mem_addr < self.size_bytes:
                self.mem[mem_addr] &= byte  # AND-only: can't set 0 bits back to 1

    def _erase_region(self, tx: List[int], region_size: int, busy_ms: int) -> List[int]:
        if self._busy or not self._wel or len(tx) < 4:
            return []
        addr = self._addr24(tx)
        base = (addr // region_size) * region_size  # Align to region boundary
        end = min(base + region_size, self.size_bytes)
        self.mem[base:end] = b"\xFF" * (end - base)
        self._wel = False
        self._set_busy(busy_ms)
        return []

    def _addr24(self, tx: List[int]) -> int:
        """Extract 24-bit address from tx[1:4]."""
        return ((tx[1] << 16) | (tx[2] << 8) | tx[3]) % self.size_bytes

    def _software_reset(self) -> None:
        """Reset status registers to defaults, preserve memory."""
        self.sr1 = 0x00
        self.sr2 = 0x00
        self.sr3 = 0x00
        self._wel = False
        self._busy = False
        self._power_down = False
        self._suspended = False


SpiMemorySimulator = SpiFlashSimulator


def main() -> None:
    parser = argparse.ArgumentParser(
        description="SPI NOR flash simulator (JSON-RPC over stdio)")
    parser.add_argument("--size", type=int, default=16 * 1024 * 1024,
                        help="Flash size in bytes (default 16MB / W25Q128)")
    args, _unknown = parser.parse_known_args()

    global server
    server = JsonRpcStdioServer()
    sim = SpiFlashSimulator(size_bytes=args.size)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
