#!/usr/bin/env python3
import json
import os
import queue
import selectors
import socket
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any, Dict, Optional

_IS_WINDOWS = os.name == "nt"


@dataclass
class RpcRequest:
    req_id: Any
    method: str
    params: Dict[str, Any]


def _stdin_reader_thread(q: "queue.Queue[str | None]") -> None:
    """Background thread that reads lines from stdin and puts them into *q*.

    This is needed on Windows where ``select()`` / ``selectors`` cannot
    poll non-socket file descriptors (pipes, stdin).  On POSIX systems the
    main event loop uses ``selectors`` directly and this thread is never
    started.
    """
    try:
        for line in sys.stdin:
            q.put(line)
    except (OSError, ValueError):
        pass
    finally:
        q.put(None)  # sentinel — stdin closed / EOF


class JsonRpcStdioServer:
    def __init__(self) -> None:
        self.running = True
        self._selector = None
        self._stdin_queue = None
        self._reader_thread = None

        # On Windows, selectors.select() cannot poll stdin (pipes);
        # use a background reader thread + queue instead.
        if _IS_WINDOWS:
            self._enable_stdin_thread_mode()
        else:
            self._selector = selectors.DefaultSelector()
            self._selector.register(sys.stdin, selectors.EVENT_READ)

    def _enable_stdin_thread_mode(self) -> None:
        self._stdin_queue = queue.Queue()
        self._reader_thread = threading.Thread(
            target=_stdin_reader_thread,
            args=(self._stdin_queue,),
            daemon=True,
        )
        self._reader_thread.start()
        self._selector = None

    def serve_forever(self, handler, tick_interval: float = 0.1) -> None:
        if callable(handler):
            request_handler = handler
        elif hasattr(handler, "handle") and callable(getattr(handler, "handle")):
            request_handler = handler.handle
        else:
            raise TypeError("handler must be callable or provide a callable handle(req) method")

        while self.running:
            line: str | None = None
            got_line = False

            if self._selector is None:
                # Drain as many complete lines as available (non-blocking
                # after the first one which blocks up to tick_interval).
                try:
                    line = self._stdin_queue.get(timeout=tick_interval)
                    got_line = True
                except queue.Empty:
                    line = None
                    got_line = False
            else:
                try:
                    events = self._selector.select(timeout=tick_interval)
                except OSError as exc:
                    # Defensive fallback for Windows builds/bundles where
                    # select() can fail with WSAStartup-related errors.
                    if getattr(exc, "winerror", None) in (10093, 10038) and self._stdin_queue is None:
                        _tmp = socket.socket()
                        _tmp.close()
                        self._enable_stdin_thread_mode()
                        continue
                    raise
                if events:
                    line = sys.stdin.readline()
                    got_line = True

            if got_line:
                if line is None:
                    break  # EOF sentinel in thread mode
                if not line:
                    break  # EOF
                line = line.strip()
                if line:
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        pass
                    else:
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
        try:
            sys.stdout.write(json.dumps(payload, separators=(",", ":")) + "\n")
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            self.running = False


def monotonic_ms() -> int:
    return int(time.monotonic() * 1000)
