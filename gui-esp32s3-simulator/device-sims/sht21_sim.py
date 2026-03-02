#!/usr/bin/env python3
"""SHT21 temperature & humidity sensor simulator -- models real hardware behaviour.

Faithfully implements the Sensirion SHT21 I2C protocol per datasheet:
  - Hold-master (0xE3 / 0xE5) and no-hold-master (0xF3 / 0xF5) measurement modes
  - CRC-8 checksum (polynomial x^8 + x^5 + x^4 + 1 = 0x131)
  - User register read / write with reserved-bit preservation
  - Resolution-dependent raw value encoding (status bits in LSB)
  - Soft reset (0xFE) -- resets to defaults, preserves heater bit
  - Correct measurement timing per resolution setting
"""
import argparse
import random
from typing import Any, Dict, List, Optional

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms

# ---------------------------------------------------------------
# Resolution table:  user_reg bits [7, 0] -> (rh_bits, temp_bits,
#                     max_rh_ms, max_temp_ms)
# From SHT21 datasheet Table 7 & Table 8
# ---------------------------------------------------------------
_RESOLUTION_TABLE = {
    0b00: (12, 14, 29, 85),    # RH:12-bit, T:14-bit (default)
    0b01: (8, 12, 4, 22),      # RH:8-bit,  T:12-bit
    0b10: (10, 13, 9, 43),     # RH:10-bit, T:13-bit
    0b11: (11, 11, 15, 11),    # RH:11-bit, T:11-bit
}


def _crc8_sht21(data: bytes | bytearray | list) -> int:
    """CRC-8 per SHT21 datasheet section 6.  Polynomial 0x131."""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def _encode_temp_raw(temp_c: float, resolution_bits: int = 14) -> int:
    """Convert °C to raw 16-bit sensor value.

    Formula (inverted from datasheet):
        S_T = (T + 46.85) * 2^16 / 175.72

    Status bits: bit 1 = 0 (temperature), bit 0 = 0.
    Low bits below resolution are zeroed.
    """
    raw = int((temp_c + 46.85) * 65536.0 / 175.72 + 0.5)
    raw = max(0, min(0xFFFF, raw))
    # Clear status bits (bits 1-0)
    raw &= 0xFFFC
    # Mask out lower bits beyond resolution
    shift = 14 - resolution_bits
    if shift > 0:
        mask = (0xFFFF >> shift) << shift
        raw &= mask
    # Bit 1 = 0 for temperature (already cleared above)
    return raw


def _encode_rh_raw(rh: float, resolution_bits: int = 12) -> int:
    """Convert %RH to raw 16-bit sensor value.

    Formula (inverted from datasheet):
        S_RH = (RH + 6) * 2^16 / 125

    Status bits: bit 1 = 1 (humidity), bit 0 = 0.
    Low bits below resolution are zeroed.
    """
    raw = int((rh + 6.0) * 65536.0 / 125.0 + 0.5)
    raw = max(0, min(0xFFFF, raw))
    # Clear the lowest 2 bits, then set bit 1 = 1 (humidity flag)
    raw &= 0xFFFC
    shift = 12 - resolution_bits
    if shift > 0:
        mask = (0xFFFF >> shift) << shift
        raw &= mask
    raw |= 0x02  # Set status bit 1 = humidity
    return raw


