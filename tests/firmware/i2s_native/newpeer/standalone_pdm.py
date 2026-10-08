#!/usr/bin/env python3
"""Host-only raw PDM verification; no PCM converter/hardware qualification."""
import argparse
import ctypes as C
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import traceback

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence', type=Path, required=True, help='NEW standalone verification directory')
args = parser.parse_args()
ROOT = Path(__file__).resolve().parents[4]
OUT = args.evidence.resolve()
OUT.mkdir(parents=True, exist_ok=False)
SOURCE = ROOT / 'qemu-extensions/prototypes/i2s'
COPIES = SOURCE / 'copies'
results = {'scope': 'standalone raw PDM helper only; no hardware, PCM converter or integrated QEMU runtime qualification', 'commands': [], 'checks': {}, 'inputs': {}}
for name in ('copies/esp32s3_i2s_pdm.c', 'copies/esp32s3_i2s_pdm.h', 'copies/esp32s3-i2s-pdm-test.c', 'pdm-reference-vectors.py', 'pdm-provenance.json'):
    results['inputs'][name] = hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()


def run(label, args, env=None):
    completed = subprocess.run([str(a) for a in args], cwd=ROOT, env=env, capture_output=True, text=True)
    (OUT / (label + '.log')).write_text(completed.stdout + completed.stderr, encoding='utf-8')
    results['commands'].append({'label': label, 'argv': [str(a) for a in args], 'exit_code': completed.returncode, 'log': label + '.log'})
    return completed


class State(C.Structure):
    _fields_ = [('words', C.c_uint16 * 8), ('counts', C.c_uint8 * 8), ('ready', C.c_uint8), ('enabled', C.c_uint8), ('port', C.c_uint8)]


def bind(lib, name, args):
    fn = getattr(lib, 'esp32s3_i2s_pdm_' + name)
    fn.argtypes = args
    fn.restype = C.c_int
    return fn


