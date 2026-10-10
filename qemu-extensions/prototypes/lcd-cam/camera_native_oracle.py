"""Pure ordinary OV2640 oracle; reads actual sole-collector evidence only.

No QMP, invented ACK/payload/clock, emulator encoder, or relaxed bad-frame
filtering. External consumers retain their separate strict memory oracle.
"""
import hashlib
import importlib.util
from pathlib import Path
import re

_path = Path(__file__).resolve().parents[1] / "memory/lcd_cam_consumer_oracle.py"
_spec = importlib.util.spec_from_file_location("_ordinary_camera_memory_rules", _path)
_rules = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_rules)
_need, _eq, _u, _addr = _rules._need, _rules._eq, _rules._u, _rules._addr
PROFILES = {"camera-" + name for name in ("rgb565", "yuv422", "jpeg", "multi", "slow",
           "truncation", "overflow", "reset", "disconnected-sccb", "disconnected-pclk")}


class UART:
    def __init__(self, text):
        _need(type(text) is str and len(text.encode()) <= 8 * 1024 * 1024, "UART absent/bound")
        self.text = re.sub(r"\x1b\[[0-9;]*m", "", text)
        _need(not re.search(r"Guru Meditation|assert failed|abort\(\)|Backtrace:|Stack smashing", self.text),
              "guest panic/assert/abort")
        self.rows = []
        for line in self.text.splitlines():
            match = re.search(r"LCDCAM ([A-Z_0-9]+)(?:\s|$)", line)
            if not match:
                continue
            pairs = re.findall(r"([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)", line[match.end():])
            _need(len({key for key, _ in pairs}) == len(pairs), "duplicate UART field")
            self.rows.append((match[1], dict(pairs, _index=len(self.rows))))

    def all(self, tag, **fields):
        return [row for name, row in self.rows if name == tag and
                all(row.get(key) == value for key, value in fields.items())]

    def one(self, tag, **fields):
        result = self.all(tag, **fields)
        _need(len(result) == 1, f"required unique {tag} {fields}: {len(result)}")
        return result[0]

    def api(self, op):
        row = self.one("API", op=op)
        _eq(row, {"err": "ESP_OK", "code": "0"}, op)
        return row


def _clock(uart, captures, snapshots):
    uart.api("camera_ledc_timer")
    uart.api("camera_ledc_channel")
    uart.one("CAM_XCLK", gpio="15", source="LEDC", channel="0", timer="0",
             requested_hz="20000000", resolution="1", duty="1")
    _need(captures and all(item["lease_status"]["xclk_period_ns"] == 50 for item in captures),
          "missing actual resolved physical 20MHz XCLK at sensor")
    qualified = []
    for snapshot in snapshots:
        host = snapshot["_host_observation"]
        _need(host["paused"] is True and uart.text.startswith(host["uart_prefix"]),
              "camera read lease is not an actual stopped UART prefix")
        if "CAM_XCLK " not in host["uart_prefix"] or "camera_ledc_stop" in host["uart_prefix"]:
            continue
        raw = host.get("ledc")
        if raw is None:
            continue
        _need(all(type(raw[key]) is int and 0 <= raw[key] <= 0xffffffff for key in
                  ("channel_conf0", "channel_hpoint", "channel_duty", "timer_conf", "gpio15_matrix_out")),
              "raw LEDC/matrix observations malformed")
        timer = raw["timer_conf"]
        _need(timer & 15 == 1 and (timer >> 4) & 0x3ffff == 512 and not timer & ((1 << 22) | (1 << 23)),
              "actual LEDC timer is not live resolution1/divider2")
        _need(raw["channel_conf0"] & 7 == 4 and raw["channel_hpoint"] == 0 and
              raw["channel_duty"] == 16 and raw["gpio15_matrix_out"] & 0x3ff == 73,
              "physical GPIO15 is not driven by channel0 duty1 LEDC signal73")
        qualified.append(raw)
    _need(qualified, "no current-binary live LEDC register/matrix capture")
    return {"gpio": 15, "matrix_signal": 73, "resolved_period_ns": 50, "live_observations": len(qualified)}


