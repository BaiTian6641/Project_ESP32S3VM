#!/usr/bin/env python3
"""
SSD1331 SPI display simulator (JSON-RPC over stdio).

Maintains an internal 96×64 RGB565 framebuffer.
Accepts SPI transactions with a 'dc' field (0 = command, 1 = data)
as emitted by the QEMU GP-SPI bridge.  Falls back to heuristic
command/data detection when 'dc' is absent.

Frame updates emit RGB565 pixels as 16-bit integers in scanline order
so the GUI can render full 65K-colour output.
"""
import argparse
from typing import Any, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class Ssd1331Simulator:
    def __init__(self, width: int = 96, height: int = 64) -> None:
        self.server: Optional[JsonRpcStdioServer] = None
        self.width = max(1, int(width))
        self.height = max(1, int(height))

        self.fb = [0x0000] * (self.width * self.height)

        self.display_on = True
        self.inverted = False
        self.all_on = False
        self.contrast_a = 0x91
        self.contrast_b = 0x50
        self.contrast_c = 0x7D

        self.col_start = 0
        self.col_end = self.width - 1
        self.row_start = 0
        self.row_end = self.height - 1
        self.cur_col = 0
        self.cur_row = 0

        self._pending_cmd: Optional[int] = None
        self._pending_args: List[int] = []
        self._write_ram_mode = False
        self._pixel_hi_pending: Optional[int] = None

        self._frame_dirty = False
        self._dirty_since_ms = 0

    def bind_server(self, server: JsonRpcStdioServer) -> None:
        self.server = server

    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "ssd1331", "id": params.get("id", "ssd1331")},
                "panel": {
                    "kind": "display",
                    "title": "SSD1331 OLED",
                    "description": "96\u00d764 65K-colour RGB OLED display.",
                    "width": self.width,
                    "height": self.height,
                    "pixel_format": "rgb565",
                    "display": {
                        "state_key": "frame_update",
                        "fallback_state_key": "buffer",
                        "layout": "scanline",
                        "encoding": "rgb565",
                    },
                    "metrics": [
                        {"label": "Display", "state_path": "display_on", "true_text": "ON", "false_text": "OFF"},
                        {"label": "Invert", "state_path": "inverted", "true_text": "YES", "false_text": "NO"},
                        {"label": "All On", "state_path": "all_on", "true_text": "YES", "false_text": "NO"},
                    ],
                },
                "transports": ["spi"],
                "spi": {
                    "modes": ["single"],
                    "cpol": 0,
                    "cpha": 0,
                    "max_hz": 20000000,
                },
            }

        if method == "get_state":
            include_buffer = bool(params.get("include_buffer", False))
            return self._state_payload(include_buffer=include_buffer)

        if method == "set_parameter":
            name = str(params.get("name", "")).strip()
            value = params.get("value")
            if name == "display_on":
                self.display_on = bool(value)
            elif name == "inverted":
                self.inverted = bool(value)
            elif name == "all_on":
                self.all_on = bool(value)
            else:
                raise ValueError(f"Unsupported parameter: {name}")
            self._mark_dirty()
            return {"ok": True, "state": self._state_payload(include_buffer=False)}

        if method == "spi_transfer":
            return self._spi_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        if self._frame_dirty and (monotonic_ms() - self._dirty_since_ms) >= 50:
            self._frame_dirty = False
            self._emit_frame_update()

    def _mark_dirty(self) -> None:
        if not self._frame_dirty:
            self._dirty_since_ms = monotonic_ms()
        self._frame_dirty = True

    def _state_payload(self, include_buffer: bool) -> Dict[str, Any]:
        state: Dict[str, Any] = {
            "display_on": self.display_on,
            "inverted": self.inverted,
            "all_on": self.all_on,
            "contrast": int((self.contrast_a + self.contrast_b + self.contrast_c) / 3),
            "contrast_a": self.contrast_a,
            "contrast_b": self.contrast_b,
            "contrast_c": self.contrast_c,
            "geometry": {"width": self.width, "height": self.height},
            "window": {
                "col_start": self.col_start,
                "col_end": self.col_end,
                "row_start": self.row_start,
                "row_end": self.row_end,
            },
            "cursor": {"column": self.cur_col, "row": self.cur_row},
        }
        if include_buffer:
            state["buffer"] = {
                "encoding": "rgb565",
                "layout": "scanline",
                "data": self._rgb565_scanline(),
            }
        return state

    def _emit_frame_update(self) -> None:
        if not self.server:
            return
        self.server.notify(
            "frame_update",
            {
                "width": self.width,
                "height": self.height,
                "encoding": "rgb565",
                "layout": "scanline",
                "data": self._rgb565_scanline(),
                "ts_ms": monotonic_ms(),
            },
        )

    def _rgb565_scanline(self) -> List[int]:
        """Return framebuffer as a flat list of 16-bit RGB565 values in
        row-major (scanline) order, applying display_on / invert / all_on."""
        total = self.width * self.height
        out: List[int] = [0] * total

        if not self.display_on:
            return out  # all black

        for idx in range(total):
            pix = 0xFFFF if self.all_on else self.fb[idx]
            if self.inverted:
                # Invert each colour component
                r5 = (~(pix >> 11)) & 0x1F
                g6 = (~(pix >> 5)) & 0x3F
                b5 = (~pix) & 0x1F
                pix = (r5 << 11) | (g6 << 5) | b5
            out[idx] = pix

        return out

    def _spi_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        raw_tx = params.get("tx", [])
        if not isinstance(raw_tx, list):
            return {"ok": False, "error": "invalid_tx", "rx": []}

        tx = [int(x) & 0xFF for x in raw_tx]
        rx_len = max(0, int(params.get("rx_len", 0)))
        rx = [0x00] * rx_len

        # DC pin: 0 = command, 1 = data, absent = heuristic fallback
        dc = params.get("dc")

        if dc is not None:
            dc = int(dc)
            if dc == 1:
                # Data phase — always pixel data
                if tx:
                    self._write_pixel_stream_rgb565(tx)
                    self._mark_dirty()
            else:
                # Command phase — parse as commands
                self._ingest_command_stream(tx)
        else:
            # Legacy fallback: heuristic detection (no DC pin info)
            self._ingest_spi_stream(tx)

        return {
            "ok": True,
            "rx": rx,
            "timing": {
                "mode": str(params.get("mode", "single")),
                "frequency_hz": int(params.get("frequency_hz", 8_000_000)),
            },
        }

    def _command_arg_count(self, cmd: int) -> int:
        if cmd in (0x15, 0x75):
            return 2
        if cmd in (0x81, 0x82, 0x83):
            return 1
        if cmd in (0xA0, 0xA1, 0xA2, 0xA8, 0xAB, 0xAD, 0xB1, 0xB3, 0xBB, 0xBE, 0xFD):
            return 1
        if cmd in (0xA4, 0xA5, 0xA6, 0xA7, 0xAE, 0xAF, 0x5C):
            return 0
        return -1

    def _looks_like_command_packet(self, tx: List[int]) -> bool:
        if not tx:
            return False
        arg_count = self._command_arg_count(tx[0])
        if arg_count < 0:
            return False
        return len(tx) <= max(1, arg_count + 1)

    def _ingest_command_stream(self, tx: List[int]) -> None:
        """Process bytes known to be on the command (DC=0) bus."""
        if not tx:
            return
        changed = False
        i = 0
        while i < len(tx):
            if self._pending_cmd is not None:
                need = self._command_arg_count(self._pending_cmd) - len(self._pending_args)
                take = min(need, len(tx) - i)
                self._pending_args.extend(tx[i:i + take])
                i += take
                if len(self._pending_args) == self._command_arg_count(self._pending_cmd):
                    if self._apply_command(self._pending_cmd, self._pending_args):
                        changed = True
                    self._pending_cmd = None
                    self._pending_args = []
                continue

            cmd = tx[i]
            i += 1
            arg_count = self._command_arg_count(cmd)
            if arg_count < 0:
                continue

            if arg_count == 0:
                if self._apply_command(cmd, []):
                    changed = True
                continue

            available = len(tx) - i
            if available >= arg_count:
                args = tx[i:i + arg_count]
                i += arg_count
                if self._apply_command(cmd, args):
                    changed = True
                continue

            self._pending_cmd = cmd
            self._pending_args = list(tx[i:])
            i = len(tx)

        if changed:
            self._mark_dirty()

    def _apply_command(self, cmd: int, args: List[int]) -> bool:
        changed = False

        if cmd == 0x15 and len(args) >= 2:  # Set Column Address
            col_start = max(0, min(self.width - 1, args[0]))
            col_end = max(0, min(self.width - 1, args[1]))
            if col_end < col_start:
                col_start, col_end = col_end, col_start
            if self.col_start != col_start or self.col_end != col_end or self.cur_col != col_start:
                changed = True
            self.col_start = col_start
            self.col_end = col_end
            self.cur_col = self.col_start
            return changed

        if cmd == 0x75 and len(args) >= 2:  # Set Row Address
            row_start = max(0, min(self.height - 1, args[0]))
            row_end = max(0, min(self.height - 1, args[1]))
            if row_end < row_start:
                row_start, row_end = row_end, row_start
            if self.row_start != row_start or self.row_end != row_end or self.cur_row != row_start:
                changed = True
            self.row_start = row_start
            self.row_end = row_end
            self.cur_row = self.row_start
            return changed

        if cmd == 0x5C:  # Write RAM
            self._write_ram_mode = True
            return False

        if cmd == 0x81 and len(args) >= 1:
            if self.contrast_a != args[0]:
                changed = True
            self.contrast_a = args[0]
            return changed
        if cmd == 0x82 and len(args) >= 1:
            if self.contrast_b != args[0]:
                changed = True
            self.contrast_b = args[0]
            return changed
        if cmd == 0x83 and len(args) >= 1:
            if self.contrast_c != args[0]:
                changed = True
            self.contrast_c = args[0]
            return changed

        if cmd == 0xA4:
            if self.all_on:
                changed = True
            self.all_on = False
            return changed
        if cmd == 0xA5:
            if not self.all_on:
                changed = True
            self.all_on = True
            return changed
        if cmd == 0xA6:
            if self.inverted:
                changed = True
            self.inverted = False
            return changed
        if cmd == 0xA7:
            if not self.inverted:
                changed = True
            self.inverted = True
            return changed
        if cmd == 0xAE:
            if self.display_on:
                changed = True
            self.display_on = False
            return changed
        if cmd == 0xAF:
            if not self.display_on:
                changed = True
            self.display_on = True
            return changed

        return False

    def _ingest_spi_stream(self, tx: List[int]) -> None:
        if not tx:
            return

        changed = False
        if self._write_ram_mode and not self._looks_like_command_packet(tx):
            self._write_pixel_stream_rgb565(tx)
            changed = True
        else:
            i = 0
            while i < len(tx):
                if self._pending_cmd is not None:
                    need = self._command_arg_count(self._pending_cmd) - len(self._pending_args)
                    take = min(need, len(tx) - i)
                    self._pending_args.extend(tx[i:i + take])
                    i += take
                    if len(self._pending_args) == self._command_arg_count(self._pending_cmd):
                        if self._apply_command(self._pending_cmd, self._pending_args):
                            changed = True
                        self._pending_cmd = None
                        self._pending_args = []
                    continue

                cmd = tx[i]
                i += 1
                arg_count = self._command_arg_count(cmd)
                if arg_count < 0:
                    continue

                if arg_count == 0:
                    if self._apply_command(cmd, []):
                        changed = True
                    if cmd == 0x5C and i < len(tx):
                        self._write_pixel_stream_rgb565(tx[i:])
                        changed = True
                        i = len(tx)
                    continue

                available = len(tx) - i
                if available >= arg_count:
                    args = tx[i:i + arg_count]
                    i += arg_count
                    if self._apply_command(cmd, args):
                        changed = True
                    continue

                self._pending_cmd = cmd
                self._pending_args = tx[i:]
                i = len(tx)

        if changed:
            self._mark_dirty()

    def _write_pixel_stream_rgb565(self, pix_bytes: List[int]) -> None:
        col = self.cur_col
        row = self.cur_row

        i = 0
        if self._pixel_hi_pending is not None and pix_bytes:
            value = ((self._pixel_hi_pending & 0xFF) << 8) | (pix_bytes[0] & 0xFF)
            i = 1
            self._pixel_hi_pending = None
            if 0 <= col < self.width and 0 <= row < self.height:
                self.fb[row * self.width + col] = value
            col += 1
            if col > self.col_end:
                col = self.col_start
                row += 1
                if row > self.row_end:
                    row = self.row_start

        while i + 1 < len(pix_bytes):
            value = ((pix_bytes[i] & 0xFF) << 8) | (pix_bytes[i + 1] & 0xFF)
            i += 2

            if 0 <= col < self.width and 0 <= row < self.height:
                self.fb[row * self.width + col] = value

            col += 1
            if col > self.col_end:
                col = self.col_start
                row += 1
                if row > self.row_end:
                    row = self.row_start

        if i < len(pix_bytes):
            self._pixel_hi_pending = pix_bytes[i]

        self.cur_col = col
        self.cur_row = row


def main() -> None:
    parser = argparse.ArgumentParser(description="SSD1331 SPI display simulator (JSON-RPC over stdio)")
    parser.add_argument("--width", type=int, default=96)
    parser.add_argument("--height", type=int, default=64)
    args, _unknown = parser.parse_known_args()

    server = JsonRpcStdioServer()
    sim = Ssd1331Simulator(width=args.width, height=args.height)
    sim.bind_server(server)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