def mathematical_comparison(lib):
    u = C.c_uint32
    b = C.c_uint8
    w = C.c_uint16
    ptr = C.POINTER(State)
    reset = bind(lib, 'raw_reset', [ptr, C.c_uint])
    reset.restype = None
    load = bind(lib, 'raw_tx_load', [ptr, u, u, u, w, w, w])
    edge = bind(lib, 'raw_tx_edge', [ptr, u, u, C.c_bool, C.POINTER(b), C.POINTER(b)])
    capture = bind(lib, 'raw_rx_edge', [ptr, u, u, u, C.c_bool, b, b])
    pop = bind(lib, 'raw_rx_pop', [ptr, u, C.POINTER(b), C.POINTER(w)])
    txmode = bind(lib, 'tx_mode', [C.c_uint, u, u, u])
    rxmode = bind(lib, 'rx_mode', [C.c_uint, u, u, u])
    base = (1 << 20) | (1 << 12)
    conf = (15 << 13) | (15 << 18)
    rng = random.Random(0x5320261007)
    tx_frames = rx_frames = edges = 0
    # A static interpretation of TRM table 28.9-4, indexed by WS polarity.
    routing = (('L', 'R'), ('L', 'L'), ('R', 'R'), ('S', 'R'), ('L', 'S'))
    inverted_routing = (('L', 'R'), ('R', 'R'), ('L', 'L'), ('L', 'S'), ('S', 'R'))
    for pol, big, lsb, mono, first_valid, mode, dac in itertools.product(range(2), range(2), range(2), range(2), range(2), range(5), range(3)):
        if dac == 1 and not mono or dac == 2 and mono:
            continue
        for repeat in range(3):
            units = [rng.randrange(65536) for _ in range(3)]
            first, second, single = units
            def stream(word, swap=False):
                octets = word.to_bytes(2, 'little')
                if swap:
                    octets = octets[::-1]
                text = ''.join(format(octet, '08b') for octet in octets[::-1])
                return text[::-1] if lsb else text
            a, d, constant = stream(first, big), stream(second, big), stream(single)
            if mono:
                channel_streams = {'L': a if first_valid != pol else constant, 'R': constant if first_valid != pol else a, 'S': constant}
            else:
                channel_streams = {'L': d if pol else a, 'R': a if pol else d, 'S': constant}
            names = (inverted_routing if pol else routing)[mode]
            left, right = [channel_streams[name] for name in names]
            if dac == 1:
                left = right = a
            expected = []
            for x, y in zip(left, right):
                if dac == 0:
                    expected += [int(x), int(y)]
                elif dac == 1:
                    expected += [int(y)] * 2
                else:
                    expected += [int(y) + 2 * int(x)] * 2
            cfg = base | (mono << 5) | (first_valid << 9) | (big << 7) | (pol << 17) | (lsb << 18) | (mode << 24)
            pdm = (1 << 24 if dac else 0) | (1 << 23 if dac == 2 else 0)
            txconf = conf if mono else (31 << 13) | (15 << 18)
            state = State()
            reset(C.byref(state), 0)
            assert load(C.byref(state), cfg, txconf, pdm, first, second, single) == 0
            bits, mask = b(), b()
            for phase, value in enumerate(expected):
                ws = bool(pol) != bool(phase % 2)
                assert edge(C.byref(state), cfg, pdm, ws, C.byref(bits), C.byref(mask)) == 0
                assert (bits.value, mask.value) == (value, 3 if dac == 2 else 1)
                edges += 1
            assert state.ready == 0
            tx_frames += 1
    # Generate serial streams directly; decode via Python int and byte order,
    # independently of TX and the helper's shift/count accumulation.
    for pol, big, lsb, mono_selection, active in itertools.product(range(2), range(2), range(2), range(3), range(256)):
        cfg = base | (pol << 17) | (big << 7) | (lsb << 18)
        enabled = active
        if mono_selection:
            first_valid = mono_selection == 1
            cfg |= (1 << 5) | (int(first_valid) << 9)
            enabled &= 0x55 if first_valid != bool(pol) else 0xaa
        streams = [''.join(rng.choice('01') for _ in range(16)) for ch in range(8)]
        state = State()
        reset(C.byref(state), 0)
        for index in range(16):
            for phase in range(2):
                # Complete mono/sparse frames can become ready on phase zero.
                if state.ready:
                    break
                levels = sum(int(streams[2 * line + phase][index]) << line for line in range(4))
                valid = sum(bool(enabled & (1 << (2 * line + phase))) << line for line in range(4))
                if valid:
                    before = bytes(state)
                    missing = valid & (valid - 1)
                    assert capture(C.byref(state), cfg, conf, active, bool(phase) != bool(pol), levels, missing) == 4
                    assert bytes(state) == before
                assert capture(C.byref(state), cfg, conf, active, bool(phase) != bool(pol), levels, valid) == 0
                edges += 1
        channel, word = b(0xa5), w(0xcdef)
        for ch in range(8):
            if not enabled & (1 << ch):
                continue
            text = streams[ch][::-1] if lsb else streams[ch]
            value = int(text, 2)
            if big:
                value = int.from_bytes(value.to_bytes(2, 'little'), 'big')
            assert pop(C.byref(state), cfg, C.byref(channel), C.byref(word)) == 0
            assert (channel.value, word.value) == (ch, value)
        before = bytes(state)
        assert pop(C.byref(state), cfg, C.byref(channel), C.byref(word)) == 1
        assert bytes(state) == before
        rx_frames += 1
    # Converter requests must preserve partial/loaded raw state and outputs.
    for port in (0, 1):
        state = State()
        reset(C.byref(state), port)
        assert load(C.byref(state), base, conf, 0, 0x1234, 0x5678, 0x9abc) == 0
        before = bytes(state)
        result = 5 if port == 0 else 6
        assert txmode(port, base, conf, 1 << 25) == result
        assert load(C.byref(state), base, conf, 1 << 25, 0, 0, 0) == result
        bits, mask = b(0xa5), b(0x5a)
        assert edge(C.byref(state), base, 1 << 25, False, C.byref(bits), C.byref(mask)) == result
        assert bytes(state) == before and (bits.value, mask.value) == (0xa5, 0x5a)
        reset(C.byref(state), port)
        assert capture(C.byref(state), base, conf, 3, False, 1, 1) == 0
        before = bytes(state)
        assert rxmode(port, base | (1 << 21), conf, 3) == result
        assert capture(C.byref(state), base | (1 << 21), conf, 3, True, 0, 0) == result
        assert bytes(state) == before
        assert rxmode(port, base, conf, 0xff) == (0 if port == 0 else 6)
        assert txmode(port, base, conf, 1 << 24) == (7 if port == 0 else 6)
        assert txmode(port, base | (1 << 5), conf, (1 << 24) | (1 << 23)) == (7 if port == 0 else 6)
    assert txmode(2, base, conf, 0) == 6
    assert rxmode(2, base, conf, 3) == 6
    for invalid in (base & ~(1 << 12), base & ~(1 << 20), base | (1 << 19)):
        assert txmode(0, invalid, conf, 0) == 3
        assert rxmode(0, invalid, conf, 3) == 3
    return {'status': 'pass', 'random_seed': '0x5320261007', 'tx_frames': tx_frames, 'rx_frames': rx_frames, 'compared_edges': edges, 'rx_masks': 256, 'converter_state_and_output_atomicity': 'pass', 'port_capabilities': 'pass'}