def _guest_bytes(uart, row, sources, snapshots):
    frame = _u(row, "frame")
    lease = uart.one("CAM_BUFFER", frame=str(frame))
    address, length = _addr(lease, "payload"), _u(row, "len")
    _rules._internal(address, length)
    observed = []
    for state in snapshots:
        host = state["_host_observation"]
        prefix = host["uart_prefix"]
        if not re.search(rf"LCDCAM CAM_HOLD_BEGIN frame={frame}\b", prefix) or re.search(
                rf"LCDCAM CAM_RETURN frame={frame}\b", prefix):
            continue
        for item in host.get("camera_buffers", []):
            if item["frame"] != frame:
                continue
            _need(item["address"] == address and type(item["data"]) is bytes and
                  len(item["data"]) == length and item["raw_reads"], "guest full payload lease absent/malformed")
            observed.append(item["data"])
    _need(observed and all(data == observed[0] for data in observed), "held guest bytes absent/changed")
    data = observed[0]
    _eq(row, {"hash": _rules._ref.fnv1a(data), "head": data[:2].hex(), "tail": data[-2:].hex(),
              "width": "160", "height": "120", "valid": "1"}, "actual guest byte contract")
    candidates = [meta["frame"] for meta, raw, _ in sources if raw == data]
    _need(candidates, "full actual guest payload differs byte-by-byte from every independently captured source")
    retained = uart.one("CAM_RETAIN", frame=str(frame), metadata_same="1", unchanged="1")
    _eq(retained, {"before_hash": row["hash"], "after_hash": row["hash"]}, "retained bytes")
    _need(lease["_index"] < row["_index"] < retained["_index"] < uart.one("CAM_RETURN", frame=str(frame))["_index"],
          "actual ownership ordering invalid")
    timestamp = _u(lease, "timestamp_us")
    _need(0 < timestamp <= _u(row, "time_us"), "actual producer timestamp absent or after delivered frame")
    return {"frame": frame, "address": address, "bytes": length, "sha256": hashlib.sha256(data).hexdigest(),
            "source_candidate_frames": candidates, "hold_us": _u(retained, "elapsed_us"),
            "timestamp_us": timestamp, "handle": _addr(lease, "handle")}


