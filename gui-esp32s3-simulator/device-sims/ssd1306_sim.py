#!/usr/bin/env python3
import argparse
from typing import Any, Dict, List

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class Ssd1306Simulator:
    def __init__(self, width: int = 128, height: int = 64) -> None:
        self.width = width
        self.height = height
        self.pages = height // 8
        self.gddram = bytearray(width * self.pages)

        self.display_on = False
        self.inverted = False
        self.contrast = 0x7F

        self.page = 0
        self.column = 0

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
            req_len = max(0, int(params.get("len", 0)))
            return {"ack": True, "data": [0] * req_len}

        if method == "i2c_transfer":
            return self._i2c_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        return

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
                out_ops.append({
                    "dir": "read",
                    "data": [0] * req_len,
                })
            else:
                out_ops.append({"dir": direction, "error": "unsupported_op"})

        timing = {
            "clock_hz": int(params.get("clock_hz", 400000)),
            "repeated_start": bool(params.get("repeated_start", False)),
        }
        return {"ack": True, "ops": out_ops, "timing": timing}

    def _state_payload(self, include_buffer: bool) -> Dict[str, Any]:
        state = {
            "display_on": self.display_on,
            "inverted": self.inverted,
            "contrast": self.contrast,
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

    def _handle_i2c_payload(self, data: List[int]) -> None:
        idx = 0
        dirty = False
        while idx < len(data):
            control = data[idx]
            idx += 1
            if idx >= len(data):
                break
            if control == 0x00:
                while idx < len(data):
                    cmd = data[idx]
                    idx += 1
                    if (cmd & 0xF0) == 0xB0:
                        self.page = cmd & 0x0F
                    elif (cmd & 0xF0) == 0x10:
                        self.column = (self.column & 0x0F) | ((cmd & 0x0F) << 4)
                    elif (cmd & 0xF0) == 0x00:
                        self.column = (self.column & 0xF0) | (cmd & 0x0F)
                    elif cmd == 0xAE:
                        self.display_on = False
                    elif cmd == 0xAF:
                        self.display_on = True
                    elif cmd == 0xA6:
                        self.inverted = False
                    elif cmd == 0xA7:
                        self.inverted = True
                    elif cmd == 0x81 and idx < len(data):
                        self.contrast = data[idx]
                        idx += 1
                    else:
                        pass
                    if idx < len(data) and data[idx] in (0x00, 0x40):
                        break
            elif control == 0x40:
                while idx < len(data) and data[idx] not in (0x00, 0x40):
                    if 0 <= self.page < self.pages and 0 <= self.column < self.width:
                        self.gddram[self.page * self.width + self.column] = data[idx]
                        dirty = True
                        self.column = (self.column + 1) % self.width
                    idx += 1
            else:
                while idx < len(data) and data[idx] not in (0x00, 0x40):
                    idx += 1

        if dirty:
            self._emit_frame_update()

    @staticmethod
    def _parse_addr(value: Any) -> int:
        if isinstance(value, int):
            return value
        text = str(value).strip().lower()
        return int(text, 16 if text.startswith("0x") else 10)


def main() -> None:
    parser = argparse.ArgumentParser(description="SSD1306 peripheral simulator (JSON-RPC over stdio)")
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--height", type=int, default=64)
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = Ssd1306Simulator(width=args.width, height=args.height)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
