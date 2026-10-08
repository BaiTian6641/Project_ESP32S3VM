#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native TCG lifecycle assertions; use the immutable verified QEMU image.

The original lane injects no firmware/instructions: CPU1 starts at the real ROM
reset vector before MESSAGE. The --counter-elf/--counter-flash lane boots the
locked native SDK fixture and observes actual CCOUNT, CCOMPARE1 and timer ISRs.
"""
import argparse
import json
import pathlib
import re
import socket
import subprocess
import time
import hashlib


COUNTER_WORDS = (
    'magic stage command cpu1_request cpu1_ack cpu0_count cpu0_loops '
    'cpu0_time_lo cpu0_time_hi cpu1_loops seed delta armed compare '
    'resumed timer_irqs timer_at external_irqs external_at '
    'gate_cpu0_start gate_cpu0_end gate_time_start gate_time_end '
    'gate_loops_start gate_loops_end gate_cpu1_loops gate_timer_irqs '
    'gate_external_irqs gate_resumed gate_timer_at gate_external_at '
    'stall_cpu0_start stall_cpu0_end stall_cpu1_loops stall_timer_irqs '
    'stall_resumed stall_timer_at reset_cpu0_start reset_cpu0_end cpu_hz external_intno'
).split()


def counter_main(args):
    """Real SDK instructions and timer ISR, with externally observed handshakes."""
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=False)
    transcript, observations, checks = [], {}, {}
    result = {'passed': False, 'criterion': 'physical-cpu1-counter-clock-gate',
              'checks': checks, 'observations': observations,
              'deadline_seconds': 90, 'hardware_used': False}
    deadline = time.monotonic() + 90
    sockets = []
    process = None
    try:
        for name, path in [('qemu', args.qemu), ('elf', args.counter_elf),
                           ('flash_input', args.counter_flash)]:
            result[name] = {'path': str(path.resolve()),
                            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        rom_dir = args.rom_dir or args.qemu.resolve().parent.parent / 'share/qemu'
        rom = rom_dir / 'esp32s3_rev0_rom.bin'
        result['rom'] = {'path': str(rom), 'sha256': hashlib.sha256(rom.read_bytes()).hexdigest()}
        symbols = subprocess.check_output(
            ['readelf', '-Ws', str(args.counter_elf.resolve())], text=True)
        (root / 'elf-symbols.txt').write_text(symbols)
        matches = re.findall(r'^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+OBJECT\s+'
                             r'GLOBAL\s+\S+\s+\S+\s+counter_state$', symbols, re.M)
        assert len(matches) == 1, 'Missing unique actual ELF counter_state'
        address = int(matches[0], 16)
        result['counter_state_address'] = hex(address)
        flash = root / 'counter.flash.bin'
        flash.write_bytes(args.counter_flash.read_bytes())
        qmp_path, qt_path = root / 'qmp.sock', root / 'qtest.sock'
        with (root / 'stderr.log').open('wb') as stderr, \
                (root / 'serial.log').open('wb') as serial:
            argv = [str(args.qemu.resolve()), '-M', 'esp32s3', '-accel', 'tcg',
                    '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
                    '-drive', f'file={flash},if=mtd,format=raw',
                    '-qmp', f'unix:{qmp_path},server=on,wait=off',
                    '-qtest', f'unix:{qt_path},server=on,wait=off']
            argv += ['-L', str(rom_dir)]
            result['argv'] = argv
            process = subprocess.Popen(argv, stdout=serial, stderr=stderr)
            for path in (qmp_path, qt_path):
                sock = socket.socket(socket.AF_UNIX)
                sock.settimeout(5)
                connect_deadline = time.monotonic() + 5
                while True:
                    try:
                        sock.connect(str(path))
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        assert process.poll() is None, 'QEMU exited before sockets'
                        assert time.monotonic() < connect_deadline, 'Socket deadline'
                        time.sleep(0.01)
                sockets.append(sock)
            qmp, qt = (sock.makefile('rwb', buffering=0) for sock in sockets)
            assert 'QMP' in json.loads(qmp.readline())

            def command(execute, arguments=None, optional_observer=False):
                request = {'execute': execute}
                if arguments is not None:
                    request['arguments'] = arguments
                qmp.write(json.dumps(request).encode() + b'\n')
                while True:
                    response = json.loads(qmp.readline())
                    transcript.append({'request': request, 'response': response})
                    if 'event' not in response:
                        if optional_observer and 'error' in response:
                            return None
                        assert 'return' in response, response
                        return response['return']

            command('qmp_capabilities')

            def mmio(operation, location, value=None):
                request = f'{operation} 0x{location:x}'
                if value is not None:
                    request += f' 0x{value:x}'
                qt.write(request.encode() + b'\n')
                response = qt.readline().decode().strip()
                assert response.startswith('OK'), response
                return int(response.split()[1], 0) if operation == 'readl' else None

            def state():
                return {name: mmio('readl', address + index * 4)
                        for index, name in enumerate(COUNTER_WORDS)}

            def wait_stage(stage):
                while mmio('readl', address + 4) != stage:
                    assert process.poll() is None, 'QEMU exited during guest proof'
                    if time.monotonic() >= deadline:
                        capture(f'deadline_stage_{stage}')
                        raise AssertionError(f'Guest stage {stage} deadline')
                    time.sleep(0.002)

            def send(value):
                mmio('writel', address + 8, value)

            def capture(name):
                command('stop')
                entry = {'guest': state(), 'ctrl0': mmio('readl', 0x600c0000)}
                entry['from_cpu3'] = mmio('readl', 0x600c003c)
                observations[name] = entry
                entry['qtree'] = command('human-monitor-command',
                                         {'command-line': 'info qtree'})
                for cpu in command('query-cpus-fast'):
                    index = cpu['cpu-index']
                    dump = command('human-monitor-command',
                                   {'command-line': f'info registers {index}'})
                    entry[f'cpu{index}_registers'] = dump
                    entry[f'cpu{index}_clock_period'] = command('qom-get', {
                        'path': cpu['qom-path'],
                        'property': 'clk-in-period'}, optional_observer=True)
                    for register in ('CCOUNT', 'CCOMPARE1', 'INTERRUPT', 'INTENABLE', 'PC'):
                        match = re.search(r'\b' + register + r'\s*=\s*'
                                          r'(?:0x)?([0-9a-fA-F]+)', dump)
                        if match:
                            key = 'intset' if register == 'INTERRUPT' else register.lower()
                            entry[f'cpu{index}_{key}'] = int(match[1], 16)
                observations[name] = entry
                command('cont')
                return entry

            wait_stage(1)
            capture('boot_ready')
            send(1)
            wait_stage(2)
            gate_first = capture('gate_first')
            wait_stage(3)
            gate_last = capture('gate_after_cpu0_600ms')
            # A second sample after asserting the real FROM_CPU interrupt.
            time.sleep(0.02)
            gate_irq = capture('gate_pending_external_irq')
            hold = gate_irq['guest']
            checks['gate_compare_not_pending'] = not (gate_irq['cpu1_intset'] & (1 << 15))
            external_mask = 1 << hold['external_intno']
            checks['gate_external_irq_enabled_and_pending'] = bool(
                gate_irq['cpu1_intset'] & gate_irq['cpu1_intenable'] & external_mask)
            checks['cpu0_counter_advances_gate_off'] = (
                (hold['gate_cpu0_end'] - hold['gate_cpu0_start']) & 0xffffffff
            ) >= hold['delta'] * 3
            checks['cpu0_actual_time_advances_gate_off'] = (
                (hold['gate_time_end'] - hold['gate_time_start']) & 0xffffffff
            ) >= 500000
            checks['cpu0_instructions_advance_gate_off'] = (
                hold['gate_loops_end'] > hold['gate_loops_start'])
            send(2)
            time.sleep(0.02)
            capture('gate_release_first_instructions')
            wait_stage(4)
            gate_release = capture('gate_released_timer_isr')
            send(3)
            wait_stage(5)
            stall_first = capture('runstall_first')
            wait_stage(6)
            stall_last = capture('runstall_after_cpu0_600ms')
            send(4)
            wait_stage(7)
            stall_release = capture('runstall_released_timer_isr')
            reset_before = mmio('readl', 0x60008050)
            resets = command('qom-get', {'path': '/machine/soc',
                                        'property': 'diag-cpu1-reset-count'})
            peripheral_before = {hex(location): mmio('readl', location) for location in
                                 (0x600c0010, 0x600c0018, 0x600c001c, 0x60023064)}
            send(5)
            wait_stage(8)
            time.sleep(0.02)
            reset = capture('cpu1_only_reset_cpu0_continues')
            result['reset_count_before'] = resets
            result['reset_count_after'] = command('qom-get', {
                'path': '/machine/soc', 'property': 'diag-cpu1-reset-count'})
            result['peripherals_before_reset'] = peripheral_before
            result['peripherals_after_reset'] = {location: mmio('readl', int(location, 16))
                                                  for location in peripheral_before}
            result['rtc_before_fixture_marker'] = reset_before
            result['rtc_after_reset'] = mmio('readl', 0x60008050)
            g, s, r = (entry['guest'] for entry in (gate_release, stall_release, reset))
            delta = g['delta']
            diff = lambda after, before: (after - before) & 0xffffffff
            checks.update({
                'gate_counter_known_nonzero': gate_first.get('cpu1_ccount', 0) != 0,
                'gate_actual_counter_frozen': (
                    gate_first.get('cpu1_ccount') == gate_last.get('cpu1_ccount') ==
                    gate_irq.get('cpu1_ccount') and
                    diff(g['gate_resumed'], g['armed']) < delta),
                'gate_compare_not_pending': not (
                    gate_irq.get('cpu1_intset', 0) & (1 << 15)),
                'gate_compare_no_isr': g['gate_timer_irqs'] == 0,
                'gate_external_irq_cannot_execute': (
                    g['gate_external_irqs'] == 0 and
                    gate_irq['guest']['cpu1_loops'] == gate_first['guest']['cpu1_loops']),
                'gate_timer_fires_after_remaining_ticks': (
                    diff(g['gate_timer_at'], g['compare']) < delta // 2 and
                    diff(g['gate_timer_at'], g['gate_resumed']) >
                    delta - diff(g['gate_resumed'], g['armed']) - delta // 4),
                'gate_external_irq_delivered_after_release': g['external_irqs'] == 1,
                'gate_external_irq_acknowledged': gate_release['from_cpu3'] == 0,
                'cpu0_counter_advances_gate_off': diff(
                    g['gate_cpu0_end'], g['gate_cpu0_start']) >= delta * 3,
                'cpu0_actual_time_advances_gate_off': diff(
                    g['gate_time_end'], g['gate_time_start']) >= 500000,
                'cpu0_instructions_advance_gate_off':
                    g['gate_loops_end'] > g['gate_loops_start'],
                'physical_clock_cpu1_zero_cpu0_positive':
                    gate_irq['cpu1_clock_period'] == 0 and
                    (gate_irq['cpu0_clock_period'] or 0) > 0,
                'cpu0_actual_clock_unchanged_by_cpu1_lifecycle': (
                    (gate_irq['cpu0_clock_period'] or 0) > 0 and
                    all(entry['cpu0_clock_period'] ==
                        observations['boot_ready']['cpu0_clock_period']
                        for entry in (gate_first, gate_last, gate_irq,
                                      gate_release, stall_first, stall_last,
                                      stall_release, reset))),
                'cpu1_actual_clock_restored_before_execution': (
                    (gate_release['cpu1_clock_period'] or 0) > 0 and
                    gate_release['cpu1_clock_period'] ==
                    observations['boot_ready']['cpu1_clock_period']),
                'runstall_clock_enabled': (stall_last['cpu1_clock_period'] or 0) > 0,
                'runstall_counter_advances': diff(
                    s['stall_resumed'], s['armed']) >= delta * 3,
                'runstall_timer_pending': bool(
                    stall_last.get('cpu1_intset', 0) & (1 << 15)),
                'runstall_execution_frozen':
                    stall_first['guest']['cpu1_loops'] == stall_last['guest']['cpu1_loops'],
                'runstall_isr_waits_for_release': s['stall_timer_irqs'] == 0 and
                    diff(s['stall_timer_at'], s['armed']) >= delta * 3,
                'reset_only_cpu1_count': result['reset_count_after'] == resets + 1,
                'reset_cpu0_continues': diff(
                    r['reset_cpu0_end'], r['reset_cpu0_start']) > 0,
                'reset_preserves_peripheral_state':
                    result['peripherals_after_reset'] == peripheral_before,
                'reset_preserves_rtc_marker': result['rtc_after_reset'] == 0xc001cafe,
                'reset_preserves_shared_guest_handshake': r['magic'] == 0xc001cafe,
            })
            result['passed'] = all(checks.values())
            result['classification'] = ('physical-clock-gate-qualified' if result['passed']
                                        else 'observed-criterion-failure')
            command('quit')
            process.wait(timeout=5)
            assert process.returncode == 0, process.returncode
    except Exception as error:
        result['error'] = str(error)
        result['classification'] = ('observed-criterion-failure'
                                    if any(value is False for value in checks.values())
                                    else 'fixture-observation-prerequisite-failure')
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        for sock in sockets:
            sock.close()
        (root / 'qmp.json').write_text(json.dumps(transcript, indent=2) + '\n')
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu', type=pathlib.Path)
    parser.add_argument('evidence', type=pathlib.Path)
    parser.add_argument('--cold-hold-only', action='store_true')
    parser.add_argument('--cold-direct-release', action='store_true')
    parser.add_argument('--counter-elf', type=pathlib.Path)
    parser.add_argument('--counter-flash', type=pathlib.Path)
    parser.add_argument('--rom-dir', type=pathlib.Path)
    args = parser.parse_args()
    if args.counter_elf or args.counter_flash:
        assert args.counter_elf and args.counter_flash, 'Counter ELF and flash required'
        raise SystemExit(counter_main(args))
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=False)
    qmp_path, qt_path = root / 'qmp.sock', root / 'qtest.sock'
    trace = root / 'exec.log'
    transcript = []
    with (root / 'stderr.log').open('wb') as stderr:
        process = subprocess.Popen([
            str(args.qemu.resolve()), '-M', 'esp32s3', '-accel', 'tcg',
            '-S', '-display', 'none', '-serial', 'null', '-monitor', 'none',
            '-qmp', f'unix:{qmp_path},server=on,wait=off',
            '-qtest', f'unix:{qt_path},server=on,wait=off',
            '-d', 'exec,nochain', '-D', str(trace)], stderr=stderr,
            stdout=subprocess.DEVNULL)
        sockets = []
        try:
            for path in (qmp_path, qt_path):
                sock = socket.socket(socket.AF_UNIX)
                sock.settimeout(5)
                deadline = time.monotonic() + 5
                while True:
                    try:
                        sock.connect(str(path))
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        assert process.poll() is None, 'QEMU exited before sockets'
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.01)
                sockets.append(sock)
            qmp, qt = (sock.makefile('rwb', buffering=0) for sock in sockets)
            assert 'QMP' in json.loads(qmp.readline())

            def command(execute, arguments=None):
                request = {'execute': execute}
                if arguments is not None:
                    request['arguments'] = arguments
                qmp.write(json.dumps(request).encode() + b'\n')
                while True:
                    response = json.loads(qmp.readline())
                    transcript.append(response)
                    if 'event' not in response:
                        assert 'return' in response, response
                        return response['return']

            command('qmp_capabilities')

            def mmio(operation, address, value=None):
                request = f'{operation} 0x{address:x}'
                if value is not None:
                    request += f' 0x{value:x}'
                qt.write(request.encode() + b'\n')
                response = qt.readline().decode().strip()
                assert response.startswith('OK'), response
                return int(response.split()[1], 0) if operation == 'readl' else None

            def write(value):
                mmio('writel', 0x600c0000, value)

            def prop(name):
                return command('qom-get', {'path': '/machine/soc', 'property': name})

            def registers(cpu):
                dump = command('human-monitor-command',
                               {'command-line': f'info registers {cpu}'})
                # This original lane compares execution state. RUNSTALL with
                # a positive clock does not freeze CCOUNT; the separate native
                # SDK lane asserts its actual counter/timer behavior.
                return re.sub(r'(\bCCOUNT\s*=\s*)(?:0x)?[0-9a-fA-F]+',
                              r'\1<clock-derived>', dump)

            def run():
                command('cont')
                time.sleep(0.02)
                command('stop')

            assert mmio('readl', 0x600c0000) == 4
            assert mmio('readl', 0x600c0004) == 0
            cold = registers(1)
            vector_match = re.search(r'PC\s*=\s*(?:0x)?([0-9a-fA-F]+)', cold)
            assert vector_match, cold
            vector = int(vector_match.group(1), 16)
            assert vector != 0
            # Stall CPU0 independently so its ROM cannot release CPU1 or change
            # the tested registers. CPU1 has not executed even once (-S).
            options = mmio('readl', 0x60008000)
            mmio('writel', 0x600080bc, 0x21 << 26)
            mmio('writel', 0x60008000, (options & ~12) | 8)
            cpu0 = registers(0)
            if args.cold_hold_only:
                run()
                assert registers(1) == cold, 'Cold-held CPU1 executed'
                assert registers(0) == cpu0
                assert prop('diag-cpu1-held') is True
                assert prop('diag-cpu1-reset-count') == 0
                command('quit')
                process.wait(timeout=5)
                assert process.returncode == 0
                assert not re.search(r'Trace\s+1:', trace.read_text()), \
                    'Cold-held CPU1 polled ROM'
                (root / 'result.json').write_text(json.dumps({
                    'passed': True, 'checks': ['cold-hold-no-native-execution'],
                    'rom_vector': hex(vector)}, indent=2) + '\n')
                return
            if args.cold_direct_release:
                write(7)
                assert prop('diag-cpu1-stalled') is True
            else:
                # A pure RUNSTALL edge: no reset callback can clear HALT.
                # No CPU has executed yet (-S).
                write(2)
                write(3)
                assert prop('diag-cpu1-stalled') is True
                assert prop('diag-cpu1-halt-pending') is True
            # Clear RUNSTALL before first CPU execution and
            # before publishing MESSAGE. A stale HALT request breaks this.
            write(2)
            assert prop('diag-cpu1-stalled') is False
            assert prop('diag-cpu1-halt-pending') is False
            assert mmio('readl', 0x600c0004) == 0
            run()
            live = registers(1)
            assert live != cold, 'CPU1 never progressed from cold ROM state'
            assert registers(0) == cpu0, 'CPU0 unexpectedly executed'
            resets = prop('diag-cpu1-reset-count')
            for control in (3, 0, 1):
                write(control)
                assert prop('diag-cpu1-stalled') is True
                frozen = registers(1)
                run()
                assert registers(1) == frozen, f'CPU1 progressed under CTRL0={control}'
                assert prop('diag-cpu1-reset-count') == resets
            write(2)
            mmio('writel', 0x600080bc, (0x21 << 26) | (0x21 << 20))
            mmio('writel', 0x60008000, (options & ~15) | 10)
            assert prop('diag-rtc-cpu1-stall') is True
            frozen = registers(1)
            run()
            assert registers(1) == frozen, 'RTC stall failed to hold CPU1'
            mmio('writel', 0x600080bc, 0x21 << 26)
            assert prop('diag-cpu1-stalled') is False
            # Request the reset while native CPU1 is actually running.
            command('cont')
            write(6)
            command('stop')
            assert prop('diag-cpu1-reset-count') == resets + 1
            reset_state = registers(1)
            match = re.search(r'PC\s*=\s*(?:0x)?([0-9a-fA-F]+)', reset_state)
            assert match and int(match.group(1), 16) == vector, reset_state
            write(6)
            run()
            assert prop('diag-cpu1-reset-count') == resets + 1
            assert registers(1) == reset_state
            assert registers(0) == cpu0
            write(2)
            run()
            assert registers(1) != reset_state, 'CPU1 did not resume after reset'
            command('quit')
            process.wait(timeout=5)
            assert process.returncode == 0
            text = trace.read_text()
            cpu1_trace = [line for line in text.splitlines()
                          if re.search(r'Trace\s+1:', line)]
            assert cpu1_trace, 'No native CPU1 translated block executed'
            assert f'{vector:08x}' in cpu1_trace[0].lower(), cpu1_trace[0]
            (root / 'result.json').write_text(json.dumps({
                'passed': True, 'rom_vector': hex(vector),
                'first_cpu1_trace': cpu1_trace[0],
                'cpu1_trace_blocks': len(cpu1_trace),
                'reset_count_before': resets,
                'reset_count_after': resets + 1,
                'checks': ['cold-release-before-first-execution',
                           'release-before-message', 'native-ROM-progress',
                           'gate-and-runstall-freeze-no-reset', 'RTC-independent-OR',
                           'reset-while-running-CPU1-only', 'same-level-no-reset',
                           'post-reset-native-ROM-progress']}, indent=2) + '\n')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for sock in sockets:
                sock.close()
            (root / 'qmp.json').write_text(json.dumps(transcript, indent=2) + '\n')


if __name__ == '__main__':
    main()