def verify(profile, uart_text, sensor_captures, controller_snapshots, electrical_snapshots=None, ledc_snapshots=None):
    _need(profile in PROFILES, "unknown ordinary camera profile")
    uart = UART(uart_text)
    _need(controller_snapshots and len(controller_snapshots) >= 2, "missing actual controller series")
    if profile.startswith("camera-disconnected-"):
        _need(not uart.all("CAM_FRAME") and not uart.all("CAM_RECOVERY"), "disconnected negative delivered a frame")
        stopped = controller_snapshots[-1]["_host_observation"]
        diagnostics = stopped.get("guest_errors", "") + "\n" + str(stopped.get("electrical", {}))
        strict = (stopped.get("phase") == "strict-electrical-pause" and
                  stopped.get("running") is False and stopped.get("paused") is True and
                  "UNKNOWN" in diagnostics)
        timeouts = uart.all("CAM_TIMEOUT")
        official = timeouts and all(3900000 <= _u(row, "elapsed_us") <= 4500000 for row in timeouts)
        _need(strict or official or (profile.endswith("sccb") and
              any(row.get("op") == "camera_init" and row.get("err") == "ESP_ERR_NOT_SUPPORTED"
                  for row in uart.all("API"))), "negative lacks surfaced strict UNKNOWN or actual official driver failure")
        return {"status": "PASS", "profile": profile, "scope": "genuine_disconnected_negative",
                "strict_unknown": bool(strict), "official_timeout": bool(official)}
    _need(not uart.all("ERROR") and not uart.all("UNQUALIFIED"), "fixture surfaced ERROR/UNQUALIFIED")
    uart.one("BEGIN", camera="v2.1.8", hash="fnv1a32")
    uart.one("END")
    uart.api("camera_init")
    uart.one("CAM_PROBE", pid="0026", ver="41", midh="7f", midl="a2")
    fmt = "jpeg" if profile in {"camera-jpeg", "camera-overflow"} else "yuv422" if profile == "camera-yuv422" else "rgb565"
    sources = [_rules._sensor(item, fmt) for item in sensor_captures]
    _need(sources, "no independent actual OV2640 DVP capture")
    clock = _clock(uart, sensor_captures, controller_snapshots)
    for a, b in zip(sources, sources[1:]):
        _need(a[0]["frame"] < b[0]["frame"] and a[0]["frame_end_ns"] < b[0]["frame_start_ns"],
              "sensor capture series duplicate/reordered")
    begin = uart.all("CAM_GET_BEGIN")
    _need([_u(row, "frame") for row in begin] == list(range(4)), "all four public GET requests required")
    rows = uart.all("CAM_FRAME")
    _need(all(row.get("valid") == "1" for row in rows), "bad guest frame cannot be trimmed")
    for row in rows:
        _eq(row, {"format": {"rgb565": "0", "yuv422": "1", "jpeg": "4"}[fmt]}, "official pixel format")
        if fmt != "jpeg":
            _eq(row, {"len": "38400"}, "QQVGA raw extent")
        else:
            _eq(row, {"head": "ffd8", "tail": "ffd9"}, "actual JPEG SOI/EOI")
    timeouts = uart.all("CAM_TIMEOUT")
    _need(len(rows) + len(timeouts) == 4, "each requested public GET needs a frame or actual timeout")
    for row in timeouts:
        _need(3900000 <= _u(row, "elapsed_us") <= 4500000, "GET null is not the official bounded4s timeout")
    guests = [_guest_bytes(uart, row, sources, controller_snapshots) for row in rows]
    summary = uart.one("CAM_SUMMARY", invalid="0",
                       buffers="2" if profile in {"camera-multi", "camera-slow"} else "1",
                       slow_ms="500" if profile == "camera-slow" else "0",
                       fault="1" if profile == "camera-truncation" else "2" if profile == "camera-reset" else "0")
    _need(_u(summary, "received") == len(rows), "actual GET summary count differs")
    live = [channel for state in controller_snapshots for channel in
            state["_host_observation"].get("gdma", {}).get("channels", [])
            if channel["direction"] == "IN" and channel["peripheral"] == 5 and not channel["park"]]
    _need(live and any(channel.get("descriptors") for channel in live),
          "no actual live LCD_CAM peri5 IN descriptor/DMA observation")
    final = controller_snapshots[-1]
    if profile == "camera-overflow":
        # The official internal-buffer drop occurs in cam_hal.c298/376 after
        # fulfilled DMA, not necessarily in the hardware RX-overflow counter.
        _need(not rows and len(timeouts) == 4 and "cam_hal: FB-OVF" in uart.text and
              final["cam_bytes"] - controller_snapshots[0]["cam_bytes"] > 128,
              "128byte JPEG overflow lacks actual driver FB-OVF/drop, fulfilled RX and four bounded null GETs")
        _need(all(len(data) > 128 for _, data, _ in sources), "overflow source is not larger than128")
    elif profile in {"camera-truncation", "camera-reset"}:
        fault = uart.one("CAM_PHYSICAL_FAULT", href="1", vsync="0")
        _eq(fault, {"pin": "14" if profile.endswith("truncation") else "13",
                    "level": "1" if profile.endswith("truncation") else "0"}, "actual fault pin")
        before = [item.get("partial_frame_aborts", 0) for state in controller_snapshots
                  if "CAM_PHYSICAL_FAULT " not in state["_host_observation"]["uart_prefix"]
                  for item in state["_host_observation"].get("sensors", [])]
        _need(before, "physical fault lacks a real pre-fault sensor observation")
        aborted = [item for state in controller_snapshots
                   if "CAM_PHYSICAL_FAULT " in state["_host_observation"]["uart_prefix"] and
                      "CAM_RECOVERY " not in state["_host_observation"]["uart_prefix"]
                   for item in state["_host_observation"].get("sensors", [])
                   if item.get("last_abort_reason") == ("hardware PWDN high" if profile.endswith("truncation") else "reset settling")
                   and 0 < item.get("last_abort_bytes", 0) < 38400 and
                   item.get("partial_frame_aborts", 0) > max(before) and item.get("last_abort_ns", 0) > 0]
        _need(aborted and timeouts, "physical activeframe truncation/reset rejection absent")
        uart.api("camera_physical_recovery")
        uart.api("camera_reinit_after_reset")
        recovery = uart.one("CAM_RECOVERY", width="160", height="120", len="38400", valid="1")
        guests.append(_guest_bytes(uart, recovery, sources, controller_snapshots))
        uart.one("CAM_RECOVERY_RETURN")
        uart.api("camera_recovery_deinit")
    else:
        _need(len(rows) == 4 and not timeouts, "ordinary format/buffering profile requires four actual frames")
        _need(len(sources) >= 4, "four physical source frames required, GPIO camera-loop is not sensor proof")
        _rules._controllers(controller_snapshots, True, sum(item["bytes"] for item in guests))
    if profile in {"camera-multi", "camera-slow"}:
        addresses = sorted({item["address"] for item in guests})
        _need(len(addresses) == 2 and addresses[0] + 38400 <= addresses[1] and
              len({item["handle"] for item in guests}) == 2,
              "requested two public buffers did not expose two distinct nonoverlapping actual slots")
        _need(all(a["timestamp_us"] < b["timestamp_us"] for a, b in zip(guests, guests[1:])),
              "actual two-buffer producer timestamps did not advance")
    if profile == "camera-slow":
        _need(all(item["hold_us"] >= 500000 for item in guests), "actual owned buffer500ms retention absent")
    uart.api("camera_deinit")
    uart.api("camera_ledc_stop")
    return {"status": "PASS", "profile": profile, "scope": "ordinary_public_camera_observations",
            "clock": clock, "guest_frames": guests, "source_frames": [proof for _, _, proof in sources],
            "limits": ["Static source bytes do not uniquely identify a source frame; actual candidate IDs retained.",
                       "Stopped ownership captures do not fabricate continuous write-address traces.",
                       "Silicon OV2640 compression/timing and physical reference metrology are not qualified."]}


