"""Pure UART/QOM oracle for the five ordinary external LCD/CAM consumers.

No emulator/device encoder imports or reconstructed capture substitutes.
Success qualifies enumerated consumer observations, not unavailable continuous
write-address traces. The caller binds same-binary foundation provenance.
"""
import hashlib
import importlib.util
from pathlib import Path
import re
import sys

# Reuse the independently authored reference, not a renderer/sensor encoder.
_REFERENCE = Path(__file__).resolve().parents[1] / "lcd-cam" / "reference"
_spec = importlib.util.spec_from_file_location("_memory_capture_reference", _REFERENCE / "capture_reference.py")
_ref = importlib.util.module_from_spec(_spec)
sys.path.insert(0, str(_REFERENCE))
try:
    _spec.loader.exec_module(_ref)
finally:
    sys.path.remove(str(_REFERENCE))

PROFILES = {"lcd-rgb-double", "lcd-rgb-bounce", "lcd-camera-rgb565",
            "lcd-camera-yuv422", "lcd-camera-jpeg"}
_PATTERN = {"source": "color-bars", "scene": [400, 296],
            "crop": [0, 0, 400, 296], "mirror": False, "flip": False}
_LIMITS = [
    "Cache/backing qualification requires the runner's renewed same-final-binary twelve-case memory qtest and real SDK C2M/M2C receipt; consumer cache API logs are not a substitute.",
    "Compiled SDK/component/ELF/merged-image and shared SSI provenance must be bound by the runner; this pure oracle cannot establish it from UART/QOM.",
]


def _need(condition, message):
    if not condition:
        raise RuntimeError(message)


def _u(row, key):
    value = row[key]
    _need(isinstance(value, str) and re.fullmatch(r"[0-9]+", value), f"invalid UART integer {key}")
    return int(value)


def _addr(row, key):
    value = row[key]
    _need(isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]+", value), f"invalid pointer {key}")
    return int(value, 16)


def _external(address, size):
    _need(size > 0 and 0x3c000000 <= address < address + size <= 0x3e000000,
          "external span is not an S3 PSRAM CPU alias")


def _internal(address, size):
    _need(size > 0 and 0x3fc88000 <= address < address + size <= 0x3fd00000,
          "internal DMA span is outside S3 DRAM")


def _eq(row, expected, label):
    for key, value in expected.items():
        _need(key in row and type(row[key]) is type(value) and row[key] == value,
              f"{label}: {key} differs from required {value!r}")


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _artifact(data):
    return {"bytes": len(data), "sha256": _digest(data), "fnv1a32": _ref.fnv1a(data),
            "head": data[:3].hex(), "tail": data[-3:].hex()}


class _UART:
    def __init__(self, text):
        _need(isinstance(text, str) and len(text.encode("utf-8")) <= _ref.MAX_LOG,
              "UART text absent or exceeds bound")
        plain = re.sub(r"\x1b\[[0-9;]*m", "", text)
        _need(not re.search(r"(?m)^E \([0-9]+\)|Guru Meditation|assert failed|abort\(\)|"
                            r"Backtrace:|SPI SRAM memory test fail|Stack smashing", plain),
              "SDK/fixture reports an error, panic, or failed boot memory test")
        boot = [re.search(pattern, plain) for pattern in (
            r"esp_psram: Found 8MB PSRAM device",
            r"esp_psram: Speed: 40MHz",
            r"esp_psram: SPI SRAM memory test OK",
            r"esp_psram: Adding pool of ([0-9]+)K of PSRAM memory to heap allocator")]
        _need(all(boot) and int(boot[3][1]) > 0, "missing actual quad40 8MiB boot initialization/test/heap evidence")
        begin = plain.find("LCDCAM BEGIN ")
        _need(begin >= 0 and all(match.start() < begin for match in boot),
              "PSRAM boot checks were not observed before actual consumer execution")
        self.rows = []
        for line in text.splitlines():
            match = re.search(r"LCDCAM ([A-Z_0-9]+)(?:\s|$)", line)
            if not match:
                continue
            pairs = re.findall(r"([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)", line[match.end():])
            _need(len({key for key, _ in pairs}) == len(pairs), "duplicate UART field")
            row = dict(pairs)
            row["_index"] = len(self.rows)
            self.rows.append((match[1], row))
            _need(match[1] not in {"ERROR", "UNQUALIFIED", "CAM_TIMEOUT", "CAM_PHYSICAL_FAULT"},
                  f"fixture failure: {line}")
            if match[1] == "API":
                _eq(row, {"err": "ESP_OK", "code": "0"}, "API")
        _need(self.rows, "no LCDCAM UART records")
        self.one("PSRAM_REQUEST", initialized="1", capacity="8388608")
        _before(self.one("BEGIN", camera="v2.1.3", hash="fnv1a32"), self.one("END"))

    def all(self, tag, **filters):
        return [row for name, row in self.rows if name == tag and
                all(row.get(key) == value for key, value in filters.items())]

    def one(self, tag, **filters):
        rows = self.all(tag, **filters)
        _need(len(rows) == 1, f"required unique {tag} {filters}: found {len(rows)}")
        return rows[0]

    def api(self, op):
        return self.one("API", op=op)


def _before(*rows):
    _need(all(a["_index"] < b["_index"] for a, b in zip(rows, rows[1:])),
          "UART ownership/API ordering differs from actual required sequence")
    times = [_u(row, "time_us") for row in rows if "time_us" in row]
    _need(all(a <= b for a, b in zip(times, times[1:])), "UART timestamps move backwards")