try:
    version = run('gcc-version', ['gcc', '--version'])
    results['compiler'] = version.stdout.splitlines()[0]
    generated = run('reference-header', ['python3', SOURCE / 'pdm-reference-vectors.py', '--header', OUT / 'esp32s3-i2s-pdm-vectors.h'])
    assert generated.returncode == 0
    reference = run('reference-json', ['python3', SOURCE / 'pdm-reference-vectors.py'])
    assert reference.returncode == 0
    vectors = json.loads(reference.stdout)['vectors']
    results['checks']['reference_vectors'] = {'status': 'pass', 'count': len(vectors), 'edges': sum(len(v['edges']) for v in vectors)}
    flags = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DESP32S3_I2S_PDM_STANDALONE', '-I' + str(OUT), '-I' + str(COPIES)]
    files = [COPIES / 'esp32s3_i2s_pdm.c', COPIES / 'esp32s3-i2s-pdm-test.c']
    normal = run('compile-normal', flags + files + ['-o', OUT / 'esp32s3-i2s-pdm-test'])
    assert normal.returncode == 0
    normal_run = run('test-normal', [OUT / 'esp32s3-i2s-pdm-test'])
    results['checks']['authored_c_test'] = {'status': 'pass' if normal_run.returncode == 0 else 'fail'}
    sanitizer = run('compile-asan-ubsan', flags + ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] + files + ['-o', OUT / 'esp32s3-i2s-pdm-test-sanitized'])
    if sanitizer.returncode == 0:
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        sanitized_run = run('test-asan-ubsan', [OUT / 'esp32s3-i2s-pdm-test-sanitized'], env=env)
        results['checks']['asan_ubsan'] = {'status': 'pass' if sanitized_run.returncode == 0 else 'fail'}
    else:
        results['checks']['asan_ubsan'] = {'status': 'unavailable', 'reason': 'compiler failure recorded in compile-asan-ubsan.log'}
    shared = run('compile-shared', flags + ['-fPIC', '-shared', COPIES / 'esp32s3_i2s_pdm.c', '-o', OUT / 'libesp32s3-i2s-pdm.so'])
    assert shared.returncode == 0
    results['checks']['independent_math'] = mathematical_comparison(C.CDLL(str(OUT / 'libesp32s3-i2s-pdm.so')))
    results['status'] = 'pass' if all(v['status'] == 'pass' for v in results['checks'].values()) else 'fail'
except Exception:
    results['status'] = 'fail'
    results['failure'] = traceback.format_exc()
finally:
    results['source_unchanged'] = all(hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() == value for name, value in results['inputs'].items())
    if not results['source_unchanged']:
        results['status'] = 'fail'
        results['failure'] = 'source changed while standalone verification ran'
    results['generated_header_sha256'] = hashlib.sha256((OUT / 'esp32s3-i2s-pdm-vectors.h').read_bytes()).hexdigest() if (OUT / 'esp32s3-i2s-pdm-vectors.h').exists() else None
    (OUT / 'verification.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': results['status'], 'checks': results['checks'], 'failure': results.get('failure')}, indent=2))
raise SystemExit(0 if results['status'] == 'pass' else 1)
