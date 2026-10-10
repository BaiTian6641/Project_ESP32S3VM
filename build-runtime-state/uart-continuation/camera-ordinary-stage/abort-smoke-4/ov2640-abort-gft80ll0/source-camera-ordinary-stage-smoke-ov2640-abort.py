#!/usr/bin/env python3
"""Source-bound native producer smoke, not an ordinary SDK acceptance shortcut.

Uses the existing pulse QMP/qtest transports and LCD source authority. Writes
only real GPIO/LEDC/I2C peripheral registers. Sensor registers travel through
actual timed SCCB transactions; no sensor setter, payload or input injection.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import operator
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
GPIO, IOMUX, SYSTEM, LEDC, I2C = 0x60004000, 0x60009000, 0x600c0000, 0x60019000, 0x60027000


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    sys.modules[name] = value
    spec.loader.exec_module(value)
    return value


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def require(value, message):
    if not value:
        raise RuntimeError(message)


def official_sequence(component):
    """Evaluate only integer expressions in untouched upstream constant tables."""
    regs = (component / 'sensors/private_include/ov2640_regs.h').read_text()
    settings = (component / 'sensors/private_include/ov2640_settings.h').read_text()
    macros = {}
    for match in re.finditer(r'^#define\s+(\w+)(\([^\n]*?\))?[ \t]+([^\n]+)', regs, re.M):
        name, params, expression = match.groups()
        expression = re.sub(r'/\*.*?\*/', '', expression).strip()
        macros[name] = (None if params is None else [p.strip() for p in params[1:-1].split(',')], expression)
    bank_enum = re.search(r'typedef enum\s*\{\s*(BANK_DSP[^}]+)\}', regs).group(1)
    banks = {name.strip(): i for i, name in enumerate(bank_enum.split(','))}
    binary = {ast.BitOr: operator.or_, ast.BitAnd: operator.and_, ast.LShift: operator.lshift,
              ast.RShift: operator.rshift, ast.Add: operator.add, ast.Sub: operator.sub}

    def expression(text, names=None):
        names = names or {}
        def node(value):
            if isinstance(value, ast.Constant) and type(value.value) is int:
                return value.value
            if isinstance(value, ast.Name):
                if value.id in names:
                    return names[value.id]
                if value.id in banks:
                    return banks[value.id]
                params, body = macros[value.id]
                require(params is None, 'Function macro used without arguments')
                return expression(body, names)
            if isinstance(value, ast.BinOp) and type(value.op) in binary:
                return binary[type(value.op)](node(value.left), node(value.right))
            if isinstance(value, ast.UnaryOp) and isinstance(value.op, ast.Invert):
                return ~node(value.operand)
            if isinstance(value, ast.Call) and isinstance(value.func, ast.Name):
                params, body = macros[value.func.id]
                require(params is not None and len(params) == len(value.args), 'Official macro argument count')
                return expression(body, dict(zip(params, map(node, value.args))))
            raise RuntimeError('Unsupported official integer expression: ' + text)
        return node(ast.parse(text.strip(), mode='eval').body)

    def table(name):
        body = re.search(re.escape(name) + r'\[\]\[2\]\s*=\s*\{(.*?)^\};', settings, re.M | re.S).group(1)
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', body, flags=re.S)
        pairs = []
        for row in re.findall(r'\{([^{}]+)\}', body):
            depth, split = 0, None
            for index, char in enumerate(row):
                if char == '(':
                    depth += 1
                elif char == ')':
                    depth -= 1
                elif char == ',' and depth == 0:
                    require(split is None, 'Unexpected official table width')
                    split = index
            require(split is not None, 'Missing official register/value separator')
            reg, value = expression(row[:split]), expression(row[split + 1:])
            if reg == 0:
                require(value == 0, 'Official table terminator differs')
                return pairs
            require(0 <= reg <= 255 and 0 <= value <= 255, 'Official SCCB byte range')
            pairs.append([reg, value])
        raise RuntimeError('Official register table lacks terminator')

    sensor = (component / 'driver/sensor.c').read_text()
    dimensions = re.search(r'\{\s*(\d+),\s*(\d+),\s*ASPECT_RATIO_4X3\s*\}\s*,\s*/\* QQVGA \*/', sensor)
    require(dimensions is not None, 'Official QQVGA dimensions missing')
    width, height = map(int, dimensions.groups())
    ratio = re.search(r'ratio_table\[\]\s*=\s*\{\s*//[^\n]*\n\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', settings)
    require(ratio is not None, 'Official 4x3 ratio missing')
    x, y, maximum_x, maximum_y = map(int, ratio.groups())
    x //= 4; y //= 4; maximum_x //= 4; maximum_y = min(maximum_y // 4, 296)
    maximum_x //= 4; maximum_y //= 4
    w, h = width // 4, height // 4
    window = [('BANK_SEL', banks['BANK_DSP']), ('HSIZE', maximum_x & 255), ('VSIZE', maximum_y & 255),
              ('XOFFL', x & 255), ('YOFFL', y & 255),
              ('VHYX', ((maximum_y >> 1) & 128) | ((y >> 4) & 112) | ((maximum_x >> 5) & 8) | ((x >> 8) & 7)),
              ('TEST', (maximum_x >> 2) & 128), ('ZMOW', w & 255), ('ZMOH', h & 255),
              ('ZMHH', ((h >> 6) & 4) | ((w >> 8) & 3))]
    sequence = table('ov2640_settings_cif')
    sequence += [[expression('BANK_SEL'), banks['BANK_DSP']], [expression('R_BYPASS'), expression('R_BYPASS_DSP_BYPAS')]]
    sequence += table('ov2640_settings_to_cif')
    sequence += [[expression(reg), value] for reg, value in window]
    # Exact ESP32-S3 non-JPEG CIF set_window: clk_2x=1, clk_div=3,
    # pclk_auto=1, pclk_div=8, followed by official RGB565 format table.
    sequence += [[expression('BANK_SEL'), banks['BANK_SENSOR']], [expression('CLKRC'), 0x83],
                 [expression('BANK_SEL'), banks['BANK_DSP']], [expression('R_DVP_SP'), 0x88],
                 [expression('R_BYPASS'), expression('R_BYPASS_DSP_EN')]]
    sequence += table('ov2640_settings_rgb565')
    return sequence, (width, height)


def main():
    parser = argparse.ArgumentParser()
    for name in ('qemu', 'qemu-source', 'runtime-manifest', 'prepared-source-record', 'merged-flash', 'preparation-receipt', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.profile = 'camera-rgb565'
    args.evidence.mkdir(parents=True, exist_ok=False)
    native = Path(tempfile.mkdtemp(prefix='ov2640-abort-', dir=Path.home() / '.cache/esp32s3vm/lcd-cam-ordinary-live'))
    pulse = module(ROOT / 'qemu-extensions/prototypes/pulse/run-native-fixture.py', 'ov_abort_pulse')
    collector = module(ROOT / 'qemu-extensions/prototypes/lcd-cam/run-native-fixture.py', 'ov_abort_collector')
    proof = collector.Provenance(native)
    result = {'status': 'FAIL', 'boundary': 'Native source producer partial-abort diagnostics only; qtest does not execute the source-bound compiled SDK image; no ordinary SDK/LCDRX/DMA/physical metrology qualification', 'evidence': str(native), 'phases': []}
    proc = q = t = None
    try:
        for source in (Path(__file__).resolve(), ROOT / 'qemu-extensions/prototypes/pulse/run-native-fixture.py',
                       ROOT / 'qemu-extensions/prototypes/lcd-cam/run-native-fixture.py',
                       ROOT / 'qemu-extensions/prototypes/memory/run-lcd-cam-consumers.py'):
            proof.pin(source)
            shutil.copyfile(source, native / ('source-' + source.parent.name + '-' + source.name))
        proof.runtime(args)
        preparation = json.loads(proof.pin(args.preparation_receipt).read_text())
        workspace = next(row for row in preparation['sdk_projects'] if row['sdk'] == 'idf-6.1')
        fixture = Path(workspace['project_path'])
        args.native_fixture = fixture
        proof.official_camera(args)
        graphs = module(proof.pin(fixture / 'graph_vectors.py'), 'ov_abort_graph')
        graph = graphs.project('camera-rgb565')
        save(native / 'project.json', graph)
        component = fixture / 'components/esp32-camera'
        for relative in ('sensors/ov2640.c', 'sensors/private_include/ov2640_regs.h', 'sensors/private_include/ov2640_settings.h', 'driver/sensor.c'):
            proof.pin(component / relative)
        sequence, configured_geometry = official_sequence(component)
        save(native / 'official-sccb-sequence.json', {'sequence': sequence, 'configured_geometry': configured_geometry,
             'primary': 'Unmodified official v2.1.8 ov2640_settings_cif, set_framesize QQVGA/set_window CIF and ov2640_settings_rgb565; MMIO master setup from esp32s3-i2c-test.c, real20M from qualified parent clock helper'})
        qpath, tpath = native / 'qmp.sock', native / 'qtest.sock'
        command = [str(args.qemu), '-machine', 'esp32s3', '-S', '-nographic', '-monitor', 'none', '-serial', 'none', '-accel', 'qtest',
                   '-qmp', f'unix:{qpath},server=on,wait=off', '-qtest', f'unix:{tpath},server=on,wait=off', '-qtest-log', '/dev/null']
        save(native / 'command.json', command)
        with (native / 'qmp.log').open('w') as qlog, (native / 'qtest.log').open('w') as tlog, (native / 'stderr.log').open('wb') as errors:
            proc = subprocess.Popen(command, stderr=errors)
            deadline = time.monotonic() + 900
            while not qpath.exists() or not tpath.exists():
                require(proc.poll() is None, 'Native QEMU startup failed')
                require(time.monotonic() < deadline, 'Native startup deadline')
                time.sleep(.02)
            q, t = pulse.Qmp(qpath, deadline, qlog), pulse.Qtest(tpath, deadline, tlog)
            t.write(SYSTEM + 0x60, 0x00028401)
            t.bits(SYSTEM + 0x18, (1 << 11) | (1 << 18), True)
            for pin in range(1, 18):
                t.write(IOMUX + 4 * (pin + 1), (1 << 12) | (1 << 9))
            for pin in (13, 14):
                t.write(GPIO + 0x554 + 4 * pin, 256 | (1 << 10))
            t.write(GPIO + 8, 1 << 13)
            t.write(GPIO + 12, 1 << 14)
            t.write(GPIO + 0x24, (1 << 13) | (1 << 14))
            for pin, signal in ((1, 92), (2, 91)):
                t.write(GPIO + 0x74 + 4 * pin, 1 << 2)
                t.write(GPIO + 0x154 + 4 * signal, (1 << 7) | pin)
                t.write(GPIO + 0x554 + 4 * pin, signal)
            t.write(I2C, 199); t.write(I2C + 0x38, 200)
            t.write(I2C + 0x54, 1 << 21); t.write(I2C + 0xc, (1 << 5) | 16)
            t.write(GPIO + 0x554 + 4 * 15, 73)
            q.call('qom-set', {'path': collector.BASE.ELECTRICAL, 'property': 'project-json', 'value': json.dumps(graph)})
            q.call('cont')
            # Real powered settling elapses before enabling the full-rate clock;
            # no SCCB transaction or streaming assertion happens without XCLK.
            t.step(10000000)
            t.write(LEDC + 0xd0, 1)
            timer = (512 << 4) | 1
            t.write(LEDC + 0xa0, timer | (1 << 23) | (1 << 25))
            t.write(LEDC + 4, 0); t.write(LEDC + 8, 16); t.write(LEDC + 12, 1 << 30)
            t.write(LEDC, (1 << 2) | (1 << 4)); t.write(LEDC + 0xa0, timer)
            edges = []
            epoch = t.step(0)
            for index in range(256):
                now = epoch if index == 0 else t.step(25)
                snap = q.snapshot()
                actual = pulse.level(snap, 15)
                require(now == epoch + index * 25 and actual == int(index % 2 == 0), 'Actual full-rate GPIO15 edge mismatch')
                edges.append({'ns': now, 'gpio15': pulse.pad(snap, 15), 'solver_ns': snap['timestamp_ns']})
            save(native / 'clock-edges.json', edges)

            def observe(phase):
                now = t.step(0)
                snap = q.snapshot()
                source = json.loads(q.call('qom-get', {'path': collector.BASE.SENSORS, 'property': 'status-json'}))
                require(len(source) == 1 and source[0]['component_id'] == 'D', 'Actual source identity differs')
                status = source[0]
                controls = {str(pin): pulse.pad(snap, pin) for pin in (1, 2, 13, 14, 15, 16, 17)}
                for pin in (1, 2, 13, 14, 15):
                    pulse.level(snap, pin)
                row = {'phase': phase, 'ns': now, 'source': status, 'pads': controls, 'electrical': snap}
                result['phases'].append(row)
                with (native / 'phases.jsonl').open('a') as journal:
                    journal.write(json.dumps(row, separators=(',', ':')) + '\n')
                return row

            def transaction(tx, reading=False):
                t.write(I2C + 4, (1 << 10) | (1 << 4) | 3)
                t.write(I2C + 0x18, (1 << 12) | (1 << 13)); t.write(I2C + 0x24, 0x3ffff)
                for index in range(8): t.write(I2C + 0x58 + 4 * index, 4 << 11)
                for value in tx: t.write(I2C + 0x1c, value)
                commands = [6 << 11, (1 << 11) | (1 << 8) | len(tx)]
                if reading: commands.append((3 << 11) | (1 << 10) | 1)
                commands.append(2 << 11)
                for index, value in enumerate(commands): t.write(I2C + 0x58 + 4 * index, value)
                t.write(I2C + 4, (1 << 4) | (1 << 5) | 3)
                start = t.step(0)
                for _ in range(500):
                    t.step(10000)
                    raw = t.read(I2C + 0x20)
                    if raw & ((1 << 7) | (1 << 8) | (1 << 10)): break
                require(raw & ((1 << 7) | (1 << 8) | (1 << 10)) == 1 << 7, f'Actual SCCB transfer failed: {tx}, raw={raw:#x}')
                data = t.read(I2C + 0x1c) if reading else None
                with (native / 'sccb-transactions.jsonl').open('a') as log:
                    log.write(json.dumps({'tx': tx, 'reading': reading, 'rx': data, 'start_ns': start, 'end_ns': t.step(0), 'interrupt_raw': raw}) + '\n')
                return data

            def write(reg, value): transaction([0x60, reg, value])
            def configure():
                write(0xff, 1)
                identity = []
                for reg in (0x0a, 0x0b, 0x1c, 0x1d):
                    transaction([0x60, reg]); identity.append(transaction([0x61], True))
                require(identity == [0x26, 0x41, 0x7f, 0xa2], 'Real SCCB immutable PID/VER/MID mismatch')
                for reg, value in sequence: write(reg, value)
                row = observe('configured-real-sccb')
                require(row['source']['powered'] and row['source']['sccb_ready'] and row['source']['xclk_period_ns'] == 50 and not row['source']['failed'], 'Real powered/clock/source readiness failed')
                require((row['source']['width'], row['source']['height']) == configured_geometry and row['source']['format'] == 'rgb565', 'Source-derived geometry/format differs from official programming')

            def active(phase):
                # Observer cadence only: clock_step still executes every real
                # 25ns LEDC and 400ns DVP timer deadline inside each interval.
                for _ in range(2000):
                    t.step(10000)
                    row = observe(phase)
                    status = row['source']
                    if status['active_frame_bytes'] > 0 and pulse.level(row['electrical'], 17) == 1 and pulse.level(row['electrical'], 16) == 0:
                        require(status['streaming'] and status['active_cycle'] > 0 and status['active_frame_bytes'] < status['width'] * status['height'] * 2, 'Source is not genuinely mid-active frame')
                        return row
                raise RuntimeError('No actual active HREF/VSYNC source phase')

            observe('powered-clock-ready')
            configure()
            for fault in ('pwdn', 'reset'):
                before = active('active-before-' + fault)
                fault_ns = t.step(0)
                t.write(GPIO + (8 if fault == 'pwdn' else 12), 1 << (14 if fault == 'pwdn' else 13))
                after = observe('physical-' + fault + '-asserted')
                a, b = after['source'], before['source']
                require(not a['streaming'] and a['active_frame_bytes'] == 0 and a['partial_frame_aborts'] == b['partial_frame_aborts'] + 1, 'Actual partial-abort counter/state transition failed')
                require(a['last_abort_bytes'] == b['active_frame_bytes'] and a['last_abort_ns'] == fault_ns and a['last_abort_reason'] == ('hardware PWDN high' if fault == 'pwdn' else 'reset settling'), 'Actual source abort bytes/time/reason mismatch')
                require(pulse.level(after['electrical'], 14 if fault == 'pwdn' else 13) == int(fault == 'pwdn'), 'Fault control pad was not physically asserted')
                t.write(GPIO + (12 if fault == 'pwdn' else 8), 1 << (14 if fault == 'pwdn' else 13))
                if fault == 'reset':
                    # Keep each existing transport call below its 10s socket
                    # bound; total genuine settling remains exactly 10ms.
                    for _ in range(100):
                        t.step(100000)
                else:
                    t.step(200)
                configure()
                recovered = active('active-after-' + fault + '-sccb-recovery')
                require(recovered['source']['last_abort_ns'] >= fault_ns and recovered['source']['xclk_period_ns'] == 50, 'Actual source recovery lost clock/abort lineage')
            result['status'] = 'PASS_NATIVE_OV2640_PARTIAL_ABORT_DIAGNOSTICS_ONLY'
    except Exception as error:
        result['error'] = str(error)
        (native / 'failure-traceback.log').write_text(traceback.format_exc())
    finally:
        for transport in (q, t):
            if transport is not None:
                try: transport.close()
                except Exception: pass
        if proc is not None:
            proc.terminate()
            try: proc.wait(10)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait(10)
        for path in (native / 'qmp.sock', native / 'qtest.sock'):
            if path.exists(): path.unlink()
        result['input_hashes'] = proof.hashes
        result['raw_hashes'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in native.iterdir() if path.is_file()}
        save(native / 'result.json', result)
        shutil.copytree(native, args.evidence / native.name)
    print(json.dumps({'status': result['status'], 'error': result.get('error'), 'evidence': str(native), 'archive': str(args.evidence)}))
    return 0 if result['status'].startswith('PASS_') else 1


if __name__ == '__main__':
    raise SystemExit(main())