class Sht21Simulator:
    """Models Sensirion SHT21 sensor per official datasheet."""

    # I2C command bytes
    CMD_TEMP_HOLD = 0xE3
    CMD_RH_HOLD = 0xE5
    CMD_TEMP_NO_HOLD = 0xF3
    CMD_RH_NO_HOLD = 0xF5
    CMD_WRITE_USER_REG = 0xE6
    CMD_READ_USER_REG = 0xE7
    CMD_SOFT_RESET = 0xFE

    # User register reset default = 0b0000_0010  (OTP_reload disabled)
    _USER_REG_DEFAULT = 0x02

    def __init__(self) -> None:
        # Simulated environment values
        self.temperature: float = 25.0      # °C
        self.humidity: float = 50.0         # %RH

        # I2C user register (8 bits)
        self.user_reg: int = self._USER_REG_DEFAULT

        # Measurement state
        self._pending_measurement: Optional[str] = None  # "temp" or "rh"
        self._hold_mode: bool = False
        self._meas_start_ms: int = 0
        self._meas_result: Optional[List[int]] = None  # [MSB, LSB, CRC]

        # --- Variation / drift ---
        self._temp_drift = 0.0
        self._rh_drift = 0.0
        self._noise = True

    # -- Resolution property --
    @property
    def _resolution_key(self) -> int:
        """Extract resolution bits [7, 0] from user register."""
        return ((self.user_reg >> 6) & 0x02) | (self.user_reg & 0x01)

    @property
    def _rh_bits(self) -> int:
        return _RESOLUTION_TABLE[self._resolution_key][0]

    @property
    def _temp_bits(self) -> int:
        return _RESOLUTION_TABLE[self._resolution_key][1]

    @property
    def _rh_max_ms(self) -> int:
        return _RESOLUTION_TABLE[self._resolution_key][2]

    @property
    def _temp_max_ms(self) -> int:
        return _RESOLUTION_TABLE[self._resolution_key][3]

    @property
    def heater_enabled(self) -> bool:
        return bool(self.user_reg & 0x04)

    @property
    def end_of_battery(self) -> bool:
        return bool(self.user_reg & 0x40)

    # -- JSON-RPC handler --
    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "sht21", "id": params.get("id", "sht21")},
                "panel": {
                    "kind": "sensor",
                    "controls": [
                        {"name": "temperature", "type": "float",
                         "min": -40, "max": 125, "unit": "°C", "writable": True},
                        {"name": "humidity", "type": "float",
                         "min": 0, "max": 100, "unit": "%RH", "writable": True},
                        {"name": "noise", "type": "bool", "writable": True},
                    ],
                },
                "transports": ["i2c"],
                "i2c": {"addresses": ["0x40"]},
            }

        if method == "get_state":
            return self._state_payload()

        if method == "set_parameter":
            return self._set_parameter(params)

        if method == "i2c_write":
            return self._i2c_write(params)

        if method == "i2c_read":
            return self._i2c_read(params)

        if method == "i2c_transfer":
            return self._i2c_transfer(params)

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        """Called periodically; finish any pending no-hold measurement."""
        if self._pending_measurement and not self._hold_mode:
            elapsed = monotonic_ms() - self._meas_start_ms
            needed = (self._temp_max_ms
                      if self._pending_measurement == "temp"
                      else self._rh_max_ms)
            if elapsed >= needed:
                self._finish_measurement()

    # -- I2C write --
    def _i2c_write(self, params: Dict[str, Any]) -> Dict[str, Any]:
        addr = self._parse_addr(params.get("addr", "0x40"))
        if addr != 0x40:
            return {"ack": False}
        data = [int(x) & 0xFF for x in params.get("data", [])]
        if not data:
            return {"ack": True, "written": 0}

        self._process_command(data)
        return {"ack": True, "written": len(data)}

    # -- I2C read --
    def _i2c_read(self, params: Dict[str, Any]) -> Dict[str, Any]:
        addr = self._parse_addr(params.get("addr", "0x40"))
        if addr != 0x40:
            return {"ack": False}
        req_len = max(0, int(params.get("len", 0)))
        result = self._get_read_data(req_len)
        return {"ack": True, "data": result}

    # -- I2C transfer (multi-op) --
    def _i2c_transfer(self, params: Dict[str, Any]) -> Dict[str, Any]:
        addr = self._parse_addr(params.get("address", params.get("addr", "0x40")))
        if addr != 0x40:
            return {"ack": False, "ops": [], "error": "address_nack"}

        ops = params.get("ops", [])
        out_ops: List[Dict[str, Any]] = []
        for op in ops:
            direction = str(op.get("dir", "")).lower()
            if direction == "write":
                data = [int(x) & 0xFF for x in op.get("data", [])]
                self._process_command(data)
                out_ops.append({
                    "dir": "write",
                    "written": len(data),
                    "acked_bytes": [True] * len(data),
                })
            elif direction == "read":
                req_len = max(0, int(op.get("len", 0)))
                out_ops.append({"dir": "read", "data": self._get_read_data(req_len)})
            else:
                out_ops.append({"dir": direction, "error": "unsupported_op"})

        timing = {
            "clock_hz": int(params.get("clock_hz", 400000)),
            "repeated_start": bool(params.get("repeated_start", False)),
        }
        return {"ack": True, "ops": out_ops, "timing": timing}

    # -- Command processing --
    def _process_command(self, data: List[int]) -> None:
        if not data:
            return

        cmd = data[0]

        # -- Trigger Temperature Measurement (hold / no-hold) --
        if cmd in (self.CMD_TEMP_HOLD, self.CMD_TEMP_NO_HOLD):
            self._pending_measurement = "temp"
            self._hold_mode = (cmd == self.CMD_TEMP_HOLD)
            self._meas_start_ms = monotonic_ms()
            self._meas_result = None
            if self._hold_mode:
                # Hold master: data available immediately (SCL held low conceptually)
                self._finish_measurement()
            return

        # -- Trigger Humidity Measurement (hold / no-hold) --
        if cmd in (self.CMD_RH_HOLD, self.CMD_RH_NO_HOLD):
            self._pending_measurement = "rh"
            self._hold_mode = (cmd == self.CMD_RH_HOLD)
            self._meas_start_ms = monotonic_ms()
            self._meas_result = None
            if self._hold_mode:
                self._finish_measurement()
            return

        # -- Write User Register (0xE6) --
        if cmd == self.CMD_WRITE_USER_REG:
            if len(data) >= 2:
                new_val = data[1]
                # Bits 3, 4, 5 are reserved -- preserve from current register.
                reserved_mask = 0x38            # bits 5:3
                writable_mask = ~reserved_mask & 0xFF
                self.user_reg = (self.user_reg & reserved_mask) | (new_val & writable_mask)
            return

        # -- Read User Register (0xE7) --
        if cmd == self.CMD_READ_USER_REG:
            # Result prepared for next read: [register_value, CRC]
            crc = _crc8_sht21([self.user_reg])
            self._meas_result = [self.user_reg, crc]
            self._pending_measurement = "user_reg"
            return

        # -- Soft Reset (0xFE) --
        if cmd == self.CMD_SOFT_RESET:
            heater = self.user_reg & 0x04   # Preserve heater bit
            self.user_reg = self._USER_REG_DEFAULT | heater
            self._pending_measurement = None
            self._meas_result = None
            return

    # -- Produce measurement result --
    def _finish_measurement(self) -> None:
        if self._pending_measurement == "temp":
            value = self._sampled_temperature()
            raw = _encode_temp_raw(value, self._temp_bits)
            msb = (raw >> 8) & 0xFF
            lsb = raw & 0xFF
            crc = _crc8_sht21([msb, lsb])
            self._meas_result = [msb, lsb, crc]

        elif self._pending_measurement == "rh":
            value = self._sampled_humidity()
            raw = _encode_rh_raw(value, self._rh_bits)
            msb = (raw >> 8) & 0xFF
            lsb = raw & 0xFF
            crc = _crc8_sht21([msb, lsb])
            self._meas_result = [msb, lsb, crc]

        self._pending_measurement = None

    # -- Return buffered read data --
    def _get_read_data(self, req_len: int) -> List[int]:
        if self._meas_result is not None:
            result = self._meas_result[:req_len]
            # Pad with 0xFF if requested length exceeds available data
            while len(result) < req_len:
                result.append(0xFF)
            self._meas_result = None
            return result
        # No data ready -- NACK would be correct; return 0xFF as filler.
        return [0xFF] * req_len

    # -- Sampled values with optional noise --
    def _sampled_temperature(self) -> float:
        t = self.temperature + self._temp_drift
        if self._noise:
            t += random.gauss(0, 0.1)
        return max(-40.0, min(125.0, t))

    def _sampled_humidity(self) -> float:
        rh = self.humidity + self._rh_drift
        if self._noise:
            rh += random.gauss(0, 0.5)
        return max(0.0, min(100.0, rh))

    # -- set_parameter handler --
    def _set_parameter(self, params: Dict[str, Any]) -> Dict[str, Any]:
        name = params.get("name")
        value = params.get("value")
        if name == "temperature":
            self.temperature = max(-40.0, min(125.0, float(value)))
        elif name == "humidity":
            self.humidity = max(0.0, min(100.0, float(value)))
        elif name == "noise":
            self._noise = bool(value)
        elif name == "temp_drift":
            self._temp_drift = float(value)
        elif name == "rh_drift":
            self._rh_drift = float(value)
        else:
            raise ValueError(f"Unsupported parameter: {name}")
        return {"ok": True, "state": self._state_payload()}

    # -- Build state dict for GUI --
    def _state_payload(self) -> Dict[str, Any]:
        rh_bits, temp_bits, rh_ms, temp_ms = _RESOLUTION_TABLE[self._resolution_key]
        return {
            "temperature": round(self.temperature, 2),
            "humidity": round(self.humidity, 2),
            "user_register": f"0x{self.user_reg:02X}",
            "resolution": {
                "rh_bits": rh_bits,
                "temp_bits": temp_bits,
                "max_rh_ms": rh_ms,
                "max_temp_ms": temp_ms,
            },
            "heater_enabled": self.heater_enabled,
            "end_of_battery": self.end_of_battery,
            "noise": self._noise,
            "pending_measurement": self._pending_measurement,
        }

    @staticmethod
    def _parse_addr(value: Any) -> int:
        if isinstance(value, int):
            return value
        text = str(value).strip().lower()
        return int(text, 16 if text.startswith("0x") else 10)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="SHT21 peripheral simulator (JSON-RPC over stdio)")
    parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = Sht21Simulator()
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
