#!/usr/bin/env python3
"""Compare real host-saved native capture files against independent image rules.

Usage and acquisition contracts are in provenance.json. No simulator launch,
MMIO write, injected RX data, encoder import, or generated capture is performed.
A successful source-emission comparison is explicitly NOT CAM RX qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from image_reference import (MAX_PAYLOAD, ReferenceError, compare_jpeg, dimensions,
                             expected332, expected422, expected565, expected_rgb, require)
from st7789_reference import St7789

MAX_JSON = 32 * 1024 * 1024
MAX_LOG = 8 * 1024 * 1024


def read_bounded(path, maximum):
    with Path(path).open('rb') as stream:
        payload = stream.read(maximum + 1)
    require(len(payload) <= maximum, f'{path}: exceeds {maximum} byte input bound')
    return payload


def load_json(path):
    try:
        return json.loads(read_bounded(path, MAX_JSON))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ReferenceError(f'{path}: invalid JSON: {exc}') from exc


def sha256(payload):
    return hashlib.sha256(payload).hexdigest()


def fnv1a(payload):
    value = 2166136261
    for byte in payload:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return f'{value:08x}'


def exact(actual, expected, label):
    require(len(actual) == len(expected), f'{label}: byte count {len(actual)} != {len(expected)}')
    if actual != expected:
        index = next(i for i, (a, e) in enumerate(zip(actual, expected)) if a != e)
        raise ReferenceError(f'{label}: first mismatch at byte {index}: '
                             f'0x{actual[index]:02x} != 0x{expected[index]:02x}; '
                             f'actual={sha256(actual)} expected={sha256(expected)}')


def qualify(payload, width, height, encoding, pattern, max_error=12, mean_error=3):
    dimensions(width, height)
    result = {'width': width, 'height': height, 'format': encoding,
              'bytes': len(payload), 'payload_sha256': sha256(payload), 'fnv1a32': fnv1a(payload)}
    if encoding == 'JPEG':
        decoded, errors = compare_jpeg(payload, width, height, pattern, max_error, mean_error)
        result.update(errors)
        result.update(comparison='independent-lossy-decode', decoded_rgb_sha256=sha256(decoded))
    else:
        if encoding == 'RGB888':
            expected = expected_rgb(pattern, width, height)
        elif encoding == 'RGB332':
            expected = expected332(pattern, width, height)
        elif encoding in ('RGB565-BE', 'RGB565-LE'):
            expected = expected565(pattern, width, height, encoding.endswith('LE'))
        else:
            expected = expected422(pattern, width, height, encoding)
        exact(payload, expected, encoding)
        result.update(comparison='exact-independent-bytes', expected_sha256=sha256(expected))
    return result


def path_value(value, path):
    for key in path.split('/'):
        if not key:
            continue
        value = value[int(key)] if isinstance(value, list) else value[key]
    return value


def evidence_plan(path):
    """Compare unmodified saved status/capture JSON, with explicit expected paths.

    A plan never creates device data. Counts/field names/timestamps must be given
    by the capture owner. All exact/bounded assertions are required, not hints.
    Relative files resolve relative to the plan. Hashes bind checks to artifacts.
    """
    plan = load_json(path)
    require(plan.get('version') == 1 and isinstance(plan.get('documents'), dict), 'bad evidence plan')
    require(0 < len(plan['documents']) <= 64, 'evidence document count outside bounds')
    base, docs, hashes = Path(path).resolve().parent, {}, {}
    for name, filename in plan['documents'].items():
        file = base / filename
        docs[name] = load_json(file)
        hashes[name] = sha256(read_bounded(file, MAX_JSON))
    assertions = plan.get('assertions', [])
    require(0 < len(assertions) <= 4096, 'evidence assertion count outside bounds')
    for index, check in enumerate(assertions):
        value = path_value(docs[check['document']], check['path'])
        if 'equals' in check:
            require(type(value) is type(check['equals']) and value == check['equals'],
                    f'evidence assertion {index}: exact value mismatch')
        else:
            require(type(value) is int and 'min' in check and 'max' in check
                    and check['min'] <= value <= check['max'],
                    f'evidence assertion {index}: count/timestamp outside explicit bounds')
    relations = plan.get('time_relations', [])
    require(len(relations) <= 4096, 'too many timestamp relations')
    for index, relation in enumerate(relations):
        a = path_value(docs[relation['from']['document']], relation['from']['path'])
        b = path_value(docs[relation['to']['document']], relation['to']['path'])
        require(type(a) is int and type(b) is int and a >= 0 and b >= 0
                and relation['min_delta'] <= b - a <= relation['max_delta'],
                f'timestamp relation {index}: delta outside explicit bounds')
    return {'comparison': 'explicit-real-artifact-assertions', 'documents_sha256': hashes,
            'assertions': len(assertions), 'time_relations': len(relations)}


def uart_records(path):
    raw = read_bounded(path, MAX_LOG)
    text = raw.decode('utf-8', errors='replace')
    records = []
    for line in text.splitlines():
        match = re.search(r'LCDCAM ([A-Z_0-9]+)(?:\s|$)', line)
        if match:
            fields = dict(re.findall(r'([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)', line[match.end():]))
            records.append((match.group(1), fields))
    require(records, 'UART contains no LCDCAM evidence')
    require(not any(tag in ('ERROR', 'UNQUALIFIED') for tag, _ in records),
            'UART reports an error or an explicitly unqualified fixture')
    for tag, row in records:
        if tag == 'API' and row.get('err') != 'ESP_OK':
            require(row.get('op') == 'restart_demand_expected_invalid_state'
                    and row.get('err') == 'ESP_ERR_INVALID_STATE',
                    f'UART reports failed API {row.get("op")}: {row.get("err")}')
    return records, sha256(raw)


def cam_uart(path, payload, width, height, frame, pixel_format):
    records, digest = uart_records(path)
    selected = [(tag, row) for tag, row in records if row.get('frame') == str(frame)
                and tag in ('CAM_GET_BEGIN', 'CAM_FRAME', 'CAM_RETURN', 'CAM_TIMEOUT')]
    require([tag for tag, _ in selected] == ['CAM_GET_BEGIN', 'CAM_FRAME', 'CAM_RETURN'],
            'CAM ownership must be GET_BEGIN -> one FRAME -> one RETURN without timeout')
    row = selected[1][1]
    want = {'width': str(width), 'height': str(height), 'format': str(pixel_format),
            'len': str(len(payload)), 'hash': fnv1a(payload), 'valid': '1',
            'head': payload[:2].hex(), 'tail': payload[-2:].hex()}
    require(all(row.get(key) == value for key, value in want.items()),
            'guest CAM_FRAME bytes/hash/format/dimensions differ from actual capture')
    times = [int(row['time_us']) for _, row in selected]
    require(0 <= times[0] <= times[1] <= times[2], 'CAM ownership timestamps decrease')
    return {'uart_sha256': digest, 'frame': frame, 'guest_fnv1a32': want['hash'],
            'get_us': times[0], 'frame_us': times[1], 'return_us': times[2],
            'qualification': 'guest-fnv-and-count-correlated-not-exact-guest-byte-capture'}


def rgb_contract(contract):
    """Declared fixture memory, logical pixels and IDF wire-cycle timing; no defaults."""
    width, height = contract['width'], contract['height']
    dimensions(width, height)
    require(width <= 1024 and height <= 1024, 'RGB dimensions exceed native panel bounds')
    bus, bpp = contract['bus_width'], contract['bits_per_pixel']
    require(type(bus) is int and bus in (8, 16), 'RGB bus_width must be integer 8 or 16')
    require(type(bpp) is int and bpp in (8, 16, 24), 'RGB bits_per_pixel must be integer 8, 16 or 24')
    require(bus == 8 or bpp in (16, 24), 'eight-bit RGB pixels require an eight-wire bus')
    require(width * bpp % bus == 0, 'RGB line must occupy whole wire cycles')
    encoding = contract['memory_format']
    formats = {8: ('RGB332', 'lcd-rgb332-fixture'),
               16: ('RGB565-LE', 'lcd-fixture'), 24: ('RGB888', 'lcd-rgb888-fixture')}
    require(encoding == formats[bpp][0] and contract['pattern']['source'] == formats[bpp][1],
            'RGB memory order/pattern is not the explicitly declared firmware profile')
    timing = contract['timing']
    keys = ('hsync_pulse_width', 'h_back_porch', 'h_front_porch',
            'vsync_pulse_width', 'v_back_porch', 'v_front_porch')
    require(set(timing) == set(keys), 'RGB timing requires all six explicit porch/pulse fields')
    for key in keys:
        require(type(timing[key]) is int and
                (1 if key.endswith('pulse_width') else 0) <= timing[key] <= 4096,
                f'{key}: outside native timing bound')
    period = contract['pclk_period_ns']
    require(type(period) is int and period > 0, 'RGB PCLK period must be positive integer ns')
    active = width * bpp // bus
    line = timing['hsync_pulse_width'] + timing['h_back_porch'] + active + timing['h_front_porch']
    lines = timing['vsync_pulse_width'] + timing['v_back_porch'] + height + timing['v_front_porch']
    metadata = {'bits_per_pixel': bpp, 'bus_width': bus, 'pixels': width * height,
                'pclk_samples': line * lines, 'lines': lines,
                'period_min_ns': period, 'period_max_ns': period,
                'observed_width': width, 'observed_wire_width': active, 'observed_height': height,
                'observed_hsync_width': timing['hsync_pulse_width'],
                'observed_h_back_porch': timing['h_back_porch'],
                'observed_h_front_porch': timing['h_front_porch'],
                'observed_vsync_width': timing['vsync_pulse_width'],
                'observed_v_back_porch': timing['v_back_porch'],
                'observed_v_front_porch': timing['v_front_porch']}
    return encoding, metadata, line * lines * period


def rgb_memory(contract):
    encoding, _, _ = rgb_contract(contract)
    if encoding == 'RGB332':
        return expected332(contract['pattern'], contract['width'], contract['height'])
    if encoding == 'RGB565-LE':
        return expected565(contract['pattern'], contract['width'], contract['height'], True)
    return expected_rgb(contract['pattern'], contract['width'], contract['height'])


def panel_uart(path, contract):
    records, digest = uart_records(path)
    frame = contract['pattern']['frame']
    memory = (expected565(contract['pattern'], contract['width'], contract['height'], True)
              if contract['lane'] == 'i80' else rgb_memory(contract))
    if contract['lane'] == 'i80':
        selected = [(tag, row) for tag, row in records if row.get('frame') == str(frame)
                    and tag in ('I80_SUBMIT', 'I80_DONE')]
        require([tag for tag, _ in selected] == ['I80_SUBMIT', 'I80_DONE'],
                'I80 requires exactly one submit then EOF completion per selected frame')
        submit, done = selected[0][1], selected[1][1]
        require(submit.get('width') == str(contract['width']) and
                submit.get('height') == str(contract['height']) and
                submit.get('bytes') == str(len(memory)) and submit.get('hash') == fnv1a(memory)
                and submit.get('wire') == 'rgb565-be', 'I80 submitted pattern/count/hash/order mismatch')
        begin, callback, end = int(submit['time_us']), int(done['callback_us']), int(done['time_us'])
        require(0 <= begin <= callback <= end, 'I80 EOF/ownership timestamps out of order')
        require(frame + 1 <= int(done['callbacks']) <= 3, 'I80 callback count differs from queued frames')
        return {'uart_sha256': digest, 'submitted_fnv1a32': fnv1a(memory),
                'submit_us': begin, 'eof_callback_us': callback, 'done_us': end}
    require(contract['lane'] in ('rgb-double', 'rgb-bounce', 'rgb-demand'), 'unknown panel ownership lane')
    payloads = [row for tag, row in records if tag == 'RGB_PAYLOAD' and row.get('frame') == str(frame)]
    if contract['lane'] != 'rgb-bounce':
        require(payloads, 'RGB lacks selected actual raw-memory payload evidence')
        for row in payloads:
            expected = {'bus_width': contract['bus_width'], 'bits_per_pixel': contract['bits_per_pixel'],
                        'width': contract['width'], 'height': contract['height'],
                        'stride': contract['width'] * contract['bits_per_pixel'] // 8,
                        'bytes': len(memory)}
            require(all(row.get(key) == str(value) for key, value in expected.items())
                    and row.get('hash') == fnv1a(memory)
                    and row.get('head') == memory[:3].hex() and row.get('tail') == memory[-3:].hex(),
                    'RGB submitted raw-memory profile/count/stride/hash/head/tail mismatch')
    vsyncs = [row for tag, row in records if tag == 'RGB_VSYNC']
    require(vsyncs, 'RGB lacks actual VSYNC callback evidence')
    counts = [int(row['count']) for row in vsyncs]
    times = [int(row['time_us']) for row in vsyncs]
    require(all(b >= a for a, b in zip(times, times[1:])) and
            all(b >= a for a, b in zip(counts, counts[1:])) and counts[-1] > 0,
            'RGB cumulative VSYNC count/log time decreases')
    if contract['lane'] == 'rgb-double':
        ownership = [row for tag, row in records if tag == 'RGB_OWNERSHIP']
        require(len(ownership) == 1 and int(ownership[0]['released_events']) > 0
                and ownership[0]['old_buffer_unmodified'] == '1', 'RGB old-buffer ownership not released')
        require(times[0] <= int(ownership[0]['time_us']) <= times[-1],
                'RGB ownership release timestamp outside observed callback interval')
    elif contract['lane'] == 'rgb-bounce':
        require(any(int(row['bounce']) > 0 for row in vsyncs), 'RGB bounce producer callback absent')
        require(frame == 0, 'RGB bounce producer declares only fixture frame zero')
        bounced = [row for tag, row in records if tag == 'RGB_BOUNCE_PAYLOAD']
        require(bounced, 'RGB bounce lacks actual callback-produced complete payload evidence')
        expected = {'bus_width': contract['bus_width'], 'bits_per_pixel': contract['bits_per_pixel'],
                    'width': contract['width'], 'height': contract['height'],
                    'stride': contract['width'] * contract['bits_per_pixel'] // 8, 'bytes': len(memory)}
        require(all(all(row.get(key) == str(value) for key, value in expected.items())
                    and row.get('hash') == fnv1a(memory) for row in bounced),
                'RGB bounce callback raw-memory profile/count/stride/hash mismatch')
    else:
        refreshes = [row for tag, row in records if tag == 'RGB_PAYLOAD' and row.get('frame') == str(frame)]
        require(len(refreshes) == 1, 'RGB demand-refresh payload is absent or duplicated')
    return {'uart_sha256': digest, 'pattern_fnv1a32': fnv1a(memory),
            'vsync_callbacks_observed': counts, 'vsync_timestamps_us': times}


def panel_fault(path, contract):
    bundle = load_json(path)
    before, after = qom_value(bundle['before']), qom_value(bundle['after'])
    require(contract['fault'] in ('wrong-dc', 'wrong-bus', 'disconnect'), 'unsupported panel wire fault')
    require(before['component_id'] == after['component_id'] == contract['component_id']
            and before['config_identity'] == after['config_identity']
            and before['power_epoch'] == after['power_epoch'], 'fault comparison crossed identity/power epoch')
    require(after['captures'] - before['captures'] == contract['captures_delta'],
            'fault produced unexpected completed physical-panel captures')
    deltas = contract['error_deltas']
    require(isinstance(deltas, dict) and deltas and
            any(bound['min'] > 0 for bound in deltas.values()), 'fault contract must require observed errors')
    require(contract['start_ns'] <= contract['end_ns'], 'fault timestamp interval reversed')
    observed = {}
    for category, bound in deltas.items():
        delta = after['errors'][category] - before['errors'][category]
        require(bound['min'] <= delta <= bound['max'], f'fault {category} counter delta outside contract')
        if delta:
            timestamp = after['error_last_ns'][category]
            require(type(timestamp) is int and contract['start_ns'] <= timestamp <= contract['end_ns'],
                    f'fault {category} lacks matching actual electrical error timestamp')
        observed[category] = delta
    require(after['error_total'] >= before['error_total'] + sum(observed.values()),
            'fault aggregate error count inconsistent with categories')
    return {'fault': contract['fault'], 'error_deltas': observed,
            'captures_delta': contract['captures_delta'],
            'artifact_sha256': sha256(read_bounded(path, MAX_JSON)),
            'qualification': 'actual-panel-error-contract-not-synthetic-fault-coverage'}


def wire_capture(args):
    # The input must be a recorder's actual fully resolved rising-edge samples.
    # It is an optional independent decode path, not a claim that QOM records wires.
    document = load_json(args.capture)
    events = document['events']
    require(document.get('version') == 1 and 0 < len(events) <= 100000,
            'bounded I80 resolved-edge document invalid')
    decoder = St7789(args.width, args.height, args.bus_width)
    frames, previous = [], -1
    for event in events:
        timestamp = event['timestamp_ns']
        require(type(timestamp) is int and timestamp >= previous, 'wire timestamps decrease')
        previous = timestamp
        frame = decoder.sample(timestamp, event['dc'], event['value'], event['known_mask'],
                               event['cs'], event['reset'], event['rd'])
        if frame:
            require(len(frames) < 64, 'wire capture exceeds 64 completed-frame bound')
            rgb = frame.pop('rgb')
            pattern = {'source': 'lcd-fixture', 'frame': len(frames)}
            frame.update(qualify(rgb, args.width, args.height, 'panel-RGB888', pattern))
            frames.append(frame)
    decoder.finish()
    require(len(frames) == args.frames, 'wire completed frame count mismatch')
    return {'capture_sha256': sha256(read_bounded(args.capture, MAX_JSON)), 'frames': frames,
            'resolved_wr_edges': len(events), 'qualification': 'actual-wire-decode-only'}


def qom_value(value):
    """Unwrap a real qom-get reply or its directly saved JSON-string value."""
    if isinstance(value, dict) and 'return' in value:
        value = value['return']
    if isinstance(value, str):
        value = json.loads(value)
    require(isinstance(value, dict), 'QOM capture value is not a JSON object')
    return value


def expected_metadata(meta, expected):
    for key, want in expected.items():
        got = meta[key]
        if isinstance(want, dict):
            require(set(want) == {'min', 'max'} and type(got) is int
                    and want['min'] <= got <= want['max'], f'{key}: outside explicit bounds')
        else:
            require(type(got) is type(want) and got == want, f'{key}: differs from expected metadata')


def panel_qom(path, contract):
    bundle = load_json(path)
    capture = qom_value(bundle['capture'])
    require(capture.get('version') == 1 and capture.get('kind') == 'lcd-panel-capture',
            'unsupported panel QOM capture')
    sequence = contract['sequence']
    records = [row for row in capture['frames'] if row['sequence'] == sequence]
    require(len(records) == 1, 'selected panel sequence absent/duplicated in actual retained metadata')
    meta = records[0]
    require(meta['valid'] is True and meta['visible'] is True and meta['errors'] == 0,
            'panel capture is malformed, invisible or electrically invalid')
    require(type(meta['start_ns']) is int and type(meta['end_ns']) is int
            and 0 <= meta['start_ns'] <= meta['end_ns'], 'panel capture timestamps invalid')
    expected_metadata(meta, contract['metadata'])
    if contract['lane'] != 'i80':
        _, timing_metadata, frame_duration = rgb_contract(contract)
        expected_metadata(meta, timing_metadata)
        expected_metadata(capture['status'], {'component_id': contract['component_id'],
                          'model': 'rgb-panel', 'width': contract['width'], 'height': contract['height'],
                          'bus_width': contract['bus_width'], 'bits_per_pixel': contract['bits_per_pixel'],
                          'pixel_stream_profile': {8: 'rgb332', 16: 'rgb565-le', 24: 'rgb888-r-g-b'}[contract['bits_per_pixel']]})
        require(meta['end_ns'] - meta['start_ns'] == frame_duration,
                'RGB frame duration differs from independent wire-cycle cadence')
    if 'frame_duration_ns' in contract:
        expected_metadata({'frame_duration_ns': meta['end_ns'] - meta['start_ns']},
                          {'frame_duration_ns': contract['frame_duration_ns']})
    windows = bundle['windows']
    require(isinstance(windows, list) and 0 < len(windows) <= 128, 'panel window count outside bound')
    payload = bytearray()
    for saved in windows:
        window = qom_value(saved)
        require(window.get('version') == 1 and window.get('kind') == 'lcd-panel-framebuffer'
                and window.get('format') == 'rgb888', 'unsupported panel framebuffer window')
        require(window['sequence'] == sequence and window['component_id'] == contract['component_id'],
                'panel frame identity changed between windows')
        if contract['lane'] != 'i80':
            require(window['config_identity'] == capture['status']['config_identity'],
                    'RGB framebuffer immutable config identity differs from capture status')
        require(all(window[key] == value for key, value in meta.items()),
                'panel immutable metadata changed between windows')
        require(window['width'] == contract['width'] and window['height'] == contract['height'],
                'panel dimensions differ from expected canvas')
        require(window['offset'] == len(payload) and type(window['count']) is int
                and 0 < window['count'] <= 65536, 'panel window overlaps, skips or exceeds bound')
        require(window['total_bytes'] == 3 * contract['width'] * contract['height'],
                'panel total byte count differs from dimensions')
        require(isinstance(window['hex'], str) and len(window['hex']) == 2 * window['count'],
                'panel hex length does not match count')
        raw = bytes.fromhex(window['hex'])
        require(len(raw) == window['count'], 'panel decoded hex count mismatch')
        payload.extend(raw)
        require(len(payload) <= window['total_bytes'] <= MAX_PAYLOAD, 'panel byte windows exceed bound')
    require(len(payload) == 3 * contract['width'] * contract['height'], 'panel snapshot incomplete')
    require(sha256(payload) == meta['sha256_rgb888'], 'panel metadata SHA256 differs from actual windows')
    result = qualify(payload, contract['width'], contract['height'], 'RGB888', contract['pattern'])
    result.update(sequence=sequence, metadata=meta, windows=len(windows),
                  artifact_sha256=sha256(read_bounded(path, MAX_JSON)),
                  qualification='actual-physical-panel-snapshot')
    return result


def fnv1a64(payload):
    value = 14695981039346656037
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f'{value:016x}'


def sensor_qom(path, contract):
    bundle = load_json(path)
    windows = bundle['windows']
    require(isinstance(windows, list) and 0 < len(windows) <= 4096, 'sensor window count outside bound')
    payload, timestamps, meta = bytearray(), [], None
    excluded = {'offset', 'count', 'bytes', 'timestamps_ns'}
    for saved in windows:
        window = qom_value(saved)
        current = {key: value for key, value in window.items() if key not in excluded}
        if meta is None:
            meta = current
        require(current == meta, 'sensor frame/epoch/generation or metadata changed between windows')
        require(window['component_id'] == contract['component_id'] and window['frame'] == contract['frame'],
                'sensor snapshot identity differs from expected')
        require(window['offset'] == len(payload) and type(window['count']) is int
                and 0 < window['count'] <= 4096, 'sensor window overlaps, skips or exceeds bound')
        raw, times = window['bytes'], window['timestamps_ns']
        require(isinstance(raw, list) and isinstance(times, list)
                and len(raw) == len(times) == window['count'], 'sensor byte/timestamp count mismatch')
        require(all(type(value) is int and 0 <= value <= 255 for value in raw), 'invalid sensor byte')
        require(all(type(value) is int and 0 <= value < (1 << 64) for value in times),
                'invalid sensor latch timestamp')
        payload.extend(raw)
        timestamps.extend(times)
        require(len(payload) <= meta['byte_count'] <= MAX_PAYLOAD, 'sensor windows exceed frame bound')
    require(len(payload) == meta['byte_count'] and payload, 'sensor completed snapshot is incomplete/empty')
    require(fnv1a64(payload) == meta['frame_hash_fnv1a64'], 'sensor recorded frame hash differs from bytes')
    require(meta['width'] == contract['width'] and meta['height'] == contract['height'],
            'sensor dimensions differ from expected')
    expected_metadata(meta, contract['metadata'])
    require(meta['timing_profile'] == 'ov2640-functional-dvp-v1', 'unqualified sensor timing profile')
    require(0 <= meta['frame_start_ns'] <= timestamps[0] <= timestamps[-1] <= meta['frame_end_ns'],
            'sensor latch times lie outside frame timestamps')
    require(all(b > a for a, b in zip(timestamps, timestamps[1:])), 'sensor latch timestamps not increasing')
    require(type(meta['half_period_ns']) is int and meta['half_period_ns'] > 0
            and type(meta['line_bytes']) is int and meta['line_bytes'] > 0, 'invalid sensor timing metadata')
    period = 2 * meta['half_period_ns']
    row_span = meta['line_bytes'] + 16
    for index, timestamp in enumerate(timestamps):
        cycle = (8 + index // meta['line_bytes']) * row_span + index % meta['line_bytes']
        require(timestamp == meta['frame_start_ns'] + cycle * period,
                f'sensor functional-profile latch timestamp differs at byte {index}')
    require(meta['line_count'] == (len(payload) + meta['line_bytes'] - 1) // meta['line_bytes'],
            'sensor line count differs from actual byte windows')
    require(meta['frame_end_ns'] == meta['frame_start_ns'] +
            ((meta['line_count'] + 8) * row_span - 1) * period,
            'sensor exact functional-profile frame end timestamp differs')
    pattern = contract['pattern']
    require(meta['source'] == pattern['source'] and
            [meta['scene_width'], meta['scene_height']] == pattern['scene'] and
            [meta['crop_x'], meta['crop_y'], meta['crop_width'], meta['crop_height']] == pattern['crop'],
            'sensor source scene/crop differs from independent expected contract')
    require(meta['mirror'] is pattern.get('mirror', False) and meta['flip'] is pattern.get('flip', False),
            'sensor mirror/flip differs from independent expected source')
    image_mode, ctrl0 = meta['image_mode'], meta['ctrl0']
    if meta['format'] == 'rgb565':
        require(image_mode & 0x1C == 8, 'RGB565 metadata contradicts IMAGE_MODE')
        encoding = 'RGB565-LE' if image_mode & 1 else 'RGB565-BE'
    elif meta['format'] == 'yuv422':
        require(image_mode & 0x1C == 0, 'YUV422 metadata contradicts IMAGE_MODE')
        encoding = ('VYUY' if ctrl0 & 16 else 'UYVY') if image_mode & 1 else (
            'YVYU' if ctrl0 & 16 else 'YUYV')
    else:
        require(meta['format'] == 'jpeg', 'sensor format outside RGB565/YUV422/JPEG qualification lane')
        require(image_mode & 16, 'JPEG metadata contradicts IMAGE_MODE')
        encoding = 'JPEG'
    require(encoding == contract['format'], 'sensor byte ordering differs from independent expected format')
    result = qualify(payload, contract['width'], contract['height'], encoding, pattern,
                     contract.get('jpeg_max_error', 12), contract.get('jpeg_mean_error', 3))
    result.update(metadata=meta, windows=len(windows), latch_timestamps=len(timestamps),
                  latch_period_ns=period, timestamps_sha256=sha256(
                      b''.join(timestamp.to_bytes(8, 'little') for timestamp in timestamps)),
                  artifact_sha256=sha256(read_bounded(path, MAX_JSON)),
                  qualification='actual-sensor-emission-not-controller-RX')
    return result, payload


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    image = commands.add_parser('image', help='actual host-saved frame bytes, never synthesized')
    image.add_argument('capture')
    image.add_argument('--width', type=int, required=True)
    image.add_argument('--height', type=int, required=True)
    image.add_argument('--format', required=True, choices=('RGB888', 'RGB332', 'RGB565-BE',
                       'RGB565-LE', 'YUYV', 'YVYU', 'UYVY', 'VYUY', 'JPEG'))
    image.add_argument('--pattern', required=True, help='independent expected pattern JSON file')
    image.add_argument('--jpeg-max-error', type=int, default=12)
    image.add_argument('--jpeg-mean-error', type=float, default=3)
    image.add_argument('--uart', help='optional real guest log; source image alone is not RX qualification')
    image.add_argument('--guest-frame', type=int)
    image.add_argument('--guest-format', type=int)
    plan = commands.add_parser('evidence', help='exact count/status/timestamp contract assertions')
    plan.add_argument('plan')
    fault = commands.add_parser('panel-fault', help='compare real before/after wrong-DC/bus/disconnect status')
    fault.add_argument('capture')
    fault.add_argument('--expected', required=True)
    for name in ('panel-qom', 'sensor-qom'):
        command = commands.add_parser(name, help='compare untouched host-saved immutable QOM windows')
        command.add_argument('capture')
        command.add_argument('--expected', required=True, help='independent capture contract JSON file')
        command.add_argument('--uart', help='real I80/RGB/CAM ownership log')
        command.add_argument('--guest-frame', type=int)
        command.add_argument('--guest-format', type=int)
    wire = commands.add_parser('i80', help='independent decode of real resolved WR samples')
    wire.add_argument('capture')
    wire.add_argument('--width', type=int, required=True)
    wire.add_argument('--height', type=int, required=True)
    wire.add_argument('--bus-width', type=int, required=True, choices=(8, 16))
    wire.add_argument('--frames', type=int, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'evidence':
            result = evidence_plan(args.plan)
        elif args.command == 'panel-fault':
            result = panel_fault(args.capture, load_json(args.expected))
        elif args.command == 'i80':
            result = wire_capture(args)
        elif args.command in ('panel-qom', 'sensor-qom'):
            contract = load_json(args.expected)
            if args.command == 'panel-qom':
                result = panel_qom(args.capture, contract)
                if args.uart:
                    result['guest_evidence'] = panel_uart(args.uart, contract)
            else:
                result, payload = sensor_qom(args.capture, contract)
                if args.uart:
                    require(args.guest_frame is not None and args.guest_format is not None,
                            '--uart requires explicit --guest-frame and --guest-format')
                    result['guest_evidence'] = cam_uart(args.uart, payload, contract['width'], contract['height'],
                                                      args.guest_frame, args.guest_format)
        else:
            payload = read_bounded(args.capture, MAX_PAYLOAD)
            result = qualify(payload, args.width, args.height, args.format, load_json(args.pattern),
                             args.jpeg_max_error, args.jpeg_mean_error)
            result['qualification'] = 'actual-frame-image-only-not-controller-RX'
            if args.uart:
                require(args.guest_frame is not None and args.guest_format is not None,
                        '--uart requires explicit --guest-frame and --guest-format')
                result['guest_evidence'] = cam_uart(args.uart, payload, args.width, args.height,
                                                  args.guest_frame, args.guest_format)
        print(json.dumps({'status': 'MATCH', 'evidence': result}, indent=2, sort_keys=True))
        return 0
    except (ReferenceError, OSError, KeyError, TypeError, ValueError, IndexError, OverflowError) as exc:
        print(json.dumps({'status': 'REJECTED', 'reason': str(exc)}, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
