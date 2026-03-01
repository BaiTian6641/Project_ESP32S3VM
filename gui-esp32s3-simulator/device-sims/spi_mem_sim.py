#!/usr/bin/env python3
import argparse
from typing import Any, Dict, List

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class SpiMemorySimulator:
    def __init__(self, size_bytes: int = 1024 * 1024) -> None:
        self.size_bytes = max(4096, int(size_bytes))
        self.mem = bytearray([0xFF] * self.size_bytes)
        self.write_enable_latch = False
        self.last_mode = "single"
        self.last_freq_hz = 20_000_000

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
                    "controls": [
                        {"name": "erase_chip", "type": "action"},
                        {"name": "write_enable_latch", "type": "bool", "writable": False},
                    ],
                },
                "transports": ["spi"],
                "spi": {
                    "modes": ["single", "dual", "quad"],
                    "max_hz": 80_000_000,
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
                self.write_enable_latch = False
                return {"ok": True, "state": self._state()}
            raise ValueError(f"Unsupported parameter: {name}")

        if method == "spi_transfer":
            return self._spi_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        return

    def _state(self) -> Dict[str, Any]:
        return {
            "size_bytes": self.size_bytes,
            "write_enable_latch": self.write_enable_latch,
            "last_mode": self.last_mode,
            "last_freq_hz": self.last_freq_hz,
        }

    def _spi_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        mode = str(params.get("mode", "single")).strip().lower()
        if mode not in ("single", "dual", "quad"):
            mode = "single"
        self.last_mode = mode

        freq_hz = int(params.get("frequency_hz", 20_000_000))
        self.last_freq_hz = max(1000, freq_hz)

        tx = [int(x) & 0xFF for x in params.get("tx", [])]
        rx_len = max(0, int(params.get("rx_len", 0)))

        if not tx:
            return {
                "ok": True,
                "rx": [0] * rx_len,
                "timing": {"mode": mode, "frequency_hz": self.last_freq_hz},
            }

        cmd = tx[0]
        rx = [0] * rx_len

        if cmd == 0x9F:
            jedec = [0xEF, 0x40, 0x18]
            rx = jedec[:rx_len] + [0] * max(0, rx_len - len(jedec))
        elif cmd == 0x06:
            self.write_enable_latch = True
        elif cmd == 0x04:
            self.write_enable_latch = False
        elif cmd in (0x03, 0x0B):
            if len(tx) < 4:
                return {"ok": False, "error": "read_requires_24bit_address"}
            addr = ((tx[1] << 16) | (tx[2] << 8) | tx[3]) % self.size_bytes
            if cmd == 0x0B and len(tx) >= 5:
                addr = (addr + 1) % self.size_bytes
            for i in range(rx_len):
                rx[i] = self.mem[(addr + i) % self.size_bytes]
        elif cmd == 0x02:
            if len(tx) < 4:
                return {"ok": False, "error": "program_requires_24bit_address"}
            if not self.write_enable_latch:
                return {"ok": False, "error": "write_enable_required"}
            addr = ((tx[1] << 16) | (tx[2] << 8) | tx[3]) % self.size_bytes
            payload = tx[4:]
            for i, byte in enumerate(payload):
                index = (addr + i) % self.size_bytes
                self.mem[index] &= byte
            self.write_enable_latch = False
        else:
            pass

        return {
            "ok": True,
            "rx": rx,
            "timing": {"mode": mode, "frequency_hz": self.last_freq_hz},
        }


def main() -> None:
    parser = argparse.ArgumentParser(description="SPI memory peripheral simulator (JSON-RPC over stdio)")
    parser.add_argument("--size", type=int, default=1024 * 1024, help="Flash size in bytes")
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = SpiMemorySimulator(size_bytes=args.size)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
