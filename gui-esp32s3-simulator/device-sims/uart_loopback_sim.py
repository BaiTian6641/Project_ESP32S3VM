#!/usr/bin/env python3
"""UART loopback device simulator -- models real UART transceiver behaviour.

Models a generic UART loopback device with:
  - Configurable baud rate, data bits (5-8), parity (none/even/odd), stop bits (1/1.5/2)
  - RX FIFO with configurable depth and overflow detection
  - Framing error detection on baud rate mismatch
  - Parity error injection when parity settings disagree
  - Break condition detection (all-zero character)
  - Baud-rate-dependent loopback latency
  - Flow control tracking (RTS/CTS)
  - Byte-level TX/RX counters and error counters
"""
import argparse
import collections
from typing import Any, Deque, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class UartLoopbackSimulator:
    """Models a UART loopback transceiver with real error detection."""

    # Default ESP32-S3 UART FIFO depth
    DEFAULT_FIFO_DEPTH = 128

    def __init__(self, baud: int = 115200) -> None:
        self.server: Optional[JsonRpcStdioServer] = None

        # -- Line configuration (device side) --
        self.baud: int = max(300, int(baud))
        self.data_bits: int = 8
        self.parity: str = "none"       # none / even / odd
        self.stop_bits: float = 1.0     # 1 / 1.5 / 2

        # -- Flow control --
        self.flow_control: str = "none"  # none / rtscts / xonxoff
        self.rts_active: bool = True     # Ready to receive (device side)
        self.cts_active: bool = True     # Clear to send (host side)
        self._xon: bool = True           # XON/XOFF software flow state

        # -- FIFO --
        self.fifo_depth: int = self.DEFAULT_FIFO_DEPTH
        self._rx_fifo: Deque[int] = collections.deque(maxlen=self.fifo_depth)

        # -- Host-side line config (what the firmware configured) --
        self._host_baud: int = self.baud
        self._host_data_bits: int = 8
        self._host_parity: str = "none"
        self._host_stop_bits: float = 1.0

        # -- Pending loopback with latency --
        self._pending_tx: List[int] = []
        self._pending_tx_time_ms: int = 0

        # -- Counters --
        self.tx_count: int = 0
        self.rx_count: int = 0
        self.framing_errors: int = 0
        self.parity_errors: int = 0
        self.overrun_errors: int = 0
        self.break_count: int = 0

        # -- Preview --
        self.last_rx_preview: List[int] = []

    def bind_server(self, server: JsonRpcStdioServer) -> None:
        self.server = server

    # -- JSON-RPC handler --
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
                    "title": "UART Loopback",
                    "description": "UART line model with flow/framing behavior and loopback counters.",
                    "script": {
                        "enabled": True,
                        "event_method": "panel_event",
                        "state_method": "panel_state",
                        "runtime_state_key": "",
                        "fallback_set_parameter": False,
                    },
                    "metrics": [
                        {"label": "TX", "state_path": "counters.tx"},
                        {"label": "RX", "state_path": "counters.rx"},
                        {"label": "Framing Err", "state_path": "counters.framing_errors"},
                        {"label": "FIFO", "state_path": "fifo_level"}
                    ],
                    "scripts": [
                        {"title": "Device Config", "state_path": "device_config", "min_height": 110},
                        {"title": "Host Config", "state_path": "host_config", "min_height": 110}
                    ],
                    "controls": [
                        {"name": "baud", "label": "Baud", "type": "int", "min": 300, "max": 5_000_000,
                         "writable": True, "section": "Line", "description": "Device-side baud rate."},
                        {"name": "data_bits", "label": "Data Bits", "type": "enum", "enum": [5, 6, 7, 8],
                         "writable": True, "section": "Line"},
                        {"name": "parity", "label": "Parity", "type": "enum", "enum": ["none", "even", "odd"],
                         "writable": True, "section": "Line"},
                        {"name": "stop_bits", "label": "Stop Bits", "type": "enum", "enum": [1, 1.5, 2],
                         "writable": True, "section": "Line"},
                        {"name": "flow_control", "label": "Flow", "type": "enum",
                         "enum": ["none", "rtscts", "xonxoff"], "writable": True, "section": "Flow"},
                        {"name": "fifo_depth", "label": "FIFO Depth", "type": "int", "min": 1, "max": 1024,
                         "writable": True, "section": "Flow"},
                        {"name": "reset_counters", "label": "Reset Counters", "type": "action",
                         "section": "Operations", "description": "Reset UART traffic/error counters."},
                    ],
                },
                "transports": ["uart"],
                "uart": {
                    "framing": ["8N1", "8E1", "8O1", "8N2", "7E1", "7O1", "5N1"],
                    "baud_min": 300,
                    "baud_max": 5_000_000,
                    "flow_control": ["none", "rtscts", "xonxoff"],
                },
            }

        if method == "get_state":
            return self._state()

        if method == "set_parameter":
            return self._set_parameter(params)

        if method == "panel_event":
            return self._panel_event(params)

        if method == "panel_state":
            return {"panel_state": self._panel_state(params.get("state", {}))}

        if method == "uart_set_line":
            return self._uart_set_line(params)

        if method == "uart_tx":
            return self._uart_tx(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        """Deliver pending loopback data after baud-rate-dependent latency."""
        if not self._pending_tx:
            return

        now = monotonic_ms()
        if now < self._pending_tx_time_ms:
            return

        payload = self._pending_tx
        self._pending_tx = []

        # Check for baud rate mismatch (>5% difference -> framing error)
        baud_mismatch = abs(self._host_baud - self.baud) / max(1, self.baud) > 0.05
        # Check for parity mismatch
        parity_mismatch = (self._host_parity != self.parity)
        # Check for data bits mismatch
        bits_mismatch = (self._host_data_bits != self.data_bits)

        delivered: List[int] = []

        for byte in payload:
            # Break detection: all-zero with expected framing
            if byte == 0x00:
                self.break_count += 1

            if baud_mismatch or bits_mismatch:
                self.framing_errors += 1
                # Garbled byte on framing error
                byte = byte ^ 0xFF
            elif parity_mismatch:
                self.parity_errors += 1
                # Data still delivered but parity flag set

            # Mask to data_bits
            mask = (1 << self.data_bits) - 1
            byte &= mask

            # Check flow control
            if self.flow_control == "rtscts" and not self.rts_active:
                self.overrun_errors += 1
                continue
            if self.flow_control == "xonxoff" and not self._xon:
                self.overrun_errors += 1
                continue

            # FIFO overflow check
            if len(self._rx_fifo) >= self.fifo_depth:
                self.overrun_errors += 1
                # Oldest byte is discarded (deque maxlen handles this)

            self._rx_fifo.append(byte)
            delivered.append(byte)
            self.rx_count += 1

        if delivered and self.server is not None:
            self.server.notify(
                "uart_rx",
                {
                    "data": delivered,
                    "ts_ms": monotonic_ms(),
                    "errors": {
                        "framing": self.framing_errors,
                        "parity": self.parity_errors,
                        "overrun": self.overrun_errors,
                    },
                },
            )
            self.last_rx_preview = delivered[:64]

    # -- UART TX handler --
    def _uart_tx(self, params: Dict[str, Any]) -> Dict[str, Any]:
        payload = [int(x) & 0xFF for x in params.get("data", [])]
        self.tx_count += len(payload)

        if not payload:
            return {"ok": True, "tx_bytes": 0, "loopback": True}

        # Calculate loopback latency based on baud rate
        # Each byte = start_bit + data_bits + parity_bit + stop_bits
        parity_bits = 0 if self.parity == "none" else 1
        bits_per_byte = 1 + self.data_bits + parity_bits + self.stop_bits
        total_bits = bits_per_byte * len(payload)
        latency_ms = max(1, int((total_bits / max(300, self.baud)) * 1000))

        # Queue for latency-delayed delivery
        self._pending_tx.extend(payload)
        self._pending_tx_time_ms = monotonic_ms() + latency_ms

        return {
            "ok": True,
            "tx_bytes": len(payload),
            "loopback": True,
            "latency_ms": latency_ms,
        }

    # -- Line configuration from firmware --
    def _uart_set_line(self, params: Dict[str, Any]) -> Dict[str, Any]:
        self._host_baud = max(300, int(params.get("baud", self._host_baud)))
        self._host_data_bits = int(params.get("data_bits", self._host_data_bits))
        if self._host_data_bits not in (5, 6, 7, 8):
            self._host_data_bits = 8
        parity = str(params.get("parity", self._host_parity)).strip().lower()
        self._host_parity = parity if parity in ("none", "even", "odd") else "none"
        stop = params.get("stop_bits", self._host_stop_bits)
        self._host_stop_bits = float(stop) if float(stop) in (1.0, 1.5, 2.0) else 1.0
        return {"ok": True, "state": self._state()}

    # -- Parameter setter --
    def _set_parameter(self, params: Dict[str, Any]) -> Dict[str, Any]:
        name = str(params.get("name", ""))
        value = params.get("value")
        self._apply_parameter(name, value)

        return {"ok": True, "state": self._state()}

    def _apply_parameter(self, name: str, value: Any) -> None:
        if name == "baud":
            self.baud = max(300, int(value))
            return
        if name == "data_bits":
            v = int(value)
            if v in (5, 6, 7, 8):
                self.data_bits = v
            return
        if name == "parity":
            text = str(value).strip().lower()
            if text in ("none", "even", "odd"):
                self.parity = text
            return
        if name == "stop_bits":
            v = float(value)
            if v in (1.0, 1.5, 2.0):
                self.stop_bits = v
            return
        if name == "flow_control":
            text = str(value).strip().lower()
            if text in ("none", "rtscts", "xonxoff"):
                self.flow_control = text
            return
        if name == "fifo_depth":
            depth = max(1, min(1024, int(value)))
            self.fifo_depth = depth
            self._rx_fifo = collections.deque(self._rx_fifo, maxlen=depth)
            return
        if name == "rts":
            self.rts_active = bool(value)
            return
        if name == "cts":
            self.cts_active = bool(value)
            return
        if name == "reset_counters":
            self.tx_count = 0
            self.rx_count = 0
            self.framing_errors = 0
            self.parity_errors = 0
            self.overrun_errors = 0
            self.break_count = 0
            return
        raise ValueError(f"Unsupported parameter: {name}")

    def _panel_state(self, _gui_state: Dict[str, Any]) -> Dict[str, Any]:
        state = self._state()
        return {
            "summary": {
                "tx": state["counters"]["tx"],
                "rx": state["counters"]["rx"],
                "fifo_level": state["fifo_level"],
                "flow_control": state["flow_control"],
            },
            "device_config": state["device_config"],
            "host_config": state["host_config"],
        }

    def _panel_event(self, params: Dict[str, Any]) -> Dict[str, Any]:
        control = str(params.get("control", "")).strip()
        value = params.get("value")
        event = str(params.get("event", "set_control")).strip().lower()

        if control and event in ("set_control", "action"):
            self._apply_parameter(control, value)

        return {
            "ok": True,
            "state_patch": self._state(),
            "panel_state": self._panel_state({}),
        }

    # -- State for GUI --
    def _state(self) -> Dict[str, Any]:
        return {
            "device_config": {
                "baud": self.baud,
                "data_bits": self.data_bits,
                "parity": self.parity,
                "stop_bits": self.stop_bits,
            },
            "host_config": {
                "baud": self._host_baud,
                "data_bits": self._host_data_bits,
                "parity": self._host_parity,
                "stop_bits": self._host_stop_bits,
            },
            "flow_control": self.flow_control,
            "rts_active": self.rts_active,
            "cts_active": self.cts_active,
            "fifo_depth": self.fifo_depth,
            "fifo_level": len(self._rx_fifo),
            "counters": {
                "tx": self.tx_count,
                "rx": self.rx_count,
                "framing_errors": self.framing_errors,
                "parity_errors": self.parity_errors,
                "overrun_errors": self.overrun_errors,
                "break_detected": self.break_count,
            },
            "last_rx_preview": self.last_rx_preview,
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="UART loopback simulator (JSON-RPC over stdio)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Device-side baud rate (default 115200)")
    parser.add_argument("--fifo", type=int, default=128,
                        help="RX FIFO depth (default 128)")
    args = parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = UartLoopbackSimulator(baud=args.baud)
    sim.fifo_depth = max(1, args.fifo)
    sim.bind_server(server)
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
