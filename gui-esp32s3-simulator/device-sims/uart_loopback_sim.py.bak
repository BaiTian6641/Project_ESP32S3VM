#!/usr/bin/env python3
import argparse
from typing import Any, Dict, List

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class UartLoopbackSimulator:
    def __init__(self, baud: int = 115200) -> None:
        self.server: JsonRpcStdioServer | None = None
        self.baud = int(baud)
        self.data_bits = 8
        self.parity = "none"
        self.stop_bits = 1

        self.tx_count = 0
        self.rx_count = 0
        self.last_rx_preview: List[int] = []

    def bind_server(self, server: JsonRpcStdioServer) -> None:
        self.server = server

    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "uart.loopback", "id": params.get("id", "uart-loop")},
                "panel": {
                    "kind": "uart",
                    "controls": [
                        {"name": "baud", "type": "int", "min": 1200, "max": 3000000, "writable": True},
                        {"name": "parity", "type": "enum", "enum": ["none", "even", "odd"], "writable": True},
                        {"name": "stop_bits", "type": "enum", "enum": [1, 2], "writable": True},
                    ],
                },
                "transports": ["uart"],
                "uart": {
                    "framing": ["8N1", "8E1", "8O1", "8N2"],
                    "baud_min": 1200,
                    "baud_max": 3000000,
                },
            }

        if method == "get_state":
            return self._state()

        if method == "set_parameter":
            name = str(params.get("name", ""))
            value = params.get("value")
            if name == "baud":
                self.baud = max(1200, int(value))
            elif name == "parity":
                text = str(value).strip().lower()
                if text not in ("none", "even", "odd"):
                    raise ValueError("parity must be none/even/odd")
                self.parity = text
            elif name == "stop_bits":
                value_int = int(value)
                if value_int not in (1, 2):
                    raise ValueError("stop_bits must be 1 or 2")
                self.stop_bits = value_int
            else:
                raise ValueError(f"Unsupported parameter: {name}")
            return {"ok": True, "state": self._state()}

        if method == "uart_set_line":
            self.baud = max(1200, int(params.get("baud", self.baud)))
            self.data_bits = int(params.get("data_bits", self.data_bits))
            parity = str(params.get("parity", self.parity)).strip().lower()
            self.parity = parity if parity in ("none", "even", "odd") else "none"
            stop_bits = int(params.get("stop_bits", self.stop_bits))
            self.stop_bits = stop_bits if stop_bits in (1, 2) else 1
            return {"ok": True, "state": self._state()}

        if method == "uart_tx":
            payload = [int(x) & 0xFF for x in params.get("data", [])]
            self.tx_count += len(payload)

            if payload:
                if self.server is not None:
                    self.server.notify(
                        "uart_rx",
                        {
                            "data": payload,
                            "ts_ms": monotonic_ms(),
                        },
                    )
                self.rx_count += len(payload)
                self.last_rx_preview = payload[:64]

            return {"ok": True, "tx_bytes": len(payload), "loopback": True}

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        return

    def _state(self) -> Dict[str, Any]:
        return {
            "baud": self.baud,
            "data_bits": self.data_bits,
            "parity": self.parity,
            "stop_bits": self.stop_bits,
            "tx_count": self.tx_count,
            "rx_count": self.rx_count,
            "last_rx_preview": self.last_rx_preview,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description="UART loopback peripheral simulator (JSON-RPC over stdio)")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = UartLoopbackSimulator(baud=args.baud)
    sim.bind_server(server)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
