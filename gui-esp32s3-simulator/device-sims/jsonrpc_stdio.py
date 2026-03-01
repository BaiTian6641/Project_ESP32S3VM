#!/usr/bin/env python3
import json
import selectors
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, Optional


@dataclass
class RpcRequest:
    req_id: Any
    method: str
    params: Dict[str, Any]


class JsonRpcStdioServer:
    def __init__(self) -> None:
        self.selector = selectors.DefaultSelector()
        self.selector.register(sys.stdin, selectors.EVENT_READ)
        self.running = True

    def serve_forever(self, handler, tick_interval: float = 0.1) -> None:
        if callable(handler):
            request_handler = handler
        elif hasattr(handler, "handle") and callable(getattr(handler, "handle")):
            request_handler = handler.handle
        else:
            raise TypeError("handler must be callable or provide a callable handle(req) method")

        while self.running:
            events = self.selector.select(timeout=tick_interval)
            if events:
                line = sys.stdin.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue

                req = RpcRequest(
                    req_id=obj.get("id"),
                    method=obj.get("method", ""),
                    params=obj.get("params", {}) or {},
                )
                try:
                    result = request_handler(req)
                    if req.req_id is not None:
                        self.send_result(req.req_id, result)
                except Exception as exc:
                    if req.req_id is not None:
                        self.send_error(req.req_id, code=-32000, message=str(exc))
            try:
                tick = getattr(handler, "tick", None)
                if callable(tick):
                    tick()
            except Exception:
                pass

    def notify(self, method: str, params: Optional[Dict[str, Any]] = None) -> None:
        payload = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        self._write(payload)

    def send_result(self, req_id: Any, result: Any) -> None:
        payload = {"jsonrpc": "2.0", "id": req_id, "result": result}
        self._write(payload)

    def send_error(self, req_id: Any, code: int, message: str, data: Any = None) -> None:
        payload: Dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": code, "message": message},
        }
        if data is not None:
            payload["error"]["data"] = data
        self._write(payload)

    def _write(self, payload: Dict[str, Any]) -> None:
        sys.stdout.write(json.dumps(payload, separators=(",", ":")) + "\n")
        sys.stdout.flush()


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)