def _controllers(snapshots, camera, byte_count):
    _need(isinstance(snapshots, list) and 2 <= len(snapshots) <= 100000,
          "need chronological initial and live/final controller snapshots")
    counters = ("lcd_words", "lcd_frames", "cam_bytes", "cam_frames", "tx_underflow",
                "rx_overflow", "invalid_edges", "dropped_frames", "last_lcd_eof_ns",
                "last_lcd_done_ns", "last_cam_frame_ns", "tx_fifo_bytes", "rx_fifo_bytes")
    for state in snapshots:
        _eq(state, {"profile": "s3-lcdcam-edge-functional-v1"}, "controller")
        for key in counters:
            _need(type(state[key]) is int and state[key] >= 0, f"invalid controller {key}")
        for key in ("lcd_running", "lcd_stalled"):
            _need(type(state[key]) is bool, f"invalid controller {key}")
        _need(state["tx_fifo_bytes"] <= 32 and state["rx_fifo_bytes"] <= 32,
              "controller FIFO outside bounded implementation")
        _need(state["rx_overflow"] == 0 and state["invalid_edges"] == 0 and state["dropped_frames"] == 0,
              "controller reports RX/electrical/frame failure")
    for a, b in zip(snapshots, snapshots[1:]):
        for key in counters[:7]:
            _need(a[key] <= b[key], f"controller lifetime counter {key} regressed")
    first, last = snapshots[0], snapshots[-1]
    if camera:
        _need(last["cam_bytes"] - first["cam_bytes"] >= byte_count and
              last["cam_frames"] - first["cam_frames"] >= 4 and last["last_cam_frame_ns"] > 0,
              "actual CAM RX/GDMA fulfilled bytes or completed frames insufficient")
    else:
        _need(all(state["tx_underflow"] == 0 for state in snapshots), "LCD TX underflow observed")
        _need(last["lcd_words"] - first["lcd_words"] >= 3072 and
              last["lcd_frames"] > first["lcd_frames"] and last["last_lcd_eof_ns"] > 0,
              "actual LCD OUT/GDMA words/EOF insufficient")
        _need(last["lcd_running"] is False, "LCD producer not stopped after deletion")
    return {key: last[key] - first[key] for key in counters[:8]}


def _panel(item):
    data, meta = item["rgb888"], item["metadata"]
    _need(type(data) is bytes and len(data) == 9216, "panel requires full actual RGB888 bytes")
    _eq(meta, {"version": 1, "kind": "lcd-panel-framebuffer", "component_id": "D",
               "format": "rgb888", "width": 64, "height": 48, "total_bytes": 9216,
               "valid": True, "visible": True, "errors": 0}, "panel")
    for key in ("sequence", "power_epoch", "start_ns", "end_ns"):
        _need(type(meta[key]) is int and meta[key] >= 0, f"invalid panel {key}")
    _need(isinstance(meta["config_identity"], str) and meta["config_identity"], "missing panel config identity")
    _need(meta["sha256_rgb888"] == _digest(data), "panel SHA is not over actual bytes")
    _need(meta["period_min_ns"] in (1000, 2000), "unqualified physical PCLK period")
    contract = {"width": 64, "height": 48, "bus_width": 16, "bits_per_pixel": 16,
                "memory_format": "RGB565-LE", "pattern": {"source": "lcd-fixture", "frame": 0},
                "pclk_period_ns": meta["period_min_ns"],
                "timing": {"hsync_pulse_width": 2, "h_back_porch": 4, "h_front_porch": 4,
                           "vsync_pulse_width": 2, "v_back_porch": 2, "v_front_porch": 2}}
    _, expected, duration = _ref.rgb_contract(contract)
    _eq(meta, expected, "panel physical timing")
    _need(meta["end_ns"] - meta["start_ns"] == duration, "panel frame duration differs from cadence")
    windows = item["windows"]
    _need(isinstance(windows, list) and 0 < len(windows) <= 128, "missing panel byte windows")
    joined = bytearray()
    excluded = {"offset", "count", "hex"}
    for window in windows:
        _need({k: v for k, v in window.items() if k not in excluded} ==
              {k: v for k, v in meta.items() if k not in excluded}, "immutable panel lease changed")
        _need(window["offset"] == len(joined) and type(window["count"]) is int and
              0 < window["count"] <= 65536, "panel window gap/overlap/bound")
        _need(isinstance(window["hex"], str) and len(window["hex"]) == 2 * window["count"],
              "panel hex field is not exactly the actual window byte count")
        raw = bytes.fromhex(window["hex"])
        _need(len(raw) == window["count"], "panel hex/count mismatch")
        joined.extend(raw)
    _need(bytes(joined) == data, "panel item bytes differ from actual windows")
    capture = item["capture"]
    _eq(capture, {"version": 1, "kind": "lcd-panel-capture"}, "panel metadata ring")
    matches = [row for row in capture["frames"] if row["sequence"] == meta["sequence"]]
    _need(len(matches) == 1 and all(meta[k] == v for k, v in matches[0].items()),
          "panel retained metadata does not bind byte snapshot")
    return meta, data


