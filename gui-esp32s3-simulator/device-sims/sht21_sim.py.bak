#!/usr/bin/env python3
import argparse
import csv
import os
import time
from typing import Any, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


def _encode_temp_raw(temp_c: float) -> int:
    raw = int(((temp_c + 46.85) * 65536.0) / 175.72)
    raw &= 0xFFFC
    return max(0, min(0xFFFC, raw))


def _encode_rh_raw(rh: float) -> int:
    raw = int(((rh + 6.0) * 65536.0) / 125.0)
    raw &= 0xFFFC
    raw |= 0x0002
    return max(0, min(0xFFFE, raw))


class Sht21Simulator:
    def __init__(self, addr: int = 0x40) -> None:
        self.addr = addr
        self.temperature_c = 25.0
        self.humidity_rh = 50.0
        self.user_reg = 0x02

        self.last_cmd: Optional[int] = None
        self.pending_read: List[int] = []

        self.mode = "manual"
        self.playback_rows: List[Dict[str, float]] = []
        self.playback_idx = 0
        self.playback_running = False
        self.playback_loop = False
        self.playback_speed = 1.0
        self.playback_start_mono = 0.0

    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "sht21", "id": params.get("id", "sht21")},
                "panel": {
                    "kind": "sensor.scalar",
                    "controls": [
                        {"name": "temperature_c", "type": "number", "min": -40.0, "max": 125.0, "writable": True},
                        {"name": "humidity_rh", "type": "number", "min": 0.0, "max": 100.0, "writable": True},
                        {"name": "mode", "type": "enum", "enum": ["manual", "csv"], "writable": True},
                    ],
                    "csv": {
                        "supported": True,
                        "columns": ["timestamp_s", "temperature_c", "humidity_rh"],
                    },
                },
                "transports": ["i2c"],
                "i2c": {"addresses": [hex(self.addr)]},
            }

        if method == "get_state":
            return self._state()

        if method == "set_parameter":
            name = params.get("name")
            value = params.get("value")
            if name == "temperature_c":
                self.temperature_c = float(value)
                self.mode = "manual"
            elif name == "humidity_rh":
                self.humidity_rh = float(value)
                self.mode = "manual"
            elif name == "mode":
                text = str(value).strip().lower()
                if text not in ("manual", "csv"):
                    raise ValueError("mode must be manual or csv")
                self.mode = text
            else:
                raise ValueError(f"Unsupported parameter: {name}")
            self._emit_telemetry()
            return {"ok": True, "state": self._state()}

        if method == "start_csv_playback":
            path = str(params.get("path", "")).strip()
            loop = bool(params.get("loop", False))
            speed = float(params.get("speed", 1.0))
            if not path:
                raise ValueError("path is required")
            self._load_csv(path)
            self.playback_loop = loop
            self.playback_speed = max(0.05, speed)
            self.playback_running = True
            self.mode = "csv"
            self.playback_idx = 0
            self.playback_start_mono = time.monotonic()
            return {"ok": True, "rows": len(self.playback_rows)}

        if method == "pause_csv_playback":
            self.playback_running = False
            return {"ok": True, "state": self._state()}

        if method == "stop_csv_playback":
            self.playback_running = False
            self.playback_idx = 0
            self.mode = "manual"
            return {"ok": True, "state": self._state()}

        if method == "seek_csv_playback":
            idx = int(params.get("index", 0))
            self.playback_idx = max(0, min(idx, max(0, len(self.playback_rows) - 1)))
            return {"ok": True, "index": self.playback_idx}

        if method == "i2c_write":
            addr = self._parse_addr(params.get("addr", hex(self.addr)))
            if addr != self.addr:
                return {"ack": False}
            data = [int(x) & 0xFF for x in params.get("data", [])]
            self._handle_i2c_write(data)
            return {"ack": True, "written": len(data)}

        if method == "i2c_read":
            addr = self._parse_addr(params.get("addr", hex(self.addr)))
            req_len = max(0, int(params.get("len", 0)))
            if addr != self.addr:
                return {"ack": False, "data": []}
            payload = self.pending_read[:req_len]
            if len(payload) < req_len:
                payload.extend([0] * (req_len - len(payload)))
            self.pending_read = self.pending_read[req_len:]
            return {"ack": True, "data": payload}

        if method == "i2c_transfer":
            return self._i2c_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        if not self.playback_running or not self.playback_rows:
            return

        elapsed = (time.monotonic() - self.playback_start_mono) * self.playback_speed

        advanced = False
        while self.playback_idx < len(self.playback_rows):
            row = self.playback_rows[self.playback_idx]
            t = row.get("timestamp_s", float(self.playback_idx))
            if t > elapsed:
                break
            self.temperature_c = row.get("temperature_c", self.temperature_c)
            self.humidity_rh = row.get("humidity_rh", self.humidity_rh)
            self.playback_idx += 1
            advanced = True

        if advanced:
            self._emit_telemetry()

        if self.playback_idx >= len(self.playback_rows):
            if self.playback_loop:
                self.playback_idx = 0
                self.playback_start_mono = time.monotonic()
            else:
                self.playback_running = False
                server.notify("playback_finished", {"ts_ms": monotonic_ms()})

    def _i2c_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        addr = self._parse_addr(params.get("address", params.get("addr", hex(self.addr))))
        if addr != self.addr:
            return {"ack": False, "ops": [], "error": "address_nack"}

        ops = params.get("ops", [])
        out_ops: List[Dict[str, Any]] = []
        for op in ops:
            direction = str(op.get("dir", "")).lower()
            if direction == "write":
                data = [int(x) & 0xFF for x in op.get("data", [])]
                self._handle_i2c_write(data)
                out_ops.append({
                    "dir": "write",
                    "written": len(data),
                    "acked_bytes": [True] * len(data),
                })
            elif direction == "read":
                req_len = max(0, int(op.get("len", 0)))
                payload = self.pending_read[:req_len]
                if len(payload) < req_len:
                    payload.extend([0] * (req_len - len(payload)))
                self.pending_read = self.pending_read[req_len:]
                out_ops.append({
                    "dir": "read",
                    "data": payload,
                    "nack_last": bool(op.get("nack_last", True)),
                })
            else:
                out_ops.append({"dir": direction, "error": "unsupported_op"})

        timing = {
            "clock_hz": int(params.get("clock_hz", 100000)),
            "repeated_start": bool(params.get("repeated_start", False)),
        }
        return {"ack": True, "ops": out_ops, "timing": timing}

    def _state(self) -> Dict[str, Any]:
        return {
            "temperature_c": round(self.temperature_c, 3),
            "humidity_rh": round(self.humidity_rh, 3),
            "mode": self.mode,
            "playback": {
                "running": self.playback_running,
                "index": self.playback_idx,
                "rows": len(self.playback_rows),
                "loop": self.playback_loop,
                "speed": self.playback_speed,
            },
        }

    def _emit_telemetry(self) -> None:
        server.notify(
            "telemetry",
            {
                "temperature_c": round(self.temperature_c, 4),
                "humidity_rh": round(self.humidity_rh, 4),
                "ts_ms": monotonic_ms(),
            },
        )

    def _load_csv(self, path: str) -> None:
        if not os.path.exists(path):
            raise ValueError(f"CSV not found: {path}")

        rows: List[Dict[str, float]] = []
        with open(path, "r", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for i, row in enumerate(reader):
                t = float(row.get("timestamp_s", i))
                temp = float(row.get("temperature_c", self.temperature_c))
                rh = float(row.get("humidity_rh", self.humidity_rh))
                rows.append({"timestamp_s": t, "temperature_c": temp, "humidity_rh": rh})

        if not rows:
            raise ValueError("CSV has no rows")

        rows.sort(key=lambda x: x["timestamp_s"])
        t0 = rows[0]["timestamp_s"]
        for row in rows:
            row["timestamp_s"] -= t0

        self.playback_rows = rows

    def _handle_i2c_write(self, data: List[int]) -> None:
        if not data:
            return
        cmd = data[0]
        self.last_cmd = cmd

        if cmd == 0xE6 and len(data) >= 2:
            self.user_reg = data[1] & 0x3F
            self.pending_read = []
            return

        if cmd == 0xE7:
            self.pending_read = [self.user_reg]
            return

        if cmd in (0xE3, 0xF3):
            raw = _encode_temp_raw(self.temperature_c)
            self.pending_read = [(raw >> 8) & 0xFF, raw & 0xFF, 0x00]
            return

        if cmd in (0xE5, 0xF5):
            raw = _encode_rh_raw(self.humidity_rh)
            self.pending_read = [(raw >> 8) & 0xFF, raw & 0xFF, 0x00]
            return

        if cmd == 0xFE:
            self.user_reg = 0x02
            self.pending_read = []
            return

        self.pending_read = []

    @staticmethod
    def _parse_addr(value: Any) -> int:
        if isinstance(value, int):
            return value
        text = str(value).strip().lower()
        return int(text, 16 if text.startswith("0x") else 10)


def main() -> None:
    parser = argparse.ArgumentParser(description="SHT21 peripheral simulator (JSON-RPC over stdio)")
    parser.add_argument("--address", default="0x40", help="I2C address")
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = Sht21Simulator(addr=int(args.address, 16 if str(args.address).lower().startswith("0x") else 10))
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
