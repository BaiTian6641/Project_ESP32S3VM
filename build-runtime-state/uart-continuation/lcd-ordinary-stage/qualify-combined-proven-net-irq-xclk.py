import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time

stage = Path(__file__).resolve().parent
root = stage.parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--pin', required=True, type=int)
parser.add_argument('--sense', required=True, type=int)
args = parser.parse_args()
assert args.pin != args.sense and 0 <= args.pin <= 48 and 0 <= args.sense <= 48
spec = importlib.util.spec_from_file_location('pulse_runner', root / 'qemu-extensions/prototypes/pulse/run-native-fixture.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
qemu = Path('/home/polar/.cache/esp32s3vm/qemu-uart-40edccac4156-02c5b1fd51470c43/build-wsl/qemu-system-xtensa')
expected_binary = 'bf3fc1bd42624bdaecd9a7caf0c2fa92474ae1a795e3c09a4b816702bea1b931'
assert hashlib.sha256(qemu.read_bytes()).hexdigest() == expected_binary
results = []
for mode in ('connected', 'disconnected'):
    evidence = stage / ('xclk-proven-net-irq-20mhz-gpio' + str(args.pin) + '-' + mode)
    evidence.mkdir(exist_ok=False)
    graph = r.graph('connected')
    graph['id'] = 'pulse-xclk-20mhz-' + mode
    graph['runtime']['electrical']['mode'] = 'dc'
    graph['components'] = [c for c in graph['components'] if c['id'] in ('U1', 'V', 'G', 'PullCount')]
    graph['components'][0]['terminals'] = [t for t in graph['components'][0]['terminals'] if t['id'] in ('U1.vdd', 'U1.gnd')]
    for pin in (args.pin, args.sense):
        graph['components'][0]['terminals'].append({'id': f'U1.io{pin}', 'name': f'io{pin}', 'role': 'gpio', 'domain': 'digital', 'direction': 'inout', 'gpio': pin})
    graph['nets'] = [
        {'id': 'gnd', 'name': 'gnd', 'endpoints': ['G.ref', 'V.n', 'U1.gnd', 'PullCount.b']},
        {'id': 'vdd', 'name': 'vdd', 'endpoints': ['V.p', 'U1.vdd']},
        {'id': 'clock', 'name': 'clock', 'endpoints': [f'U1.io{args.sense}', 'PullCount.a'] + ([f'U1.io{args.pin}'] if mode == 'connected' else [])}]
    if mode == 'disconnected': graph['nets'].append({'id': 'source', 'name': 'source', 'endpoints': [f'U1.io{args.pin}']})
    r.store(evidence / 'project.json', graph)
    records = []
    with tempfile.TemporaryDirectory(prefix='pulse-xclk-') as tmp, (evidence / 'qmp.log').open('w') as qlog, (evidence / 'qtest.log').open('w') as tlog, (evidence / 'stderr.log').open('wb') as errors:
        qpath, tpath = Path(tmp) / 'qmp.sock', Path(tmp) / 'qtest.sock'
        command = [str(qemu), '-machine', 'esp32s3', '-S', '-nographic', '-monitor', 'none', '-serial', 'none', '-accel', 'qtest', '-qmp', f'unix:{qpath},server=on,wait=off', '-qtest', f'unix:{tpath},server=on,wait=off', '-qtest-log', '/dev/null']
        r.store(evidence / 'command.json', command)
        proc = subprocess.Popen(command, stderr=errors)
        try:
            deadline = time.monotonic() + 300
            while not qpath.exists() or not tpath.exists():
                if proc.poll() is not None: raise RuntimeError('QEMU startup failed')
                time.sleep(.02)
            q = r.Qmp(qpath, deadline, qlog)
            t = r.Qtest(tpath, deadline, tlog)
            t.write(0x600c0060, 0x00028401)  # accepted common owner APB80MHz
            t.bits(0x600c0018, 1 << 11, True)
            for pin in (args.pin, args.sense): t.write(0x60009000 + 4 * (pin + 1), (1 << 12) | (1 << 9))
            t.write(0x60004000 + 0x554 + 4 * args.pin, 73)
            q.call('qom-set', {'path': '/machine/soc/electrical', 'property': 'project-json', 'value': json.dumps(graph)})
            q.call('cont')
            base = 0x60019000
            t.write(base + 0xd0, 1)
            timer = (512 << 4) | 1  # APB /2, resolution1 =>20MHz,25ns half period
            t.write(base + 0xa0, timer | (1 << 23) | (1 << 25))
            t.write(base + 4, 0)
            t.write(base + 8, 1 << 4)
            t.write(base + 12, 1 << 30)
            t.write(base, (1 << 2) | (1 << 4))
            t.write(base + 0xa0, timer)
            epoch = t.step(0)
            for index in range(256):
                ns = t.step(25) if index else epoch
                snap = q.snapshot()
                source = r.pad(snap, args.pin)
                sense = r.pad(snap, args.sense)
                expected_high = index % 2 == 0
                assert ns == epoch + index * 25
                assert r.level(snap, args.pin) == int(expected_high)
                assert r.level(snap, args.sense) == int(expected_high and mode == 'connected')
                assert source['digital_valid'] and sense['digital_valid']
                assert int(snap['timestamp_ns']) == ns
                records.append({'ns': ns, 'expected_source_high': expected_high, 'source': source, 'sense': sense, 'solver_ns': int(snap['timestamp_ns']), 'timer_count': t.read(base + 0xa4)})
            q.close(); t.close()
        finally:
            proc.terminate(); proc.wait(10)
            r.store(evidence / 'raw-edges.json', records)
    result = {'status': 'PASS', 'mode': mode, 'binary': str(qemu), 'binary_sha256': expected_binary, 'prepared_record': str(stage / 'runtime-preparation-proven-net-irq-tests/prepared-02c5b1fd51470c43-source.json'), 'prepared_record_sha256': r.digest(stage / 'runtime-preparation-proven-net-irq-tests/prepared-02c5b1fd51470c43-source.json'), 'pin': args.pin, 'sense_pin': args.sense, 'matrix_signal': 73, 'channel': 0, 'timer': 0, 'source_hz': 80000000, 'resolution_bits': 1, 'divider_fixed8': 512, 'period_ns': 50, 'high_ns': 25, 'observed_half_periods': len(records), 'evidence_hashes': {p.name: r.digest(p) for p in evidence.iterdir() if p.is_file()}, 'boundary': 'Real powered NativeNet20MHz resolved pad edges and real disconnected-wire negative; functional native timing, not independent silicon/board/camera metrology'}
    r.store(evidence / 'result.json', result)
    results.append(result)
r.store(stage / ('xclk-proven-net-irq-20mhz-gpio' + str(args.pin) + '-qualification.json'), results)
print(json.dumps(results, indent=2))