def _rgb(uart, items, bounce):
    _need(isinstance(items, list) and 2 <= len(items) <= 100000, "missing RGB capture series")
    expected_memory = {frame: _ref.expected565({"source": "lcd-fixture", "frame": frame}, 64, 48, True)
                       for frame in (0, 1)}
    expected_rgb = {frame: _ref.expected_rgb({"source": "lcd-fixture", "frame": frame}, 64, 48)
                    for frame in (0, 1)}
    frames = []
    identities = set()
    for item in items:
        meta, data = _panel(item)
        identities.add((meta["component_id"], meta["config_identity"], meta["power_epoch"]))
        matched = [f for f in ((0,) if bounce else (0, 1)) if data == expected_rgb[f]]
        _need(len(matched) == 1, "actual full panel bytes differ from independent RGB565 formula")
        frames.append(dict(frame=matched[0], sequence=meta["sequence"], start_ns=meta["start_ns"],
                           end_ns=meta["end_ns"], period_ns=meta["period_min_ns"], **_artifact(data)))
    _need(len(identities) == 1, "panel identity/epoch changed")
    _need(all(a["sequence"] < b["sequence"] and a["end_ns"] <= b["start_ns"]
              for a, b in zip(frames, frames[1:])), "panel captures reordered/duplicated")
    _need({f["period_ns"] for f in frames} == {1000, 2000}, "divider change lacks physical captures")
    _need(all(a["period_ns"] <= b["period_ns"] for a, b in zip(frames, frames[1:])),
          "physical divider reverted unexpectedly")
    for op in ("rgb_panel", "rgb_callbacks", "rgb_reset", "rgb_init", "pclk_divider", "restart", "rgb_delete"):
        uart.api(op)
    stopped = uart.one("RGB_STOPPED", driver_deleted="1")
    _before(uart.api("rgb_init"), uart.api("pclk_divider"), uart.api("restart"), uart.api("rgb_delete"), stopped)
    vsyncs = uart.all("RGB_VSYNC")
    _need(len(vsyncs) >= 5, "insufficient actual VSYNC observations")
    for row in vsyncs:
        _need(0 < _u(row, "callback_us") <= _u(row, "time_us"), "VSYNC callback timestamp invalid")
    _need(all(_u(a, "count") < _u(b, "count") for a, b in zip(vsyncs, vsyncs[1:])),
          "VSYNC callback count not advancing")
    for f in ((0,) if bounce else (0, 1)):
        row = uart.one("RGB_PAYLOAD", frame=str(f))
        raw = expected_memory[f]
        _eq(row, {"bus_width": "16", "bits_per_pixel": "16", "width": "64", "height": "48",
                  "stride": "128", "bytes": "6144", "hash": _ref.fnv1a(raw),
                  "head": raw[:3].hex(), "tail": raw[-3:].hex()}, "RGB payload")
    if bounce:
        source = uart.one("RGB_EXTERNAL_BUFFER", name="bounce_source")
        _eq(source, {"external": "1", "bytes": "6144", "mod64": "0"}, "bounce source")
        alias = _addr(source, "alias")
        _external(alias, 6144)
        _need(alias % 64 == 0 and _u(source, "allocated") >= 6144, "bounce source allocation invalid")
        wb = uart.one("RGB_CACHE", op="rgb_bounce_source_writeback", direction="C2M_WB")
        inv = uart.one("RGB_CACHE", op="rgb_bounce_source_readback", direction="M2C_INV")
        _before(source, uart.one("RGB_PAYLOAD", frame="0"), wb, inv, uart.api("rgb_init"))
        for row in (wb, inv):
            _need(_addr(row, "alias") == alias and _u(row, "bytes") == 6144, "bounce cache span mismatch")
            uart.api(row["op"])
        storage = uart.all("RGB_BOUNCE_STORAGE")
        payloads = uart.all("RGB_BOUNCE_PAYLOAD")
        _need(storage and payloads, "bounce actual ISR/copy evidence absent")
        pair = None
        for row in storage:
            _eq(row, {"distinct": "1", "errors": "0"}, "bounce buffers")
            current = (_addr(row, "bb0"), _addr(row, "bb1"))
            _need(current[0] != current[1] and abs(current[0] - current[1]) >= 512,
                  "bounce buffers overlap")
            for address in current:
                _internal(address, 512)
            _need(pair is None or current == pair, "callback bounce storage identity changed")
            pair = current
            _need(_u(row, "callback_count") >= _u(row, "isr_count"), "ISR count exceeds copies")
            _need(_u(row, "external_isr_bytes") == 512 * _u(row, "isr_count"),
                  "external ISR copied bytes differ from actual callback geometry")
        for previous, current in zip(storage, storage[1:]):
            _need(all(_u(previous, key) <= _u(current, key) for key in
                      ("callback_count", "isr_count", "external_isr_bytes")),
                  "actual bounce copy/ISR lifetime counters regressed")
            _before(previous, current)
        _need(_u(storage[-1], "isr_count") > 0 and _u(storage[-1], "external_isr_bytes") >= 6144,
              "startup CPU prefill is not an actual ISR frame")
        for row in payloads:
            _eq(row, {"bus_width": "16", "bits_per_pixel": "16", "width": "64", "height": "48",
                      "stride": "128", "bytes": "6144", "hash": _ref.fnv1a(expected_memory[0])}, "ISR payload")
            _need(_u(row, "complete_frames") > 0, "no contiguous bounce frame completed")
        detail = {"source_alias": source["alias"], "internal_bounce_aliases": list(pair),
                  "isr_copies": _u(storage[-1], "isr_count"),
                  "external_isr_bytes": _u(storage[-1], "external_isr_bytes")}
    else:
        _need({f["frame"] for f in frames} == {0, 1} and frames[0]["frame"] == 0,
              "full physical frame0 and frame1 must both be captured")
        _need(all(a["frame"] <= b["frame"] for a, b in zip(frames, frames[1:])), "RGB swap reversed")
        fb = uart.one("RGB_FRAMEBUFFERS", num_fbs="2", bytes="6144", distinct_spans="1", external_requested="1")
        addresses = [_addr(fb, key) for key in ("fb0", "fb1")]
        _need(abs(addresses[0] - addresses[1]) >= 6144, "driver framebuffer spans overlap")
        uart.api("rgb_framebuffers")
        for f, alias in enumerate(addresses):
            _external(alias, 6144)
            ext = uart.one("RGB_EXTERNAL_BUFFER", name=f"fb{f}", external="1", bytes="6144")
            _need(_addr(ext, "alias") == alias and _u(ext, "allocated") >= 6144,
                  "driver framebuffer allocation mismatch")
            wb = uart.one("RGB_CACHE", op=f"rgb_fb{f}_initial_writeback", direction="C2M_WB", bytes="6144")
            _need(_addr(wb, "alias") == alias, "initial writeback wrong buffer")
            _before(uart.one("RGB_PAYLOAD", frame=str(f)), wb, uart.api("rgb_init"))
            uart.api(wb["op"])
        swap = uart.one("RGB_SWAP_REQUEST")
        _need(_addr(swap, "old") == addresses[0] and _addr(swap, "next") == addresses[1], "swap buffer mismatch")
        draw_wb = uart.one("RGB_CACHE", op="rgb_fb1_before_draw", direction="C2M_WB", bytes="6144")
        _need(_addr(draw_wb, "alias") == addresses[1], "before-draw C2M wrong buffer")
        _before(draw_wb, swap, uart.api("rgb_swap"))
        uart.api("rgb_fb1_before_draw")
        ownership = uart.one("RGB_OWNERSHIP", old_retained_until_delete="1", overwrite_permitted="0")
        _need(_u(ownership, "reusable_events") > 0, "no actual reusable event after swap")
        previous = None
        retentions = []
        for phase in ("prepared", "before_swap", "after_swap_events", "before_stop"):
            row = uart.one("RGB_RETENTION", phase=phase)
            _eq(row, {"bytes": "6144", "before_hash": _ref.fnv1a(expected_memory[0]),
                      "after_hash": _ref.fnv1a(expected_memory[0]), "mismatches": "0", "bytes_equal": "1",
                      "head": expected_memory[0][:3].hex(), "tail": expected_memory[0][-3:].hex()}, "retention")
            _need(_addr(row, "alias") == addresses[0], "retention wrong old buffer")
            if previous:
                _before(previous, row)
            previous = row
            retentions.append(row)
        invalidations = uart.all("RGB_CACHE", op="rgb_old_fb_readback", direction="M2C_INV", bytes="6144")
        api_invalidations = uart.all("API", op="rgb_old_fb_readback")
        _need(len(invalidations) == len(api_invalidations) == 4, "retention actual API/M2C readbacks incomplete")
        for api, inv, retained in zip(api_invalidations, invalidations, retentions):
            _before(api, inv, retained)
            _need(_addr(inv, "alias") == addresses[0], "retention invalidation wrong span")
        _before(retentions[0], uart.api("rgb_init"), retentions[1], swap,
                ownership, retentions[2], uart.api("pclk_divider"), uart.api("restart"),
                retentions[3], uart.api("rgb_delete"))
        after = retentions[2]
        _need(_u(after, "vsync") > _u(swap, "vsync_before") and
              _u(after, "reusable") > _u(swap, "reusable_before") and
              _u(after, "vsync_us") >= _u(swap, "time_us") and
              _u(after, "reusable_us") >= _u(swap, "time_us"), "swap lacks actual new callback timestamps")
        detail = {"driver_framebuffer_aliases": addresses,
                  "retention_phases": [row["phase"] for row in retentions],
                  "overwrite_permitted": False, "lifetime": "retained_until_actual_panel_delete"}
    return {"frames": frames, **detail}, 6144


