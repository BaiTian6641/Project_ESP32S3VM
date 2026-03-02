#!/usr/bin/env python3
"""SSD1306 OLED display simulator -- models real hardware behaviour.

Supports the full command set needed by common Arduino libraries
(Adafruit_SSD1306, u8g2, etc.) including:
  - All 3 memory addressing modes (page / horizontal / vertical)
  - Complete hardware-configuration & timing commands
  - Charge-pump setting (0x8D) -- critical for real OLED modules
  - Proper I2C control-byte parsing (Co and D/C# bits per datasheet 8.1.5)
  - Scrolling command acceptance (parameters stored, activation tracked)
  - Status register read (D[6] = display ON/OFF)
"""
import argparse
from typing import Any, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class Ssd1306Simulator:
    """Models real SSD1306 128x64 OLED controller per Solomon Systech datasheet Rev 1.1."""

    # Addressing modes (command 0x20)
    ADDR_MODE_HORIZONTAL = 0x00
    ADDR_MODE_VERTICAL = 0x01
    ADDR_MODE_PAGE = 0x02

    # Multi-byte command -> number of parameter bytes expected.
    _CMD_PARAM_COUNT: Dict[int, int] = {
        0x20: 1,   # Set Memory Addressing Mode
        0x21: 2,   # Set Column Address (start, end)
        0x22: 2,   # Set Page Address   (start, end)
        0x26: 6,   # Right Horizontal Scroll Setup
        0x27: 6,   # Left  Horizontal Scroll Setup
        0x29: 5,   # Vertical + Right Horizontal Scroll
        0x2A: 5,   # Vertical + Left  Horizontal Scroll
        0x81: 1,   # Set Contrast Control
        0x8D: 1,   # Charge Pump Setting
        0xA3: 2,   # Set Vertical Scroll Area
        0xA8: 1,   # Set Multiplex Ratio
        0xD3: 1,   # Set Display Offset
        0xD5: 1,   # Set Display Clock Divide Ratio / Oscillator Frequency
        0xD9: 1,   # Set Pre-charge Period
        0xDA: 1,   # Set COM Pins Hardware Configuration
        0xDB: 1,   # Set VCOMH Deselect Level
    }

    def __init__(self, width: int = 128, height: int = 64) -> None:
        self.width = width
        self.height = height
        self.pages = height // 8

        # GDDRAM: <width> columns x <pages> pages
        self.gddram = bytearray(width * self.pages)

        # -- Display state registers (reset defaults from datasheet) --
        self.display_on = False            # AEh = OFF (RESET), AFh = ON
        self.inverted = False              # A6h = normal (RESET), A7h = inverse
        self.entire_display_on = False     # A4h = follow RAM (RESET), A5h = all-on
        self.contrast = 0x7F               # 81h, default 7Fh

        # -- Addressing mode --
        self.addr_mode = self.ADDR_MODE_PAGE   # 20h, default 10b (page mode)

        # Page-addressing-mode column pointers (00h-0Fh / 10h-1Fh)
        self.col_lower = 0x00
        self.col_upper = 0x00

        # Horizontal / Vertical addressing range (21h, 22h)
        self.col_start = 0                 # RESET = 0
        self.col_end = 127                 # RESET = 127
        self.page_start = 0               # RESET = 0
        self.page_end = 7                 # RESET = 7

        # Current write pointers
        self.column = 0
        self.page = 0

        # -- Hardware configuration registers --
        self.display_start_line = 0        # 40h-7Fh (RESET = 0)
        self.segment_remap = False          # A0h = normal (RESET), A1h = remapped
        self.multiplex_ratio = 63          # A8h (RESET = 63 -> 64 MUX)
        self.com_scan_remapped = False      # C0h = normal (RESET), C8h = remapped
        self.display_offset = 0            # D3h (RESET = 0)
        self.com_pins_config = 0x12        # DAh (RESET = 0x12)

        # -- Timing registers --
        self.clock_divide_ratio = 0x80     # D5h (RESET = 0x80)
        self.precharge_period = 0x22       # D9h (RESET = 0x22)
        self.vcomh_deselect = 0x20         # DBh (RESET = 0x20)

        # -- Charge pump --
        self.charge_pump_enabled = False   # 8Dh  14h = enable, 10h = disable

        # -- Scrolling state --
        self.scrolling_active = False
        self.scroll_right = True
        self.scroll_start_page = 0
        self.scroll_end_page = 7
        self.scroll_speed = 0
        self.scroll_v_offset = 0
        self.scroll_area_top = 0
        self.scroll_area_rows = 64

        # -- Command parser state (multi-byte commands) --
        self._pending_cmd: Optional[int] = None
        self._pending_params: List[int] = []
        self._expected_params = 0

    # -- JSON-RPC request handler --
    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "ssd1306", "id": params.get("id", "ssd1306")},
                "panel": {
                    "kind": "display",
                    "width": self.width,
                    "height": self.height,
                    "pixel_format": "mono1",
                    "controls": [
                        {"name": "display_on", "type": "bool", "writable": True},
                        {"name": "inverted", "type": "bool", "writable": True},
                        {"name": "contrast", "type": "int", "min": 0, "max": 255, "writable": True},
                    ],
                },
                "transports": ["i2c"],
                "i2c": {"addresses": ["0x3c", "0x3d"]},
            }

        if method == "get_state":
            return self._state_payload(include_buffer=True)

        if method == "set_parameter":
            name = params.get("name")
            value = params.get("value")
            if name == "display_on":
                self.display_on = bool(value)
            elif name == "inverted":
                self.inverted = bool(value)
            elif name == "contrast":
                self.contrast = max(0, min(255, int(value)))
            else:
                raise ValueError(f"Unsupported parameter: {name}")
            return {"ok": True, "state": self._state_payload(include_buffer=False)}

        if method == "i2c_write":
            addr = self._parse_addr(params.get("addr", "0x3c"))
            data = params.get("data", [])
            if addr not in (0x3C, 0x3D):
                return {"ack": False}
            self._handle_i2c_payload([int(x) & 0xFF for x in data])
            return {"ack": True, "written": len(data)}

        if method == "i2c_read":
            # SSD1306 I2C mode does not support data reads (datasheet 9.1);
            # return the status register (D[6]: 1 = display OFF, 0 = ON).
            req_len = max(0, int(params.get("len", 0)))
            status = 0x00 if self.display_on else 0x40
            return {"ack": True, "data": [status] * req_len}

        if method == "i2c_transfer":
            return self._i2c_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        pass

    # -- I2C transfer (multi-op) --
    def _i2c_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        addr = self._parse_addr(params.get("address", params.get("addr", "0x3c")))
        if addr not in (0x3C, 0x3D):
            return {"ack": False, "ops": [], "error": "address_nack"}

        ops = params.get("ops", [])
        out_ops: List[Dict[str, Any]] = []
        for op in ops:
            direction = str(op.get("dir", "")).lower()
            if direction == "write":
                data = [int(x) & 0xFF for x in op.get("data", [])]
                self._handle_i2c_payload(data)
                out_ops.append({
                    "dir": "write",
                    "written": len(data),
                    "acked_bytes": [True] * len(data),
                })
            elif direction == "read":
                req_len = max(0, int(op.get("len", 0)))
                status = 0x00 if self.display_on else 0x40
                out_ops.append({"dir": "read", "data": [status] * req_len})
            else:
                out_ops.append({"dir": direction, "error": "unsupported_op"})

        timing = {
            "clock_hz": int(params.get("clock_hz", 400000)),
            "repeated_start": bool(params.get("repeated_start", False)),
        }
        return {"ack": True, "ops": out_ops, "timing": timing}

    # -- State payload for GUI --
    def _state_payload(self, include_buffer: bool) -> Dict[str, Any]:
        state: Dict[str, Any] = {
            "display_on": self.display_on,
            "inverted": self.inverted,
            "entire_display_on": self.entire_display_on,
            "contrast": self.contrast,
            "addr_mode": self.addr_mode,
            "segment_remap": self.segment_remap,
            "com_scan_remapped": self.com_scan_remapped,
            "display_start_line": self.display_start_line,
            "multiplex_ratio": self.multiplex_ratio,
            "display_offset": self.display_offset,
            "charge_pump_enabled": self.charge_pump_enabled,
            "scrolling_active": self.scrolling_active,
            "cursor": {"page": self.page, "column": self.column},
            "geometry": {"width": self.width, "height": self.height},
        }
        if include_buffer:
            state["buffer"] = {
                "encoding": "u8",
                "layout": "page-major",
                "data": list(self.gddram),
            }
        return state

    def _emit_frame_update(self) -> None:
        server.notify(
            "frame_update",
            {
                "width": self.width,
                "height": self.height,
                "encoding": "u8",
                "layout": "page-major",
                "data": list(self.gddram),
                "ts_ms": monotonic_ms(),
            },
        )

    # ==================================================================
    #  I2C payload parser
    #
    #  SSD1306 I2C write protocol (datasheet 8.1.5):
    #    [slave_addr+W] [control] [byte] [control if Co=1] [byte] ...
    #
    #  Control byte bits:
    #    bit 7 = Co (continuation): 0 -> all remaining bytes share same D/C#
    #                                1 -> only next byte, then new control
    #    bit 6 = D/C#:             0 -> command byte,  1 -> data (GDDRAM)
    #    bits 5-0 = 0
    # ==================================================================

    def _handle_i2c_payload(self, data: List[int]) -> None:
        if not data:
            return

        idx = 0
        dirty = False

        while idx < len(data):
            control = data[idx]
            idx += 1

            co = (control >> 7) & 1   # continuation bit
            dc = (control >> 6) & 1   # data / command select

            if co == 0:
                # Remaining bytes are ALL the same type.
                if dc == 0:
                    # -- command stream --
                    while idx < len(data):
                        idx = self._process_command_byte(data, idx)
                else:
                    # -- data stream -> GDDRAM --
                    while idx < len(data):
                        self._write_gddram_byte(data[idx])
                        dirty = True
                        idx += 1
                break  # Co=0: no more control bytes after this batch
            else:
                # Co=1: exactly one byte, then expect another control byte.
                if idx >= len(data):
                    break
                if dc == 0:
                    idx = self._process_command_byte(data, idx)
                else:
                    self._write_gddram_byte(data[idx])
                    dirty = True
                    idx += 1

        if dirty:
            self._emit_frame_update()

    # -- Command byte processor (handles multi-byte param collection) --
    def _process_command_byte(self, data: List[int], idx: int) -> int:
        if idx >= len(data):
            return idx

        byte = data[idx]
        idx += 1

        # Collecting parameters for a multi-byte command?
        if self._pending_cmd is not None:
            self._pending_params.append(byte)
            if len(self._pending_params) >= self._expected_params:
                self._execute_multi_byte_cmd(self._pending_cmd, self._pending_params)
                self._pending_cmd = None
                self._pending_params = []
                self._expected_params = 0
            return idx

        # Is this the first byte of a known multi-byte command?
        if byte in self._CMD_PARAM_COUNT:
            self._pending_cmd = byte
            self._pending_params = []
            self._expected_params = self._CMD_PARAM_COUNT[byte]
            return idx

        # Otherwise it is a single-byte command.
        self._execute_single_byte_cmd(byte)
        return idx

    # -- Single-byte commands --
    def _execute_single_byte_cmd(self, cmd: int) -> None:

        # Set Lower Column Start Address for Page Addressing Mode (00h-0Fh)
        if 0x00 <= cmd <= 0x0F:
            self.col_lower = cmd & 0x0F
            if self.addr_mode == self.ADDR_MODE_PAGE:
                self.column = (self.col_upper << 4) | self.col_lower
            return

        # Set Higher Column Start Address for Page Addressing Mode (10h-1Fh)
        if 0x10 <= cmd <= 0x1F:
            self.col_upper = cmd & 0x0F
            if self.addr_mode == self.ADDR_MODE_PAGE:
                self.column = (self.col_upper << 4) | self.col_lower
            return

        # Set Display Start Line (40h-7Fh)
        if 0x40 <= cmd <= 0x7F:
            self.display_start_line = cmd & 0x3F
            return

        # Set Segment Re-map (A0h / A1h)
        if cmd == 0xA0:
            self.segment_remap = False
            return
        if cmd == 0xA1:
            self.segment_remap = True
            return

        # Entire Display ON (A4h / A5h)
        if cmd == 0xA4:
            self.entire_display_on = False
            return
        if cmd == 0xA5:
            self.entire_display_on = True
            return

        # Set Normal / Inverse Display (A6h / A7h)
        if cmd == 0xA6:
            self.inverted = False
            return
        if cmd == 0xA7:
            self.inverted = True
            return

        # Display OFF / ON (AEh / AFh)
        if cmd == 0xAE:
            self.display_on = False
            return
        if cmd == 0xAF:
            self.display_on = True
            return

        # Set Page Start Address for Page Addressing Mode (B0h-B7h)
        if 0xB0 <= cmd <= 0xB7:
            if self.addr_mode == self.ADDR_MODE_PAGE:
                self.page = cmd & 0x07
            return

        # Set COM Output Scan Direction (C0h / C8h)
        if cmd == 0xC0:
            self.com_scan_remapped = False
            return
        if cmd == 0xC8:
            self.com_scan_remapped = True
            return

        # Deactivate Scroll (2Eh)
        if cmd == 0x2E:
            self.scrolling_active = False
            return

        # Activate Scroll (2Fh)
        if cmd == 0x2F:
            self.scrolling_active = True
            return

        # NOP (E3h)
        if cmd == 0xE3:
            return

    # -- Multi-byte commands --
    def _execute_multi_byte_cmd(self, cmd: int, params: List[int]) -> None:

        # Set Memory Addressing Mode (20h)
        if cmd == 0x20:
            mode = params[0] & 0x03
            if mode <= 0x02:
                self.addr_mode = mode
                if mode in (self.ADDR_MODE_HORIZONTAL, self.ADDR_MODE_VERTICAL):
                    self.column = self.col_start
                    self.page = self.page_start
            return

        # Set Column Address (21h) -- horizontal / vertical mode
        if cmd == 0x21:
            self.col_start = params[0] & 0x7F
            self.col_end = params[1] & 0x7F
            self.column = self.col_start
            return

        # Set Page Address (22h) -- horizontal / vertical mode
        if cmd == 0x22:
            self.page_start = params[0] & 0x07
            self.page_end = params[1] & 0x07
            self.page = self.page_start
            return

        # Set Contrast Control (81h)
        if cmd == 0x81:
            self.contrast = params[0] & 0xFF
            return

        # Charge Pump Setting (8Dh)
        if cmd == 0x8D:
            self.charge_pump_enabled = (params[0] & 0x04) != 0
            return

        # Set Multiplex Ratio (A8h)
        if cmd == 0xA8:
            val = params[0] & 0x3F
            if val >= 15:            # 0-14 are invalid entries
                self.multiplex_ratio = val
            return

        # Set Vertical Scroll Area (A3h)
        if cmd == 0xA3:
            self.scroll_area_top = params[0] & 0x3F
            self.scroll_area_rows = params[1] & 0x7F
            return

        # Set Display Offset (D3h)
        if cmd == 0xD3:
            self.display_offset = params[0] & 0x3F
            return

        # Set Display Clock Divide Ratio / Oscillator Frequency (D5h)
        if cmd == 0xD5:
            self.clock_divide_ratio = params[0] & 0xFF
            return

        # Set Pre-charge Period (D9h)
        if cmd == 0xD9:
            self.precharge_period = params[0] & 0xFF
            return

        # Set COM Pins Hardware Configuration (DAh)
        if cmd == 0xDA:
            self.com_pins_config = params[0] & 0xFF
            return

        # Set VCOMH Deselect Level (DBh)
        if cmd == 0xDB:
            self.vcomh_deselect = params[0] & 0xFF
            return

        # -- Scroll setup commands --
        # Right / Left Horizontal Scroll (26h / 27h)
        if cmd in (0x26, 0x27):
            self.scroll_right = (cmd == 0x26)
            if len(params) >= 6:
                self.scroll_start_page = params[1] & 0x07
                self.scroll_speed = params[2] & 0x07
                self.scroll_end_page = params[3] & 0x07
                self.scroll_v_offset = 0
            return

        # Vertical + Horizontal Scroll (29h / 2Ah)
        if cmd in (0x29, 0x2A):
            self.scroll_right = (cmd == 0x29)
            if len(params) >= 5:
                self.scroll_start_page = params[1] & 0x07
                self.scroll_speed = params[2] & 0x07
                self.scroll_end_page = params[3] & 0x07
                self.scroll_v_offset = params[4] & 0x3F
            return

    # -- GDDRAM byte writer with auto-advance per addressing mode --
    def _write_gddram_byte(self, byte: int) -> None:
        if 0 <= self.page < self.pages and 0 <= self.column < self.width:
            self.gddram[self.page * self.width + self.column] = byte & 0xFF

        if self.addr_mode == self.ADDR_MODE_PAGE:
            # Page mode: column++ only; wraps within 0..width-1.
            # Page does NOT auto-increment (datasheet 10.1.3).
            self.column += 1
            if self.column >= self.width:
                self.column = 0

        elif self.addr_mode == self.ADDR_MODE_HORIZONTAL:
            # Horizontal mode: column++ within [col_start..col_end].
            # On overflow -> col = col_start, page++.
            # On page overflow -> page = page_start.
            self.column += 1
            if self.column > self.col_end:
                self.column = self.col_start
                self.page += 1
                if self.page > self.page_end:
                    self.page = self.page_start

        elif self.addr_mode == self.ADDR_MODE_VERTICAL:
            # Vertical mode: page++ within [page_start..page_end].
            # On overflow -> page = page_start, column++.
            # On column overflow -> column = col_start.
            self.page += 1
            if self.page > self.page_end:
                self.page = self.page_start
                self.column += 1
                if self.column > self.col_end:
                    self.column = self.col_start

    @staticmethod
    def _parse_addr(value: Any) -> int:
        if isinstance(value, int):
            return value
        text = str(value).strip().lower()
        return int(text, 16 if text.startswith("0x") else 10)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="SSD1306 peripheral simulator (JSON-RPC over stdio)")
    parser.add_argument("--width", type=int, default=128, help="Panel width")
    parser.add_argument("--height", type=int, default=64, help="Panel height")
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = Ssd1306Simulator(width=args.width, height=args.height)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