def verify_external(profile, uart_text, sensor_captures, controller_snapshots, base_proof):
    """Add physical LEDC/full guest bytes AFTER the unchanged strict memory oracle.

    Keeps all cache, direct-DMA, two-held ownership, decoder and same-final-binary
    memory-foundation requirements; it is not an alternate consumer oracle.
    """
    _need(profile in {"lcd-camera-rgb565", "lcd-camera-yuv422", "lcd-camera-jpeg"},
          "unknown external camera profile")
    _eq(base_proof, {"profile": profile, "status": "PASS",
                    "same_binary_memory_foundation_binding_required": True,
                    "uart_sha256": hashlib.sha256(uart_text.encode()).hexdigest()},
        "required prior strict memory/consumer proof")
    uart = UART(uart_text)
    uart.one("BEGIN", camera="v2.1.8", hash="fnv1a32")
    clock = _clock(uart, sensor_captures, controller_snapshots)
    sources = {}
    proven_sources = {row["frame"]: row for row in base_proof["evidence"]["source_captures"]}
    for item in sensor_captures:
        meta, data = item["metadata"], item["data"]
        _need(type(data) is bytes and meta["frame"] in proven_sources,
              "external source bytes absent from prior independent proof")
        verified = proven_sources[meta["frame"]]
        _need(len(data) == verified["bytes"] and hashlib.sha256(data).hexdigest() == verified["payload_sha256"],
              "external source payload changed after the strict independent oracle")
        sources[meta["frame"]] = data
    exact = []
    for frame in range(4):
        row = uart.one("CAM_FRAME", frame=str(frame), valid="1", width="160", height="120")
        buffer = uart.one("CAM_EXTERNAL_BUFFER", frame=str(frame), external="1", valid="1")
        address, length = _addr(buffer, "payload"), _u(row, "len")
        _rules._external(address, length)
        actual = []
        for state in controller_snapshots:
            host = state["_host_observation"]
            prefix = host["uart_prefix"]
            _need(host["paused"] is True and uart_text.startswith(prefix),
                  "external byte lease not tied to actual stopped UART prefix")
            if not re.search(rf"LCDCAM CAM_FRAME frame={frame}\b", prefix) or re.search(
                    rf"LCDCAM CAM_RETURN_BEGIN frame={frame}\b", prefix):
                continue
            for item in host.get("camera_buffers", []):
                if item["frame"] != frame:
                    continue
                _need(item["address"] == address and type(item["data"]) is bytes and
                      len(item["data"]) == length and item["raw_reads"],
                      "external actual full guest lease malformed")
                actual.append(item["data"])
        _need(actual and all(data == actual[0] for data in actual),
              "external public owned-buffer full bytes absent or changed")
        data = actual[0]
        _eq(row, {"hash": _rules._ref.fnv1a(data), "head": data[:2].hex(), "tail": data[-2:].hex()},
            "external actual payload FNV/tags")
        candidates = [identity for identity, source in sources.items() if source == data]
        _need(candidates, "external full guest payload differs byte-by-byte from every actual physical source capture")
        exact.append({"frame": frame, "payload": address, "bytes": length,
                      "sha256": hashlib.sha256(data).hexdigest(), "source_candidate_frames": candidates})
    proof = dict(base_proof)
    proof["actual_camera_ledc_xclk"] = clock
    proof["exact_guest_source_payloads"] = exact
    return proof