def _sensor(item, fmt):
    meta, data, windows = item["metadata"], item["data"], item["windows"]
    _need(type(data) is bytes and 0 < len(data) <= (278528 if fmt == "jpeg" else 38400), "sensor payload bound")
    _eq(meta, {"component_id": "D", "format": fmt, "width": 160, "height": 120,
               "source": "color-bars", "timing_profile": "ov2640-functional-dvp-v1",
               "scene_width": 400, "scene_height": 296, "crop_x": 0, "crop_y": 0,
               "crop_width": 400, "crop_height": 296, "mirror": False, "flip": False}, "sensor")
    lease = item["lease_status"]
    _eq(lease, {"component_id": "D", "config_identity": meta["config_identity"],
                "source": "color-bars", "timing_profile": "ov2640-functional-dvp-v1",
                "power_known": True, "powered": True, "sccb_ready": True, "failed": False,
                "power_epoch": meta["power_epoch"], "generation": meta["generation"],
                "capture_frame": meta["frame"], "capture_byte_count": len(data),
                "last_frame_hash_fnv1a64": meta["frame_hash_fnv1a64"]}, "stopped sensor lease")
    for key in ("frame", "power_epoch", "generation", "frame_start_ns", "frame_end_ns", "half_period_ns",
                "line_bytes", "line_count", "byte_count", "image_mode", "ctrl0"):
        _need(type(meta[key]) is int and meta[key] >= 0, f"sensor integer {key} invalid")
    _need(meta["config_identity"] and meta["frame"] > 0 and meta["half_period_ns"] > 0 and meta["line_bytes"] > 0,
          "sensor frame identity/timing invalid")
    _need(meta["byte_count"] == len(data) and meta["frame_hash_fnv1a64"] == _ref.fnv1a64(data),
          "sensor recorded count/hash differs from actual bytes")
    _need(isinstance(windows, list) and 0 < len(windows) <= 4096, "sensor actual lease windows missing")
    excluded = {"offset", "count", "bytes", "timestamps_ns"}
    immutable = {k: v for k, v in meta.items() if k not in excluded}
    joined = bytearray()
    times = []
    for window in windows:
        _need({k: v for k, v in window.items() if k not in excluded} == immutable,
              "sensor frame/power epoch/generation/config lease changed between windows")
        _need(window["offset"] == len(joined) and type(window["count"]) is int and 0 < window["count"] <= 4096,
              "sensor window gap/overlap/bound")
        raw, stamps = window["bytes"], window["timestamps_ns"]
        _need(isinstance(raw, list) and isinstance(stamps, list) and len(raw) == len(stamps) == window["count"] and
              all(type(v) is int and 0 <= v <= 255 for v in raw), "sensor raw byte window malformed")
        joined.extend(raw)
        times.extend(stamps)
    _need(bytes(joined) == data, "sensor item differs from actual window bytes")
    period, row_span = 2 * meta["half_period_ns"], meta["line_bytes"] + 16
    for index, stamp in enumerate(times):
        cycle = (8 + index // meta["line_bytes"]) * row_span + index % meta["line_bytes"]
        _need(type(stamp) is int and stamp == meta["frame_start_ns"] + cycle * period,
              "sensor latch timestamp differs from functional DVP cadence")
    _need(meta["line_count"] == (len(data) + meta["line_bytes"] - 1) // meta["line_bytes"] and
          meta["frame_end_ns"] == meta["frame_start_ns"] + ((meta["line_count"] + 8) * row_span - 1) * period,
          "sensor completed-frame timing mismatch")
    mode, ctrl = meta["image_mode"], meta["ctrl0"]
    if fmt == "rgb565":
        _need(mode & 0x1c == 8, "sensor RGB565 mode mismatch")
        encoding = "RGB565-LE" if mode & 1 else "RGB565-BE"
    elif fmt == "yuv422":
        _need(mode & 0x1c == 0, "sensor YUV422 mode mismatch")
        encoding = ("VYUY" if ctrl & 16 else "UYVY") if mode & 1 else ("YVYU" if ctrl & 16 else "YUYV")
    else:
        _need(mode & 16, "sensor JPEG mode mismatch")
        encoding = "JPEG"
    result = _ref.qualify(data, 160, 120, encoding, _PATTERN)
    result.update(frame=meta["frame"], frame_start_ns=meta["frame_start_ns"], frame_end_ns=meta["frame_end_ns"],
                  power_epoch=meta["power_epoch"], generation=meta["generation"], windows=len(windows))
    return meta, data, result


def _camera(uart, items, fmt):
    _need(isinstance(items, list) and 4 <= len(items) <= 100000, "camera needs at least four actual completed source captures")
    captures = [_sensor(item, fmt) for item in items]
    _need(len({(m["config_identity"], m["power_epoch"], m["generation"]) for m, _, _ in captures}) == 1,
          "sensor configuration/power/generation changed during consumer")
    _need(all(a[0]["frame"] < b[0]["frame"] and a[0]["frame_end_ns"] < b[0]["frame_start_ns"]
              for a, b in zip(captures, captures[1:])), "sensor captures duplicated/reordered")
    uart.one("CAM_EXTERNAL_REQUEST", profile="quad40", peri="5", direction="IN", buffers="2", qualification="not_claimed")
    uart.api("camera_init")
    uart.one("CAM_PROBE", pid="0026", ver="41", midh="7f", midl="a2")
    expected_format = {"rgb565": "0", "yuv422": "1", "jpeg": "4"}[fmt]
    capacity = 278528 if fmt == "jpeg" else 38400
    acquired, guest = [], []
    caches = uart.all("CAM_CACHE")
    cache_api = uart.all("API", op="camera_cache_M2C_invalidate")
    _need(len(caches) == len(cache_api) and len(caches) == 12, "all acquire/hold/return/rearm cache calls required")
    for api, cache in zip(cache_api, caches):
        _before(api, cache)
    for frame in range(4):
        f = str(frame)
        begin = uart.one("CAM_GET_BEGIN", frame=f)
        ext = uart.one("CAM_EXTERNAL_BUFFER", frame=f, external="1", valid="1")
        row = uart.one("CAM_FRAME", frame=f)
        _eq(row, {"width": "160", "height": "120", "format": expected_format, "valid": "1"}, "guest camera frame")
        _need(_u(ext, "capacity") == capacity and _u(ext, "len") == _u(row, "len") and
              0 < _u(row, "len") <= capacity, "guest camera bounded length/capacity mismatch")
        if fmt != "jpeg":
            _need(_u(row, "len") == 38400, "raw camera byte count differs from QQVGA")
        address = _addr(ext, "payload")
        _external(address, capacity)
        _need(address % 16 == 0 and _addr(ext, "handle") != 0, "camera payload alignment/handle invalid")
        _need(_u(ext, "timestamp_us") <= _u(row, "time_us"), "producer timestamp later than acquired frame")
        matching = [(m, data, result) for m, data, result in captures if
                    len(data) == _u(row, "len") and _ref.fnv1a(data) == row["hash"] and
                    data[:2].hex() == row["head"] and data[-2:].hex() == row["tail"]]
        _need(matching, "guest CAM RX length/FNV/tags do not correlate with actual source capture")
        acquire = uart.one("CAM_CACHE", frame=f, phase="acquire")
        _before(begin, ext, acquire, row)
        if fmt == "jpeg":
            decode = uart.one("CAM_DECODE", frame=f)
            _eq(decode, {"source_hash": row["hash"], "decoder": "fmt2rgb888", "sof": "baseline8_3component",
                         "width": "160", "height": "120", "output_type": "RGB888", "output_bytes": "57600",
                         "decoded": "1"}, "actual guest entropy decode")
            for key, length in (("hash", 8), ("head", 6), ("tail", 6)):
                _need(re.fullmatch(r"[0-9a-f]{" + str(length) + r"}", decode[key]), "decoded UART digest/tag invalid")
            _before(acquire, decode, row)
        acquired.append(ext)
        guest.append({"frame": frame, "payload": ext["payload"], "handle": ext["handle"],
                      "timestamp_us": _u(ext, "timestamp_us"), "bytes": _u(row, "len"), "fnv1a32": row["hash"],
                      "capacity": capacity,
                      "source_candidate_frames": [m["frame"] for m, _, _ in matching],
                      "source_frame_uniquely_bound": False})
        if fmt == "jpeg":
            guest[-1]["observed_cpu_decode"] = {key: decode[key] for key in
                                                ("hash", "head", "tail", "output_bytes", "decoder")}
            guest[-1]["guest_decoded_pixels_independently_compared"] = False
    _need(_addr(acquired[0], "handle") != _addr(acquired[1], "handle") and
          abs(_addr(acquired[0], "payload") - _addr(acquired[1], "payload")) >= capacity,
          "two camera held slots overlap")
    for cache in caches:
        frame = _u(cache, "frame")
        _need(frame < 4, "cache frame outside acquired range")
        payload = _addr(acquired[frame], "payload")
        start = payload & ~31
        size = (capacity + payload - start + 31) & ~31
        _eq(cache, {"direction": "M2C", "invalidate": "1"}, "camera cache")
        _need(_addr(cache, "payload") == payload and _u(cache, "capacity") == capacity and
              _addr(cache, "aligned_start") == start and _u(cache, "aligned_bytes") == size,
              "camera M2C is not the owned aligned capacity extent")
        _external(start, size)
    held = uart.one("CAM_TWO_HELD", requested="2", actual="2", distinct_handles_and_payloads="1", first_return_not_yet="1")
    hold_start = uart.one("CAM_HOLD_BEGIN", owned="2")
    hold_end = uart.one("CAM_HOLD_END", owned="2", interval_elapsed="1")
    interval = guest[1]["timestamp_us"] - guest[0]["timestamp_us"]
    _need(interval > 0 and _u(held, "interval_us") == interval and
          _u(hold_start, "interval_us") == interval and _u(hold_start, "requested_hold_us") >= interval and
          _u(hold_end, "elapsed_us") >= _u(hold_start, "requested_hold_us") and
          _u(hold_end, "time_us") - _u(hold_start, "time_us") == _u(hold_end, "elapsed_us"),
          "hold did not cover actual measured producer interval")
    _before(uart.one("CAM_FRAME", frame="0"), uart.one("CAM_GET_BEGIN", frame="1"),
            uart.one("CAM_FRAME", frame="1"), held, hold_start, hold_end)
    required_retains = {(0, "two_held_after_interval"), (1, "two_held_after_interval"),
                        (1, "other_slot_rearmed"), (2, "other_slot_rearmed"),
                        *((f, "before_return_rearm") for f in range(4))}
    retains = uart.all("CAM_RETAIN")
    _need(len(retains) == len(required_retains), "held retention observations incomplete/duplicated")
    for frame, phase in required_retains:
        retained = uart.one("CAM_RETAIN", frame=str(frame), phase=phase)
        inv = uart.one("CAM_CACHE", frame=str(frame), phase=phase)
        _eq(retained, {"before_hash": guest[frame]["fnv1a32"], "after_hash": guest[frame]["fnv1a32"],
                       "metadata_same": "1", "unchanged": "1"}, "camera retained slot")
        _before(inv, retained)
        if phase == "two_held_after_interval":
            _before(hold_end, inv, retained, uart.one("CAM_RETURN_BEGIN", frame="0"))
    for frame in range(4):
        begin = uart.one("CAM_RETURN_BEGIN", frame=str(frame))
        returned = uart.one("CAM_RETURN", frame=str(frame))
        _need(_addr(begin, "handle") == _addr(acquired[frame], "handle"), "return wrong held slot")
        _before(uart.one("CAM_RETAIN", frame=str(frame), phase="before_return_rearm"), begin, returned)
        if frame < 2:
            recovery = frame + 2
            resumed = uart.one("CAM_REARM", returned_frame=str(frame), recovery_frame=str(recovery),
                               same_returned_slot="1", timestamp_after_return="1", producer_resumed="1",
                               still_held_frame=str(recovery - 1))
            _need(acquired[frame]["handle"] == acquired[recovery]["handle"] and
                  acquired[frame]["payload"] == acquired[recovery]["payload"] and
                  guest[recovery]["timestamp_us"] > guest[frame]["timestamp_us"] and
                  guest[recovery]["timestamp_us"] >= _u(begin, "time_us"),
                  "recovery did not reuse returned slot with actual new producer timestamp")
            _before(returned, uart.one("CAM_GET_BEGIN", frame=str(recovery)),
                    uart.one("CAM_FRAME", frame=str(recovery)), resumed,
                    uart.one("CAM_RETAIN", frame=str(recovery - 1), phase="other_slot_rearmed"))
    _before(uart.one("CAM_RETURN", frame="0"), uart.one("CAM_RETURN", frame="1"),
            uart.one("CAM_RETURN", frame="2"), uart.one("CAM_RETURN", frame="3"),
            uart.api("camera_external_deinit"),
            uart.one("CAM_EXTERNAL_SUMMARY", requested_buffers="2", completed="1", no_fallback="1", qualification="not_claimed"))
    return {"source_captures": [result for _, _, result in captures], "guest_frames": guest,
            "held_interval_us": interval, "hold_elapsed_us": _u(hold_end, "elapsed_us"),
            "public_ownership_sequence_observed": True,
            "physical_absence_of_same_data_overwrite_qualified": False}, sum(g["bytes"] for g in guest)


def _gdma(snapshots, uart_text, evidence, camera, bounce):
    """Validate raw read-only MMIO/three-word descriptor observations.

    S3 LINK.PARK is IN bit24/OUT bit23. LINK.ADDR appends 20 bits to
    0x3fc80000; DW0 size/length/owner occupy 0:11/12:23/31.
    A stopped snapshot is evidence of that instant, not a continuous write log.
    """
    observed_slots, owned_completed_slots = set(), set()
    held_pauses, recovery_pauses, active_pauses = [], [], []
    prefix_length = 0
    direction = "IN" if camera else "OUT"
    if camera:
        spans = [(int(row["payload"], 16), row["capacity"], row["frame"])
                 for row in evidence["guest_frames"][:2]]
        capacity = evidence["guest_frames"][0]["capacity"]
    elif bounce:
        spans = [(address, 512, index) for index, address in enumerate(evidence["internal_bounce_aliases"])]
    else:
        spans = [(address, 6144, index) for index, address in enumerate(evidence["driver_framebuffer_aliases"])]
    descriptor_count = 0
    for observation_index, snapshot in enumerate(snapshots):
        host = snapshot["_host_observation"]
        _eq(host, {"paused": True}, "read-only observation lease")
        prefix = host["uart_prefix"]
        _need(isinstance(prefix, str) and uart_text.startswith(prefix) and len(prefix) >= prefix_length,
              "read-only observation UART prefix not actual chronological final UART")
        prefix_length = len(prefix)
        channels = host["gdma"]["channels"]
        _need(isinstance(channels, list) and len(channels) == 10, "need all five IN and OUT channel readouts")
        _need({(ch["channel"], ch["direction"]) for ch in channels} ==
              {(index, d) for index in range(5) for d in ("IN", "OUT")}, "GDMA channel readout set invalid")
        prefix_rows = [(tag, dict(re.findall(r"([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)", body)))
                       for tag, body in re.findall(r"LCDCAM ([A-Z_0-9]+)([^\r\n]*)", prefix)]
        held = any(tag == "CAM_TWO_HELD" for tag, _ in prefix_rows) and not any(
            tag == "CAM_RETURN_BEGIN" for tag, _ in prefix_rows)
        recovery = {int(row["recovery_frame"]) for tag, row in prefix_rows if tag == "CAM_REARM"}
        parked = True
        relevant = 0
        for channel in channels:
            _need(type(channel["channel"]) is int and 0 <= channel["channel"] < 5,
                  "GDMA channel index is not an actual S3 channel")
            regs, descriptors = channel["registers"], channel["descriptors"]
            for key in ("conf0", "conf1", "int_raw", "link", "state", "peri_sel", "suc_eof_desc", "desc", "bf0_desc", "bf1_desc"):
                _need(type(regs[key]) is int and 0 <= regs[key] <= 0xffffffff, f"GDMA raw register {key} invalid")
            _need(isinstance(descriptors, list) and len(descriptors) <= 1024, "descriptor walk exceeds bound")
            nodes = {}
            for node in descriptors:
                address, words = node["address"], node["words"]
                _need(type(address) is int and address % 4 == 0, "descriptor raw address invalid")
                _internal(address, 12)
                _need(isinstance(words, list) and len(words) == 3 and
                      all(type(word) is int and 0 <= word <= 0xffffffff for word in words), "descriptor words invalid")
                _need(address not in nodes, "descriptor walk duplicate")
                nodes[address] = words
            if channel["direction"] != direction or regs["peri_sel"] & 63 != 5:
                continue
            relevant += 1
            _need(not regs["int_raw"] & (12 if camera else 4), "actual peri5 GDMA raw descriptor/EOF error observed")
            park_bit = 24 if direction == "IN" else 23
            armed = not bool(regs["link"] & (1 << park_bit))
            parked &= not armed
            if camera:
                _need(regs["conf0"] & 16 == 0, "peri5 IN is configured for memory-to-memory instead of camera")
            if nodes:
                root = 0x3fc00000 | (regs["link"] & 0xfffff)
                _need(root in nodes, "GDMA LINK root omitted from real descriptor walk")
                for words in nodes.values():
                    if words[2]:
                        _internal(words[2], 12)
                        _need(words[2] in nodes, "bounded descriptor walk incomplete at DW2")
            if armed:
                _need(nodes, "actual active peri5 consumer lacks descriptor readout")
                active_pauses.append(observation_index)
            for address, words in nodes.items():
                dw0, payload, next_address = words
                size, length, owner = dw0 & 4095, (dw0 >> 12) & 4095, dw0 >> 31
                _need(0 < size <= 4095 and length <= size and not dw0 & (1 << 28),
                      "peri5 descriptor size/length/error fields invalid")
                width = size if camera else length
                if not width:
                    continue
                matching = [slot for base, capacity, slot in spans if base <= payload and payload + width <= base + capacity]
                _need(matching, "actual peri5 descriptor DW1 outside required public consumer payload span")
                if camera:
                    _external(payload, size)
                    block = (regs["conf1"] >> 13) & 3
                    _need(block <= 2 and payload % (16 << block) == 0 and size % (16 << block) == 0,
                          "external direct IN descriptor DMA block alignment invalid")
                    _need(size == (1024 if capacity == 278528 else 3840), "camera direct DMA node geometry differs")
                elif bounce:
                    _internal(payload, length)
                else:
                    _external(payload, length)
                observed_slots.update(matching)
                if camera and owner == 0 and length > 0:
                    owned_completed_slots.update(matching)
                descriptor_count += 1
        if held:
            _need(relevant >= 1 and parked, "two-held camera ownership pause lacks parked peri5 producer")
            _need(owned_completed_slots == {0, 1}, "both held slots need completed owner0 observations before any return")
            held_pauses.append(observation_index)
        if recovery:
            recovery_pauses.append((observation_index, sorted(recovery), snapshot["cam_frames"], snapshot["cam_bytes"]))
    _need(observed_slots == {0, 1} and active_pauses, "actual peri5 descriptor observations did not cover both consumer slots")
    if camera:
        _need(owned_completed_slots == {0, 1} and held_pauses, "held slots need real completed owner0 descriptors and pre-return park observation")
        _need(recovery_pauses and recovery_pauses[-1][1] == [2, 3],
              "read-only ownership observations lack both actual recoveries")
        held_state = snapshots[held_pauses[-1]]
        _need(recovery_pauses[-1][2] > held_state["cam_frames"] and
              recovery_pauses[-1][3] > held_state["cam_bytes"], "returned-slot producer has no actual controller progress")
    return {"direction": direction, "peripheral": 5, "internal_descriptors_observed": True,
            "public_ownership_park_owner_restart_observations_qualified": camera,
            "consumer_slots_observed": sorted(observed_slots), "descriptor_observations": descriptor_count,
            "active_observation_count": len(active_pauses), "two_held_park_observation_count": len(held_pauses),
            "continuous_write_address_trace": False}


def verify(profile: str, uart_text: str, panel_frames: list, sensor_captures: list,
           controller_snapshots: list) -> dict:
    """Raise RuntimeError on any missing/malformed/failing required observation.

    A PASS report covers consumer observations only. The runner MUST bind an
    actual renewed same-final-binary memory foundation receipt before accepting
    the combined functional qualification. Stronger physical/pixel gaps remain.
    """
    try:
        _need(profile in PROFILES, f"unknown consumer profile {profile!r}")
        uart = _UART(uart_text)
        camera = profile.startswith("lcd-camera-")
        limits = list(_LIMITS)
        if camera:
            _need(panel_frames == [], "unexpected RGB capture in camera lane")
            evidence, count = _camera(uart, sensor_captures, profile.removeprefix("lcd-camera-"))
            limits.extend([
                "Static hashes alone cannot exclude identical-value writes. Actual two-held peri5 PARK, direct descriptors/completed owner0 and returned-slot controller progress are required; absence of every same-value write between snapshots is not claimed.",
                "Guest producer timestamps and sensor virtual nanoseconds lack an independent timebase binding. Source candidate frames are retained, but exact source-frame identity is not qualified.",
            ])
            if profile.endswith("jpeg"):
                limits.append("Independent Pillow entropy-decodes actual captured JPEG and compares source pixels with bounded loss; guest fmt2rgb888 success/output hash/tags are observed, but no guest RGB888 bytes exist for independent pixel comparison.")
        else:
            _need(sensor_captures == [], "unexpected sensor capture in RGB lane")
            evidence, count = _rgb(uart, panel_frames, profile.endswith("bounce"))
            limits.append("Full panel bytes, direct real peri5 OUT descriptors and old-frame retention are observed; shared cache/backing qualification is bound separately by the mandatory same-binary memory foundation receipt.")
            limits.append("Stable physical 1MHz/500kHz captures show divider behavior; exact API-to-physical VSYNC/restart time binding is not qualified without a guest/virtual timebase receipt.")
        controller = _controllers(controller_snapshots, camera, count)
        gdma = _gdma(controller_snapshots, uart_text, evidence, camera, profile.endswith("bounce"))
        return {"schema_version": 1, "profile": profile, "status": "PASS",
                "qualification_scope": "consumer_observations_requires_same_binary_memory_foundation",
                "required_consumer_observations_qualified": True,
                "same_binary_memory_foundation_binding_required": True,
                "uart_sha256": _digest(uart_text.encode("utf-8")),
                "controller_counter_deltas": controller,
                "read_only_gdma": gdma,
                "controller_consumer_path": "actual_LCD_CAM_RX_linked_GDMA_peri5_IN" if camera else
                                            "actual_LCD_CAM_TX_linked_GDMA_peri5_OUT",
                "evidence": evidence, "limitations": limits}
    except RuntimeError:
        raise
    except (KeyError, TypeError, ValueError, OverflowError, IndexError) as exc:
        raise RuntimeError(f"{profile}: missing or malformed actual consumer evidence: {exc}") from exc
