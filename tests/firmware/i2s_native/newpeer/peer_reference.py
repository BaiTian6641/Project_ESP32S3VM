#!/usr/bin/env python3
"""Finite actual-event I2S reference, not a controller or guest-result oracle.

CLI: peer_reference.py attribution.json
Attribution version 1 has rows [{case, epoch, boundary, rx:{path}, din:{path},
transitions:{path}, transition_attribution:{component_id,config_identity,
controller_origin_epoch,controller_epochs,peer_epochs,min_frames}}].
Boundary is start/restart/power-epoch; min_frames must be >=768. Successful
FIFO LOAD identities and published COMPLETE boundaries derive phase, never
attempt ordinals or payload fitting. ALL physical startup events are retained
and explicitly classified; no mismatch trimming or inferred playback success.
Paths are relative to the attribution file. MCU RX is S3I2SRX1. DIN input is
a JSON array of actual capture-json windows from QOM
/machine/soc/i2s-sample-peers, obtained using capture-request-json
{componentId,offset,count:1..1024}. Windows must cover a closed capture from
offset0 through total, with unchanged component_id/config_identity/total.
Transitions are separate pages of actual optional transitions:{version,peer,
controller} getter windows; offsets span the complete retained sequence range.
Lost metadata is retained; any needed START/LOAD/BEGIN absent fails qualification.
The implemented binder source defines this input ABI. Runtime proof requires
actual captures after provider integration; authoring/tests alone is source proof.
Synthetic tests exercise parser/math only. This is source proof until actual
captures are supplied; PCM-to-PDM converter and hardware qualification remain
blocked and cannot be established by this program.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
HEADER = struct.Struct('<8sII')
EVENT = struct.Struct('<QQQIHBB')
BLOCKED = ['PCM-to-PDM exact arithmetic unavailable', 'hardware qualification not performed']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sample(source, frame, slot, width):
    """Integer fixture polynomial reduced modulo the physical word alphabet."""
    value = (73 * frame + 29 * slot + 101 * source + 17) ^ ((frame + slot + 1) * 2654435769)
    return value % (2 ** width)


def word_bits(value, width):
    return format(value, f'0{width}b')


def geometry(case):
    c = case['cmake']
    return c['PEER_FORMAT'], c['PEER_BITS'], c['PEER_SLOTS'], c['PEER_MASK']


def selected_slots(slots, mask):
    return [s for s in range(slots) if (mask // (2 ** s)) % 2]


def expected_events(case, source, phase, frames, din=False):
    fmt, width, slots, mask = geometry(case)
    chosen = selected_slots(slots, mask)
    result = []
    for frame in range(frames):
        descriptor_frame = (phase + frame) % 96
        if fmt == 'raw-pdm' and din:
            words = [word_bits(sample(source, descriptor_frame, s, 16), 16) for s in (0, 1)]
            for index in range(16):
                for slot in (0, 1):
                    result.append((int(words[slot][index]), slot))
        else:
            for slot in chosen:
                result.append((sample(source, descriptor_frame, slot, width), slot))
    return result


def wire_frame(case, source, frame):
    """Independent serial alphabet: one string per physical slot, masked zero.

    Philips/PCM/TDM have a one-clock delay; its final bit crosses the next
    boundary. This function gives the complete logical frame, not the initial
    partial Philips tail (which is never a completed captured sample).
    """
    fmt, width, slots, mask = geometry(case)
    words = [word_bits(sample(source, frame % 96, s, width), width)
             if s in selected_slots(slots, mask) else '0' * width
             for s in range(slots)]
    if fmt == 'raw-pdm':
        return ''.join(words[s][i] for i in range(16) for s in (0, 1))
    return ''.join(words)



def serial_timeline(case, source, frames):
    """Logical complete-frame bits and WS at each sampling index.

    Data is indexed from its first complete bit. Delayed formats associate WS
    with the following wire index; no fabricated preceding partial word.
    Raw chronological indices alternate phase0/phase1, beginning at actual
    phase0 edge (not at the inactive baseline).
    """
    fmt, width, slots, mask = geometry(case)
    raw = fmt == 'raw-pdm'
    delay = int(fmt in ('philips', 'pcm', 'tdm'))
    pol = fmt in ('pcm', 'tdm')
    frame_width = slots * width
    data = ''.join(wire_frame(case, source, f) for f in range(frames))
    ws = []
    for index in range(len(data)):
        wire = (index + delay) % frame_width
        if raw:
            level = index % 2
        elif fmt in ('pcm', 'tdm'):
            level = int((wire >= 1) != pol)
        else:
            level = (wire // width) % 2
        ws.append(level)
    return data, tuple(ws)
def parse_rx(data):
    require(len(data) >= HEADER.size, 'truncated RX header')
    magic, controller, version = HEADER.unpack_from(data)
    require(magic == b'S3I2SRX1' and controller in (0, 1) and version == 1, 'invalid RX header')
    require((len(data) - HEADER.size) % EVENT.size == 0, 'truncated RX event')
    events = []
    for offset in range(HEADER.size, len(data), EVENT.size):
        ns, sequence, frame, value, slot, width, flags = EVENT.unpack_from(data, offset)
        require(width in (8, 16, 24, 32) and slot < 16 and value < 2 ** width, 'invalid RX geometry')
        require(flags in (3, 5), 'invalid RX validity/disposition')
        events.append(dict(ns=ns, sequence=sequence, frame=frame, sample=value, slot=slot, width=width, flags=flags))
    validate_order(events)
    return controller, events


def validate_order(events):
    require(bool(events), 'empty actual capture')
    require(events[0]['sequence'] == 0, 'complete capture must begin at sequence zero')
    for previous, current in zip(events, events[1:]):
        require(current['sequence'] == previous['sequence'] + 1, 'missing/duplicate actual event sequence')
        require(current['ns'] >= previous['ns'], 'decreasing capture timestamp')


def parse_din(data):
    """Decode contiguous actual QOM capture-json windows of a closed capture."""
    decoded = json.loads(data)
    pages = decoded if isinstance(decoded, list) else [decoded]
    require(bool(pages), 'empty actual DIN window export')
    export = pages[0]
    require(isinstance(export, dict), 'invalid capture window')
    identity = export['config_identity']
    component = export['component_id']
    total = export['total']
    require(isinstance(component, str) and 1 <= len(component) <= 64, 'invalid component attribution')
    require(isinstance(identity, str) and len(identity) == 64 and
            all(c in '0123456789abcdef' for c in identity), 'invalid canonical config identity')
    require(type(total) is int and total > 0, 'empty/invalid closed DIN capture')
    events = []
    for page in pages:
        require(page.get('version') == 1 and page.get('kind') == 'i2s-peer-din-capture', 'unsupported actual DIN export')
        require(page['component_id'] == component and page['config_identity'] == identity and
                page['total'] == total, 'capture identity/total changed between windows')
        require(type(page['offset']) is int and page['offset'] == len(events), 'missing/reused DIN window')
        require(isinstance(page['status'], dict), 'missing actual peer status')
        window = page['events']
        require(isinstance(window, list) and 0 < len(window) <= 1024, 'invalid actual DIN window count')
        events.extend(window)
    require(len(events) == total, 'incomplete closed DIN capture windows')
    for e in events:
        require(all(type(e[k]) is int for k in ('ns', 'sequence', 'sample', 'slot')), 'invalid DIN integer')
        require(e['ns'] >= 0 and e['sequence'] >= 0 and 0 <= e['slot'] < 16 and 0 <= e['sample'] < 2 ** 32, 'invalid DIN event')
        require(type(e['raw']) is bool, 'invalid DIN raw flag')
        require(not e['raw'] or e['sample'] in (0, 1), 'invalid raw bit')
    validate_order(events)
    return export, events


def offsets(case, frames, din):
    fmt, width, slots, mask = geometry(case)
    if fmt == 'raw-pdm':
        period = Fraction(10 ** 9, 62500)
        if din:
            # Peer master (MCU slave) observes DIN at the internal BCLK rise
            # half an internal period after WS; peer slave observes WS itself.
            delay = Fraction(0) if case['cmake']['PEER_MASTER'] else period / 64
            return [delay + frame * period + Fraction(index, 32) * period
                    for frame in range(frames) for index in range(32)]
        # Both raw channel words complete on the same internal BCLK edge.
        return [frame * period for frame in range(frames) for _ in (0, 1)]
    bit = Fraction(10 ** 9, 12500 * slots * width)
    return [(frame * slots * width + slot * width) * bit
            for frame in range(frames) for slot in selected_slots(slots, mask)]


def qualify_direction(case, spec, events, source, din):
    """Explicit-phase synthetic math helper; runtime CLI requires transitions."""
    require(type(spec['start']) is int and type(spec['stop']) is int and 0 <= spec['start'] < spec['stop'] <= len(events), 'invalid attributed range')
    require(type(spec['phase']) is int and 0 <= spec['phase'] < 96, 'invalid explicit phase')
    require(type(spec['frames']) is int and spec['frames'] >= 768, 'insufficient finite sustained frames')
    actual = events[spec['start']:spec['stop']]
    expected = expected_events(case, source, spec['phase'], spec['frames'], din)
    require(len(actual) == len(expected), 'actual count differs from complete finite frame range')
    fmt, width, slots, mask = geometry(case)
    chosen = selected_slots(slots, mask)
    if not din:
        logical_frames = [e['frame'] - int(fmt == 'philips' and e['slot'] == slots - 1) for e in actual]
        first_frame = logical_frames[0]
        require(all(frame == first_frame + index // len(chosen)
                    for index, frame in enumerate(logical_frames)), 'RX immutable frame/slot boundary mismatch')
    for index, (event, pair) in enumerate(zip(actual, expected)):
        require((event['sample'], event['slot']) == pair, f'actual sample/slot mismatch at {index}')
        if din:
            require(event['raw'] == (fmt == 'raw-pdm'), 'actual DIN format mismatch')
        else:
            require(event['width'] == width and event['flags'] == 3, 'RX dropped/invalid/width mismatch')
    timing = offsets(case, spec['frames'], din)
    origin = actual[0]['ns']
    for event, offset in zip(actual, timing):
        require(abs(Fraction(event['ns'] - origin) - (offset - timing[0])) <= 1, 'rational cadence differs by more than 1ns')
    for index in range(1, len(actual)):
        require(abs(Fraction(actual[index]['ns'] - actual[index - 1]['ns']) -
                    (timing[index] - timing[index - 1])) <= 1, 'adjacent rational cadence differs by more than 1ns')
        if fmt == 'raw-pdm' and not din and index % 2:
            require(actual[index]['ns'] == actual[index - 1]['ns'], 'raw RX channels must complete at same instant')
    packed = b''.join(value.to_bytes(4, 'little') + slot.to_bytes(2, 'little') for value, slot in expected)
    actual_packed = b''.join(e['sample'].to_bytes(4, 'little') + e['slot'].to_bytes(2, 'little') for e in actual)
    return dict(count=len(actual), frames=spec['frames'], phase=spec['phase'], source=source,
                expected_sample_slot_sha256=digest(packed), actual_sample_slot_sha256=digest(actual_packed),
                exact_samples=True, exact_slot_order=True, cadence_tolerance_ns=1,
                cadence_frame_hz='62500/1' if fmt == 'raw-pdm' else '12500/1',
                raw_din_edge=('ws' if case['cmake']['PEER_MASTER'] else 'bclk-rise-after-ws')
                    if fmt == 'raw-pdm' and din else None,
                raw_din_delay_from_ws_ns=str(Fraction(0) if case['cmake']['PEER_MASTER'] else Fraction(250))
                    if fmt == 'raw-pdm' and din else None,
                absolute_clock_origin_qualified=False,
                initial_partial_philips_tail_recorded=False)


def qualify_manifest(path):
    path = Path(path)
    raw_manifest = path.read_bytes()
    manifest = json.loads(raw_manifest)
    require(manifest.get('version') == 1, 'unsupported attribution version')
    cases = {c['name']: c for c in json.loads((ROOT / 'cases-v1.json').read_bytes())['cases']}
    rows = []
    seen = set()
    for row in manifest['rows']:
        case = cases[row['case']]
        require(row['boundary'] in ('start', 'restart', 'power-epoch'), 'missing lifecycle attribution')
        require(type(row['epoch']) is int and row['epoch'] >= 0, 'invalid power/restart epoch')
        key = (row['case'], row['epoch'])
        require(key not in seen, 'duplicate case epoch')
        seen.add(key)
        inputs = [(path.parent / row[name]['path']).read_bytes()
                  for name in ('rx', 'din', 'transitions')]
        mapped = qualify_transition_inputs(case, *inputs, row['transition_attribution'])
        mapped.update(epoch=row['epoch'], boundary=row['boundary'])
        rows.append(mapped)
    require(bool(rows), 'no actual attributed rows')
    covered = {r['case'] for r in rows}
    return dict(version=1, attribution_sha256=digest(raw_manifest), rows=rows, covered_cases=len(covered), total_cases=len(cases), missing_cases=sorted(set(cases) - covered), all_68_cases_qualified=len(covered) == 68, blocked=BLOCKED)


def parse_transition_pages(data):
    """Read complete retained actual getter windows, exposing every lost record."""
    pages = json.loads(data)
    pages = pages if isinstance(pages, list) else [pages]
    require(bool(pages), 'empty transition export')
    result = {}
    for scope in ('peer', 'controller'):
        metadata = None
        records = []
        cursor = None
        for page in pages:
            root = page['transitions'] if 'transitions' in page else page
            require(root['version'] == 1, 'unsupported transition root version')
            window = root[scope]
            require(window['version'] == 1, 'unsupported transition window version')
            require(all(type(window[k]) is int and window[k] >= 0
                        for k in ('first_sequence', 'total', 'lost', 'capacity', 'count', 'offset')),
                    'invalid transition retention metadata')
            require(window['capacity'] > 0 and window['count'] <= window['capacity'],
                    'invalid bounded transition capacity')
            if metadata is None:
                metadata = window
                cursor = window['first_sequence']
                require(window['lost'] == cursor and window['count'] == window['total'] - cursor,
                        'invalid retained transition accounting')
            require(all(window[k] == metadata[k] for k in ('first_sequence', 'total', 'lost', 'capacity')),
                    'transition ring changed while exporting')
            require(window['offset'] == cursor, 'missing/duplicate transition window')
            require(len(window['records']) <= 1024, 'oversize transition window')
            for record in window['records']:
                require(type(record['sequence']) is int and record['sequence'] >= 0,
                        'invalid immutable transition sequence')
                require('sample' not in record and 'payload' not in record,
                        'transition attribution must remain payload-free')
                require(record['sequence'] == cursor, 'missing/duplicate transition record')
                require(type(record['ns']) is int and record['ns'] >= 0, 'invalid actual transition time')
                if records:
                    require(record['ns'] >= records[-1]['ns'], 'decreasing transition time')
                records.append(record)
                cursor += 1
        require(cursor == metadata['total'], 'incomplete retained transition export')
        result[scope] = dict(metadata=metadata, records=records)
    return result


def _source_expectations(case, transitions, attribution):
    """Map actual published complete source identities to physical sample times."""
    fmt, width, slots, mask = geometry(case)
    chosen = selected_slots(slots, mask)
    raw = fmt == 'raw-pdm'
    peer = transitions['peer']['records']
    controller = transitions['controller']['records']
    origin_epoch = attribution['controller_origin_epoch']
    starts = [r for r in controller if r['epoch'] == origin_epoch and r['kind'] == 0]
    loads = {r['source_word_id']: r for r in controller if r['kind'] == 6}
    require(len(loads) == sum(r['kind'] == 6 for r in controller), 'duplicate successful FIFO source ID')
    origin_loads = [r for r in loads.values() if r['epoch'] == origin_epoch]
    require(starts and origin_loads, 'required origin START/LOAD lost or absent')
    origin = min(origin_loads, key=lambda r: r['sequence'])
    require(origin['sequence'] > starts[0]['sequence'], 'source origin predates actual START')
    origin_id = origin['source_word_id']
    require(origin_id > 0, 'zero cannot identify a FIFO source')
    peer_epochs = set(attribution['peer_epochs'])
    controller_epochs = set(attribution['controller_epochs'])
    require(peer_epochs and controller_epochs, 'missing selected lifecycle epochs')
    require(peer_epochs <= {r['epoch'] for r in peer if r['kind'] in (0, 1)},
            'required peer activation/power epoch lost')
    require(controller_epochs <= {r['epoch'] for r in controller if r['kind'] in (0, 3)},
            'required controller START/reset epoch lost')
    beginnings = {r['source_id']: r for r in peer if r['kind'] == 2}
    rx = {}
    din = {}
    partial = {'rx': [], 'din': []}
    active = {'rx': [], 'din': []}
    peer_bits = []

    def insert(target, time, slot, value, record, source_index, idle=False):
        key = (time, slot)
        require(key not in target, 'ambiguous actual completed source boundary')
        target[key] = dict(sample=value, record=record, index=source_index, idle=idle)

    for record in peer:
        if record['epoch'] not in peer_epochs or record['kind'] not in (2, 3, 4):
            continue
        require(record['raw'] == raw, 'peer transition format mismatch')
        first, last = record['first_ns'], record['last_boundary_ns']
        if record['kind'] == 4:
            partial['rx'].append((first, max(record['last_ns'], record['ns'])))
        if record['kind'] != 3:
            continue
        require(record['source'] == 0 and record['powered'] and record['power_known'], 'unpowered/nonpayload peer completion')
        require(record['source_id'] in beginnings, 'required peer BEGIN lost')
        beginning = beginnings[record['source_id']]
        require(all(beginning[k] == record[k] for k in ('epoch', 'source_cursor', 'raw_channel', 'slot')),
                'peer BEGIN/COMPLETE source identity changed')
        require(record['published_bits'] == record['slot_bits'] == (1 if raw else width),
                'incomplete peer source publication')
        require(record['valid_bits'] == (1 if raw else width) and first >= 0 and last >= first,
                'invalid peer complete boundaries')
        active['rx'].append((first, last + (250 if raw else 0)))
        cursor = record['source_cursor']
        if raw:
            require(record['raw_channel'] == cursor % 2, 'raw source phase identity mismatch')
            descriptor_frame, within = divmod(cursor, 32)
            value = int(word_bits(sample(1, descriptor_frame % 96, within % 2, 16), 16)[within // 2])
            peer_bits.append((last + 250, within % 2, value, record))
        else:
            frame, logical_slot = divmod(cursor, len(chosen))
            slot = chosen[logical_slot]
            require(record['slot'] == slot, 'peer selected-slot source order mismatch')
            insert(rx, last, slot, sample(1, frame % 96, slot, width), record, cursor)

    if raw:
        # RX packs sixteen chronological physical bits per channel. Its two
        # immutable records complete together after phase1 bit15, not after
        # phase0 bit15. Metadata, never values, chooses the preceding 32 bits.
        peer_bits.sort(key=lambda item: item[0])
        for end in range(31, len(peer_bits)):
            group = peer_bits[end - 31:end + 1]
            final_record = group[-1][3]
            if final_record['source_cursor'] % 32 != 31:
                continue
            if not all(item[3]['epoch'] == final_record['epoch'] for item in group):
                continue
            require([item[1] for item in group] == [0, 1] * 16, 'raw completed source phase order mismatch')
            require(all(abs(Fraction(item[0] - group[0][0]) - 500 * index) <= 1
                        for index, item in enumerate(group)), 'raw peer physical phase cadence mismatch')
            for slot in (0, 1):
                value = int(''.join(str(item[2]) for item in group if item[1] == slot), 2)
                insert(rx, group[-1][0], slot, value, final_record, final_record['source_cursor'] // 32)

    for record in controller:
        if record['epoch'] not in controller_epochs or record['kind'] not in (7, 8):
            continue
        first, last = record['first_ns'], record['last_ns']
        if record['kind'] == 8:
            partial['din'].append((first, max(last, record['ns']) + (250 if raw else 0)))
            continue
        require(bool(record['flags'] & 1) == raw, 'controller transition format mismatch')
        require(bool(record['flags'] & 2) == (not bool(case['cmake']['PEER_MASTER'])),
                'actual controller role differs from selected case')
        require(record['shifted_bits'] == record['slot_bits'] == width and record['valid_bits'] == width,
                'controller completion lacks every physical bit')
        identity = record['source_word_id']
        idle = identity == 0
        source_index = None
        if not idle:
            require(identity in loads, 'required successful FIFO LOAD lost')
            load = loads[identity]
            require(load['sequence'] < record['sequence'], 'completion predates source LOAD')
            source_index = identity - origin_id
            require(source_index >= 0, 'completion refers to a prior case source origin')
            frame, logical_slot = divmod(source_index, len(chosen))
            source_slot = chosen[logical_slot]
            value = sample(0, frame % 96, source_slot, width)
        else:
            require(record['source'] in (1, 4), 'zero ID is not declared register/idle source')
            value = None
        if raw:
            slot = record['raw_channel']
            require(slot in (0, 1), 'invalid raw controller channel')
            if not idle:
                require(source_slot == slot or record['source'] in (2, 3), 'raw source LOAD/channel mismatch')
            # MCU master samples at BCLK rise, but the peer slave observes DIN
            # at the preceding WS edge. MCU slave has no internal boundaries:
            # actual source publication is at WS and peer samples 250ns later.
            slave = bool(record['flags'] & 2)
            begin = first + 250 if slave else record['first_boundary_ns'] - 250
            finish = last + 250 if slave else record['last_boundary_ns'] - 250
            require(begin >= 0 and abs(Fraction(finish - begin) - 15000) <= 1, 'raw completed word cadence mismatch')
            active['din'].append((begin, finish))
            bits = word_bits(value, width) if value is not None else None
            for bit in range(16):
                insert(din, begin + bit * 1000, slot, int(bits[bit]) if bits else None,
                       record, source_index, idle)
        else:
            require(record['last_boundary_ns'] >= 0, 'standard completion missing BCLK boundary')
            active['din'].append((first, record['last_boundary_ns']))
            insert(din, record['last_boundary_ns'], record['slot'], value, record, source_index, idle)
    require(rx and din, 'no completed bidirectional actual source boundaries')
    return dict(rx=rx, din=din, partial=partial, active=active, origin_id=origin_id)


def _qualify_mapped(case, events, expected, name, mapping, minimum):
    """Keep every physical event; only actual metadata classifies nonpayload."""
    fmt, width, slots, mask = geometry(case)
    classifications = []
    qualified = []
    matched = set()
    intervals = mapping['active'][name]
    start = min(first for first, _ in intervals)
    finish = max(last for _, last in intervals)
    for index, event in enumerate(events):
        key = (event['ns'], event['slot'])
        item = expected.get(key)
        if item is None:
            # A quantized reconstructed raw boundary may differ by one ns.
            candidates = [(time, event['slot']) for time in (event['ns'] - 1, event['ns'] + 1)
                          if (time, event['slot']) in expected]
            require(len(candidates) <= 1, 'ambiguous quantized actual source boundary')
            if candidates:
                key = candidates[0]
                item = expected[key]
        if item is None:
            if event['ns'] < start or event['ns'] > finish:
                label = 'outside-selected-completed-source-interval'
            elif any(first <= event['ns'] <= last for first, last in mapping['partial'][name] if first >= 0):
                label = 'actual-aborted-partial-source'
            elif fmt == 'raw-pdm' and name == 'rx' and event['ns'] < min(time for time, _ in expected):
                label = 'initial-incomplete-raw-packet'
            else:
                raise ValueError(f'{name} actual event {index} lacks complete or explicit partial source attribution')
        elif item['idle']:
            label = 'actual-register-or-idle-source'
        else:
            require(key not in matched, 'completed source boundary matched twice')
            require(event['sample'] == item['sample'], f'{name} actual sample mismatch at event {index}')
            if name == 'rx':
                require(event['flags'] == 3 and event['width'] == width, 'actual RX invalid/dropped/width mismatch')
            else:
                require(event['raw'] == (fmt == 'raw-pdm'), 'actual DIN raw geometry mismatch')
            matched.add(key)
            qualified.append((index, event, item))
            label = 'qualified-completed-payload'
        classifications.append(label)
    require(qualified, f'no qualified actual {name} payload')
    # No source completion may disappear inside the observed finite interval.
    first_ns, last_ns = qualified[0][1]['ns'], qualified[-1][1]['ns']
    required = {key for key, item in expected.items() if not item['idle'] and first_ns <= key[0] <= last_ns}
    require(required <= matched, f'missing actual {name} completed source events')
    per_frame = 32 if fmt == 'raw-pdm' and name == 'din' else len(selected_slots(slots, mask))
    require(len(qualified) >= minimum * per_frame, f'insufficient finite {name} completed samples')
    complete_groups = {}
    if name == 'rx':
        for _, event, item in qualified:
            record = item['record']
            complete_groups.setdefault((record['epoch'], record['frame']), set()).add(event['slot'])
    else:
        by_source = {}
        for _, _, item in qualified:
            by_source[item['index']] = by_source.get(item['index'], 0) + 1
        chosen = selected_slots(slots, mask)
        for source_index, count in by_source.items():
            if count >= (16 if fmt == 'raw-pdm' else 1):
                source_frame, source_slot = divmod(source_index, len(chosen))
                complete_groups.setdefault(source_frame, set()).add(chosen[source_slot])
    complete_frames = sum(group == set(selected_slots(slots, mask)) for group in complete_groups.values())
    require(complete_frames >= minimum, f'insufficient complete attributed {name} source frames')
    if name == 'din':
        unique_fifo_sources = {item['record']['source_word_id'] for _, _, item in qualified}
        require(len(unique_fifo_sources) >= minimum * len(selected_slots(slots, mask)),
                'replay/copy does not establish sustained successful FIFO source playback')
    else:
        unique_fifo_sources = None
    require([e['slot'] for _, e, _ in qualified] ==
            [key[1] for key in sorted(matched)], f'{name} actual slot order mismatch')
    # Physical cadence is independently constrained over every qualified pair,
    # not fitted by source values or by per-interval rounding accumulation.
    frame_period = Fraction(10 ** 9, 62500 if fmt == 'raw-pdm' else 12500)
    unit = frame_period / (32 if fmt == 'raw-pdm' and name == 'din' else slots)
    origin_event = qualified[0][1]
    origin_slot = origin_event['slot']
    frame_count = 0
    previous_slot = origin_slot
    for index, (_, event, _) in enumerate(qualified):
        if index and event['slot'] <= previous_slot:
            frame_count += 1
        if fmt == 'raw-pdm' and name == 'din':
            elapsed = index * unit
        elif fmt == 'raw-pdm':
            elapsed = frame_count * frame_period
        else:
            elapsed = frame_count * frame_period + (event['slot'] - origin_slot) * unit
        require(abs(Fraction(event['ns'] - origin_event['ns']) - elapsed) <= 1, f'{name} actual rational cadence mismatch')
        previous_slot = event['slot']
    ranges = []
    for index, label in enumerate(classifications):
        if ranges and ranges[-1]['classification'] == label:
            ranges[-1]['stop'] = index + 1
        else:
            ranges.append(dict(start=index, stop=index + 1, classification=label))
    packed = b''.join(e['sample'].to_bytes(4, 'little') + e['slot'].to_bytes(2, 'little')
                      for _, e, _ in qualified)
    expected_packed = b''.join(item['sample'].to_bytes(4, 'little') + e['slot'].to_bytes(2, 'little')
                              for _, e, item in qualified)
    return dict(actual_events=len(events), qualified_count=len(qualified), classifications=ranges,
                complete_attributed_frames=complete_frames,
                unique_successful_fifo_sources=len(unique_fifo_sources) if unique_fifo_sources is not None else None,
                actual_sample_slot_sha256=digest(packed), expected_sample_slot_sha256=digest(expected_packed),
                exact_samples=True, exact_slot_order=True, cadence_tolerance_ns=1,
                phase_basis='actual-LOAD/source-cursor-and-completed-boundary')


def qualify_transition_inputs(case, rx_data, din_data, transition_data, attribution):
    """Pinned runner API: bytes in, actual finite bidirectional evidence out.

    attribution: component_id, config_identity, controller_origin_epoch,
    controller_epochs, peer_epochs, min_frames (>=768). Epoch selection is
    supplied by actual lifecycle metadata; no sample value selects a phase.
    """
    controller, rx_events = parse_rx(rx_data)
    din_metadata, din_events = parse_din(din_data)
    require(controller == case['cmake']['PEER_CONTROLLER'], 'actual RX controller mismatch')
    require(din_metadata['component_id'] == attribution['component_id'] and
            din_metadata['config_identity'] == attribution['config_identity'], 'actual DIN attribution mismatch')
    minimum = attribution.get('min_frames', 768)
    require(type(minimum) is int and minimum >= 768, 'insufficient finite frame requirement')
    transitions = parse_transition_pages(transition_data)
    require(transitions['controller']['metadata']['controller'] == controller, 'transition controller mismatch')
    mapping = _source_expectations(case, transitions, attribution)
    return dict(case=case['name'], qualification='actual-transition-attributed-events',
                actual_rx_input_sha256=digest(rx_data), actual_din_input_sha256=digest(din_data),
                actual_transition_input_sha256=digest(transition_data),
                controller_source_origin_id=mapping['origin_id'],
                component_id=attribution['component_id'], config_identity=attribution['config_identity'],
                lifecycle_attribution=attribution,
                rx=_qualify_mapped(case, rx_events, mapping['rx'], 'rx', mapping, minimum),
                din=_qualify_mapped(case, din_events, mapping['din'], 'din', mapping, minimum),
                retained_transition_loss={scope: transitions[scope]['metadata']['lost']
                                          for scope in ('peer', 'controller')},
                blocked=BLOCKED)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('attribution', type=Path)
    args = parser.parse_args()
    try:
        evidence = qualify_manifest(args.attribution)
    except (ValueError, KeyError, TypeError, OSError) as exc:
        parser.exit(1, f'not qualified: {exc}\n')
    print(json.dumps(evidence, separators=(',', ':')))
    return 0 if evidence['all_68_cases_qualified'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
