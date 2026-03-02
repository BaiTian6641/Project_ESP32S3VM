#!/usr/bin/env python3
"""Basic GPIO test simulator.

Provides a simple virtual GPIO playground for GUI testing:
- Inputs: button, switch
- Outputs: LED, RGB LED channels (PWM)
- Script-owned panel mode for dynamic subpanel container flow
"""

import argparse
from typing import Any, Dict

from jsonrpc_stdio import JsonRpcStdioServer, RpcRequest, monotonic_ms


class GpioTestSimulator:
    def __init__(self) -> None:
        self.button: bool = False
        self.switch: bool = False

        self.led: bool = False
        self.pwm_r: int = 0
        self.pwm_g: int = 0
        self.pwm_b: int = 0

    def handle(self, req: RpcRequest) -> Dict[str, Any]:
        method = req.method
        params = req.params

        if method == "ping":
            return {"ok": True, "ts_ms": monotonic_ms()}

        if method == "get_capabilities":
            return {
                "device": {"type": "gpio.test", "id": params.get("id", "gpio-test0")},
                "panel": {
                    "kind": "gpio",
                    "title": "GPIO Test Bench",
                    "description": "Button/switch input and LED/RGB output verification.",
                    "script": {
                        "enabled": True,
                        "event_method": "panel_event",
                        "state_method": "panel_state",
                        "runtime_state_key": "",
                        "fallback_set_parameter": False,
                    },
                    "metrics": [
                        {"label": "Button", "state_path": "button", "true_text": "PRESSED", "false_text": "RELEASED"},
                        {"label": "Switch", "state_path": "switch", "true_text": "ON", "false_text": "OFF"},
                        {"label": "LED", "state_path": "led", "true_text": "ON", "false_text": "OFF"},
                        {"label": "RGB", "state_path": "rgb_hex"}
                    ],
                    "scripts": [
                        {"title": "GPIO State", "state_path": "gpio", "min_height": 120}
                    ],
                    "controls": [
                        {"name": "button", "label": "Button", "type": "bool", "writable": True,
                         "section": "Inputs", "description": "Momentary test button level."},
                        {"name": "switch", "label": "Switch", "type": "bool", "writable": True,
                         "section": "Inputs", "description": "Toggle switch level."},
                        {"name": "led", "label": "LED", "type": "bool", "writable": True,
                         "section": "Outputs", "description": "Single indicator LED state."},
                        {"name": "pwm_r", "label": "PWM Red", "type": "int", "min": 0, "max": 255,
                         "writable": True, "section": "RGB PWM"},
                        {"name": "pwm_g", "label": "PWM Green", "type": "int", "min": 0, "max": 255,
                         "writable": True, "section": "RGB PWM"},
                        {"name": "pwm_b", "label": "PWM Blue", "type": "int", "min": 0, "max": 255,
                         "writable": True, "section": "RGB PWM"},
                        {"name": "reset_outputs", "label": "Reset Outputs", "type": "action",
                         "section": "Operations", "description": "Turn LED off and clear RGB PWM channels."}
                    ],
                },
                "transports": ["gpio"],
            }

        if method == "get_state":
            return self._state()

        if method == "set_parameter":
            self._apply_parameter(str(params.get("name", "")), params.get("value"))
            return {"ok": True, "state": self._state()}

        if method == "panel_event":
            return self._panel_event(params)

        if method == "panel_state":
            return {"panel_state": self._panel_state(params.get("state", {}))}

        if method == "gpio_read":
            return {
                "ok": True,
                "pins": {
                    "button": int(self.button),
                    "switch": int(self.switch),
                    "led": int(self.led),
                },
            }

        if method == "gpio_write":
            values = params.get("values", {}) or {}
            for key, value in values.items():
                self._apply_parameter(str(key), value)
            return {"ok": True, "state": self._state()}

        raise ValueError(f"Unsupported method: {method}")

    def tick(self) -> None:
        pass

    def _clamp_pwm(self, value: Any) -> int:
        return max(0, min(255, int(value)))

    def _apply_parameter(self, name: str, value: Any) -> None:
        if name == "button":
            self.button = bool(value)
            return
        if name == "switch":
            self.switch = bool(value)
            return
        if name == "led":
            self.led = bool(value)
            return
        if name == "pwm_r":
            self.pwm_r = self._clamp_pwm(value)
            return
        if name == "pwm_g":
            self.pwm_g = self._clamp_pwm(value)
            return
        if name == "pwm_b":
            self.pwm_b = self._clamp_pwm(value)
            return
        if name == "reset_outputs":
            self.led = False
            self.pwm_r = 0
            self.pwm_g = 0
            self.pwm_b = 0
            return
        raise ValueError(f"Unsupported parameter: {name}")

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

    def _panel_state(self, _gui_state: Dict[str, Any]) -> Dict[str, Any]:
        state = self._state()
        return {
            "summary": {
                "button": state["button"],
                "switch": state["switch"],
                "led": state["led"],
                "rgb_hex": state["rgb_hex"],
            },
            "gpio": state["gpio"],
        }

    def _state(self) -> Dict[str, Any]:
        rgb_hex = f"#{self.pwm_r:02X}{self.pwm_g:02X}{self.pwm_b:02X}"
        return {
            "button": self.button,
            "switch": self.switch,
            "led": self.led,
            "pwm_r": self.pwm_r,
            "pwm_g": self.pwm_g,
            "pwm_b": self.pwm_b,
            "rgb_hex": rgb_hex,
            "gpio": {
                "inputs": {
                    "button": int(self.button),
                    "switch": int(self.switch),
                },
                "outputs": {
                    "led": int(self.led),
                    "rgb": {
                        "r": self.pwm_r,
                        "g": self.pwm_g,
                        "b": self.pwm_b,
                        "hex": rgb_hex,
                    },
                },
            },
        }


def main() -> None:
    parser = argparse.ArgumentParser(description="GPIO test simulator (JSON-RPC over stdio)")
    parser.parse_args()

    global server
    server = JsonRpcStdioServer()
    sim = GpioTestSimulator()
    server.serve_forever(sim)


if __name__ == "__main__":
    main()
