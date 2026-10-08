#!/usr/bin/env python3
"""Launch ordinary CASE-selected IDF firmware and export real closed captures.

No sample injection, MMIO helper, phase search, or generated playback evidence.
Native transition records independently establish source epochs and complete
word boundaries; every selected actual sample is checked without payload fitting.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

if __package__:
    from . import peer_reference as reference
else:
    import peer_reference as reference

ROOT = Path(__file__).resolve().parent
PEERS = '/machine/soc/i2s-sample-peers'
ELECTRICAL = '/machine/soc/electrical'
PROFILES = ('connected', 'power-unknown', 'power-off', 'exhaustion', 'capture-overflow')


def require(value, message):
    if not value:
        raise ValueError(message)


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def records(text, tag):
    return [dict(re.findall(r'([a-z_]+)=([^\s]+)', line))
            for line in text.splitlines() if line.startswith(tag + ' ')]


class Qmp:
    def __init__(self, path, log):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(20)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile('rwb', buffering=0)
        self.log = log
        self.serial = 0
        greeting = json.loads(self.stream.readline())
        require('QMP' in greeting, 'missing real QMP greeting')
        self.record('greeting', greeting)
        self.call('qmp_capabilities')

    def record(self, kind, value):
        self.log.write(json.dumps(dict(kind=kind, value=value)) + '\n')
        self.log.flush()

    def call(self, command, arguments=None):
        self.serial += 1
        request = dict(execute=command, arguments=arguments or {}, id=self.serial)
        self.record('request', request)
        self.stream.write((json.dumps(request) + '\n').encode())
        while True:
            line = self.stream.readline()
            require(line, 'QMP connection closed')
            reply = json.loads(line)
            self.record('reply', reply)
            if 'event' in reply:
                continue
            require(reply.get('id') == self.serial, 'QMP response identity mismatch')
            require('error' not in reply, f'QMP error: {reply}')
            return reply['return']

    def get(self, path, prop):
        return json.loads(self.call('qom-get', dict(path=path, property=prop)))

    def set(self, path, prop, value):
        return self.call('qom-set', dict(path=path, property=prop, value=json.dumps(value)))

    def stop(self):
        self.call('stop')
        require(self.call('query-status')['running'] is False, 'VM not stopped for immutable export')

    def close(self):
        self.stream.close()
        self.sock.close()


class Uart:
    def __init__(self, path, log):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(.1)
        self.sock.connect(str(path))
        self.log = log
        self.data = bytearray()

    def send(self, data):
        self.sock.sendall(data)

    def receive(self):
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        require(data, 'UART connection closed')
        self.data.extend(data)
        self.log.write(data)
        self.log.flush()

    def text(self):
        return self.data.decode('utf-8', errors='replace')

    def until(self, tag, proc, timeout):
        deadline = time.monotonic() + timeout
        while not records(self.text(), tag):
            require(proc.poll() is None, f'QEMU exited {proc.returncode}')
            require(time.monotonic() < deadline, f'UART timeout waiting for {tag}')
            require('Guru Meditation' not in self.text() and 'abort() was called' not in self.text(), 'firmware abort')
            self.receive()
        result = records(self.text(), tag)
        require(len(result) == 1, f'duplicate UART {tag}')
        return result[0]

    def close(self):
        self.sock.close()


def graph_profile(document, profile):
    # Only peer rail or immutable config changes: MCU retains actual 3.3V.
    document = json.loads(json.dumps(document))
    peer = next(c for c in document['components'] if c['id'] == 'Peer')
    config = peer['attributes']['native_i2s_peer']
    if profile in ('power-unknown', 'power-off'):
        rail = next(n for n in document['nets'] if n['id'] == 'vdd')
        rail['endpoints'].remove('Peer.vdd')
        if profile == 'power-off':
            next(n for n in document['nets'] if n['id'] == 'gnd')['endpoints'].append('Peer.vdd')
        else:
            document['nets'].append(dict(id='unknown-peer-vdd', name='unknown-peer-vdd', endpoints=['Peer.vdd']))
    elif profile == 'exhaustion':
        config['repeat'] = False
    elif profile == 'capture-overflow':
        config['captureCapacity'] = 32
    else:
        require(profile == 'connected', 'unknown actual profile')
    return document


def peer_status(qmp):
    value = qmp.get(PEERS, 'status-json')
    require(value.get('version') == 1 and value.get('kind') == 'i2s-peer-status', 'unsupported actual peer status')
    peers = [p for p in value['peers'] if p['component_id'] == 'Peer']
    require(len(peers) == 1, 'registered actual Peer missing/duplicated')
    peer = peers[0]
    require(re.fullmatch('[0-9a-f]{64}', peer.get('config_identity', '')) is not None, 'missing genuine config identity')
    require(not peer.get('activation_error'), f'actual model activation failure: {peer}')
    return peer


def export_capture(qmp, path):
    pages = []
    offset = 0
    total = None
    identity = None
    while total is None or offset < total:
        qmp.set(PEERS, 'capture-request-json', dict(componentId='Peer', offset=offset, count=1024))
        page = qmp.get(PEERS, 'capture-json')
        require(page.get('version') == 1 and page.get('kind') == 'i2s-peer-din-capture', 'unavailable real DIN export')
        require(page['component_id'] == 'Peer' and page['offset'] == offset, 'capture window attribution mismatch')
        if total is None:
            total, identity = page['total'], page['config_identity']
        require(page['total'] == total and page['config_identity'] == identity, 'closed capture changed between windows')
        require(isinstance(page['events'], list) and len(page['events']) <= 1024, 'oversized public capture page')
        require(bool(page['events']) or total == 0, 'empty intermediate capture page')
        pages.append(page)
        offset += len(page['events'])
    require(offset == total, 'incomplete real capture export')
    save(path, pages)
    if total:
        reference.parse_din(path.read_bytes())
    return dict(total=total, config_identity=identity, sha256=sha256(path), pages=len(pages))


def transition_metadata(value):
    for name in ('version', 'capacity', 'count', 'first_index', 'first_sequence', 'total', 'lost', 'epoch'):
        require(type(value[name]) is int and value[name] >= 0, f'invalid actual transition {name}')
    require(value['version'] == 1 and value['capacity'] > 0, 'unsupported actual transition metadata')
    require(value['first_index'] < value['capacity'] and value['count'] <= value['capacity'], 'invalid native ring bounds')
    require(value['first_sequence'] == value['lost'] and value['total'] - value['first_sequence'] == value['count'],
            'native transition loss/count bounds disagree')
    return {key: item for key, item in value.items() if key not in ('capture_enabled', 'offset', 'records')}


def export_transitions(qmp, path, controller, din):
    require(qmp.call('query-status')['running'] is False, 'VM running during transition export')
    discovery = qmp.get(PEERS, 'status-json')
    require(discovery.get('version') == 1 and discovery.get('kind') == 'i2s-peer-status', 'missing native transition discovery')
    peers = [p for p in discovery['peers'] if p['component_id'] == 'Peer']
    cores = [c for c in discovery['controllers'] if c['controller'] == controller]
    require(len(peers) == len(cores) == 1 and cores[0]['capture_enabled'] is True, 'actual transition capture disabled/absent')
    require(peers[0]['config_identity'] == din['config_identity'], 'native transition discovery identity changed')
    metadata = {'peer': transition_metadata(peers[0]['transitions']),
                'controller': transition_metadata(cores[0])}
    offsets = {scope: value['first_sequence'] for scope, value in metadata.items()}
    pages = []
    while not pages or any(offsets[scope] < value['total'] for scope, value in metadata.items()):
        request = dict(componentId='Peer', offset=din['total'], count=1,
                       transitions=dict(controllerId=controller, peerOffset=offsets['peer'],
                                        controllerOffset=offsets['controller'], count=1024))
        qmp.set(PEERS, 'capture-request-json', request)
        page = qmp.get(PEERS, 'capture-json')
        require(page['version'] == 1 and page['kind'] == 'i2s-peer-din-capture' and
                page['component_id'] == 'Peer' and page['config_identity'] == din['config_identity'] and
                page['total'] == page['offset'] == din['total'] and page['events'] == [],
                'closed DIN identity/count changed during native transition export')
        transitions = page['transitions']
        require(transitions['version'] == 1, 'unsupported actual transition export')
        for scope in ('peer', 'controller'):
            window = transitions[scope]
            require(transition_metadata(window) == metadata[scope], 'closed transition metadata changed')
            require(window['offset'] == offsets[scope], 'transition export gap/reused page')
            records = window['records']
            require(isinstance(records, list) and len(records) == min(1024, metadata[scope]['total'] - offsets[scope]),
                    'actual transition page incomplete/oversized')
            require(all(type(record['sequence']) is int and record['sequence'] == offsets[scope] + index
                        for index, record in enumerate(records)), 'transition immutable sequence gap')
            offsets[scope] += len(records)
        pages.append(page)
    save(path, pages)
    return dict(path=str(path), sha256=sha256(path), pages=len(pages), scopes=metadata,
                all_retained_records_exported=True, startup_din_retained_separately=True)


def transition_attribution(pages, identity):
    core = [record for page in pages for record in page['transitions']['controller']['records']]
    peer = [record for page in pages for record in page['transitions']['peer']['records']]
    origin = [record for record in core if record['kind'] == 6 and record['source_word_id'] == 1]
    require(len(origin) == 1, 'fresh-VM first actual FIFO source LOAD missing/duplicated; source origin unqualified')
    core_epochs = sorted({record['epoch'] for record in core if record['kind'] == 7})
    peer_epochs = sorted({record['epoch'] for record in peer if record['kind'] == 3})
    require(core_epochs and peer_epochs, 'missing actual complete emission epochs')
    return dict(component_id='Peer', config_identity=identity, controller_origin_epoch=origin[0]['epoch'],
                controller_epochs=core_epochs, peer_epochs=peer_epochs, min_frames=768)


def expected_geometry(case, case_id):
    c = case['cmake']
    width = c['PEER_BITS']
    units = c['PEER_MASK'].bit_count()
    return dict(id=str(case_id), name=case['name'], format=c['PEER_FORMAT'], bits=str(width),
                slots=str(c['PEER_SLOTS']), mask=f"{c['PEER_MASK']:x}", master=str(c['PEER_MASTER']),
                controller=str(c['PEER_CONTROLLER']), bytes=str(96 * units * ((width + 7) // 8)))


def validate_guest(case, text):
    observation = records(text, 'I2S_PEER_OBS')
    timing = records(text, 'I2S_PEER_CADENCE')
    done = records(text, 'I2S_PEER_DONE')
    require(len(observation) == len(timing) == len(done) == 1, 'missing/duplicate guest observations')
    got = observation[0]
    phase = int(got['offset'])
    require(0 <= phase < 96, 'guest reported no complete frame phase')
    _, width, _, _ = reference.geometry(case)
    data = b''.join(value.to_bytes((width + 7) // 8, 'little')
                    for value, _ in reference.expected_events(case, 1, phase, 768))
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    wanted = f'{value:08x}'
    require(got['expected'] == got['observed'] == wanted, 'independent RX guest bytehash mismatch')
    require(got['match'] == '8/8' and int(got['frames']) == 768 and int(got['bytes']) == len(data), 'incomplete guest sustained descriptors')
    require(int(got['txeof']) >= 8 and int(got['rxeof']) >= 8 and done[0]['failures'] == '0', 'guest DMA/lifecycle failure')
    rate = 62500 if reference.geometry(case)[0] == 'raw-pdm' else 12500
    intervals, elapsed = int(timing[0]['intervals']), int(timing[0]['elapsed_us'])
    wanted_us = Fraction(intervals * 96 * 1000000, rate)
    require(timing[0]['frame_hz'] == f'{rate}/1' and timing[0]['eof_frames'] == '96', 'guest rational cadence geometry mismatch')
    require(intervals >= 3 and elapsed > 0 and abs(elapsed - wanted_us) <= max(Fraction(2000), wanted_us / 50), 'guest ISR-service cadence mismatch')
    return dict(observation=got, cadence=timing[0], independent_bytehash=wanted,
                scope='guest-buffer plus actual DMA callbacks; NOT physical bidirectional qualification')


def negative_boundary(profile, status):
    if profile == 'power-unknown':
        return status.get('power_known') is False and status.get('powered') is False
    if profile == 'power-off':
        return status.get('power_known') is True and status.get('registered_powered') is False and status.get('powered') is False
    if profile == 'exhaustion':
        return status.get('exhausted') is True
    if profile == 'capture-overflow':
        return status.get('capture_overflow') is True
    return False


def run_case(args, case_id, case, destination):
    destination.mkdir()
    captures = destination / 'rx'
    captures.mkdir()
    local_flash = destination / 'flash.bin'
    original = Path(args.flash).resolve()
    before = sha256(original)
    result = dict(case=case['name'], id=case_id, profile=args.profile, status='FAIL',
                  physical_bidirectional_qualified=False, source_flash_sha256_before=before)
    proc = qmp = uart = None
    peer_applied = False
    with tempfile.TemporaryDirectory(prefix='i2sp-') as sockets, (destination / 'qemu.log').open('wb') as log, \
            (destination / 'qmp.jsonl').open('w', encoding='utf-8') as qlog, (destination / 'uart.log').open('wb') as ulog:
        try:
            boot = json.loads((ROOT / case['bootGraph']).read_bytes())
            peer = graph_profile(json.loads((ROOT / case['peerGraph']).read_bytes()), args.profile)
            save(destination / 'boot.json', boot)
            save(destination / 'peer.json', peer)
            shutil.copyfile(original, local_flash)
            require(sha256(local_flash) == before, 'writable flash copy differs')
            qpath, upath = Path(sockets) / 'qmp', Path(sockets) / 'uart'
            command = [str(Path(args.qemu).resolve()), '-machine', 'esp32s3', '-nographic', '-S',
                       '-icount', 'shift=0,align=off,sleep=off',
                       '-serial', f'unix:{upath},server=on,wait=off',
                       '-drive', f'file={local_flash},if=mtd,format=raw',
                       '-qmp', f'unix:{qpath},server=on,wait=off',
                       '-global', f'driver=esp32s3-i2s,property=record-directory,value={captures}']
            result['command'] = command
            proc = subprocess.Popen(command, stdout=log, stderr=log)
            deadline = time.monotonic() + 30
            while not (qpath.exists() and upath.exists()):
                require(proc.poll() is None, f'QEMU exited {proc.returncode}')
                require(time.monotonic() < deadline, 'native sockets unavailable')
                time.sleep(.02)
            qmp, uart = Qmp(qpath, qlog), Uart(upath, ulog)
            qmp.set(ELECTRICAL, 'project-json', boot)
            save(destination / 'boot-snapshot.json', qmp.get(ELECTRICAL, 'snapshot-json'))
            qmp.call('cont')
            selection = uart.until('I2S_PEER_SELECT', proc, args.timeout)
            require(selection == dict(cases='68', protocol='CASE-zero-based'), 'wrong interactive fixture protocol')
            uart.send(f'CASE {case_id}\n'.encode())
            echo = uart.until('I2S_PEER_CASE', proc, args.timeout)
            require(echo == dict(id=str(case_id), name=case['name']), 'CASE selection echo mismatch')
            ready = uart.until('I2S_PEER_READY', proc, args.timeout)
            require(ready == expected_geometry(case, case_id), f'READY geometry mismatch: {ready}')
            qmp.stop()
            qmp.set(ELECTRICAL, 'project-json', peer)
            peer_applied = True
            save(destination / 'apply-snapshot.json', qmp.get(ELECTRICAL, 'snapshot-json'))
            result['initial_peer_status'] = peer_status(qmp)
            uart.send(b'G')
            qmp.call('cont')
            if args.profile == 'connected':
                uart.until('I2S_PEER_DONE', proc, args.timeout)
            else:
                deadline = time.monotonic() + args.timeout
                while True:
                    uart.receive()
                    status = peer_status(qmp)
                    if negative_boundary(args.profile, status):
                        break
                    require(proc.poll() is None and time.monotonic() < deadline, 'actual negative boundary not observed')
            qmp.stop()
            result['final_peer_status'] = peer_status(qmp)
            save(destination / 'final-snapshot.json', qmp.get(ELECTRICAL, 'snapshot-json'))
            result['din_capture'] = export_capture(qmp, destination / 'din-pages.json')
            result['transition_capture'] = export_transitions(qmp, destination / 'transition-pages.json',
                                                              case['cmake']['PEER_CONTROLLER'], result['din_capture'])
            if args.profile == 'connected':
                status = result['final_peer_status']
                require(status['power_known'] and status['registered_powered'] and status['powered'], 'unknown/off real peer rails')
                require(not status['paused'] and not status['dependency'] and not status['capture_overflow'] and not status['exhausted'], 'active peer dependency/overflow/exhaustion')
                result['guest_checks'] = validate_guest(case, uart.text())
                rx_path = captures / f"i2s{case['cmake']['PEER_CONTROLLER']}.rx.bin"
                controller, rx_events = reference.parse_rx(rx_path.read_bytes())
                require(controller == case['cmake']['PEER_CONTROLLER'], 'actual RX controller mismatch')
                require(len(rx_events) >= 768 * case['cmake']['PEER_MASK'].bit_count(), 'insufficient actual RX events')
                require(result['din_capture']['total'] >= 768 * (32 if case['cmake']['PEER_FORMAT'] == 'raw-pdm'
                        else case['cmake']['PEER_MASK'].bit_count()), 'insufficient actual DIN events')
                result['rx_capture'] = dict(path=str(rx_path), count=len(rx_events), sha256=sha256(rx_path))
                transition_path = destination / 'transition-pages.json'
                attribution = transition_attribution(json.loads(transition_path.read_bytes()), result['din_capture']['config_identity'])
                save(destination / 'transition-attribution.json', attribution)
                result['physical_checks'] = reference.qualify_transition_inputs(
                    case, rx_path.read_bytes(), (destination / 'din-pages.json').read_bytes(),
                    transition_path.read_bytes(), attribution)
                save(destination / 'physical-reference.json', result['physical_checks'])
                result['physical_bidirectional_qualified'] = True
                result['status'] = 'PASS_ACTUAL_PEER_BIDIRECTIONAL'
            else:
                require(negative_boundary(args.profile, result['final_peer_status']), 'negative boundary changed')
                require(not result['final_peer_status'].get('activation_error'), 'activation failure is not an electrical negative')
                if args.profile in ('power-unknown', 'power-off'):
                    require(result['din_capture']['total'] == 0, 'unpowered peer captured active samples')
                if args.profile == 'capture-overflow':
                    require(result['din_capture']['total'] == 32, 'overflow did not retain bounded real captures')
                result['status'] = 'PASS_ACTUAL_NEGATIVE_BOUNDARY'
        except Exception as exc:
            result['error'] = repr(exc)
            if qmp is not None and proc is not None and proc.poll() is None:
                try:
                    qmp.stop()
                    save(destination / 'failure-snapshot.json', qmp.get(ELECTRICAL, 'snapshot-json'))
                    if peer_applied:
                        result['failure_peer_status'] = peer_status(qmp)
                        result['din_capture'] = export_capture(qmp, destination / 'failure-din-pages.json')
                        result['transition_capture'] = export_transitions(qmp, destination / 'failure-transition-pages.json',
                                                                          case['cmake']['PEER_CONTROLLER'], result['din_capture'])
                except Exception as capture_error:
                    result['failure_export_error'] = repr(capture_error)
        finally:
            if uart is not None:
                uart.close()
            if qmp is not None:
                qmp.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            result['source_flash_sha256_after'] = sha256(original)
            if result['source_flash_sha256_after'] != before:
                result['status'] = 'FAIL'
                result['error'] = 'frozen image changed'
            result['files'] = {str(p.relative_to(destination)): sha256(p) for p in destination.rglob('*') if p.is_file()}
            save(destination / 'result.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--flash', required=True)
    parser.add_argument('--elf', required=True)
    parser.add_argument('--build-manifest', required=True)
    parser.add_argument('--source-manifest', required=True, help='authoritative complete SOURCE_READY checkout manifest')
    parser.add_argument('--evidence', type=Path, required=True, help='NEW run directory')
    parser.add_argument('--profile', choices=PROFILES, default='connected')
    parser.add_argument('--case', type=int, action='append', help='stable case id; default all68, never rebuild')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    require(os.name == 'posix', 'run on native Linux/WSL sockets')
    cases = json.loads((ROOT / 'cases-v1.json').read_bytes())['cases']
    ids = list(range(len(cases))) if args.case is None else args.case
    require(len(cases) == 68 and len(set(ids)) == len(ids) and all(0 <= i < 68 for i in ids), 'invalid case matrix')
    build = json.loads(Path(args.build_manifest).read_bytes())
    require(build.get('state') == 'built' and build.get('idf_commit') == 'fff9895c82d744c7237be8847347bdd1b07c6643',
            'ordinary pinned IDF build not ready')
    artifacts = {Path(a['path']).name: a['sha256'] for a in build['artifacts']}
    require(artifacts.get(Path(args.flash).name) == sha256(args.flash) and
            artifacts.get(Path(args.elf).name) == sha256(args.elf), 'frozen ELF/image differs from build manifest')
    require(build['case_corpus']['sha256'] == sha256(ROOT / 'cases-v1.json'), 'fixture case corpus differs')
    args.evidence = args.evidence.resolve()
    args.evidence.mkdir(parents=True, exist_ok=False)
    identity = {name: dict(path=str(Path(path).resolve()), sha256=sha256(path))
                for name, path in [('qemu', args.qemu), ('flash', args.flash), ('elf', args.elf),
                                   ('build_manifest', args.build_manifest), ('source_manifest', args.source_manifest)]}
    identity['host_sources'] = {p.name: sha256(p) for p in (ROOT / 'run_peer.py', ROOT / 'peer_reference.py', ROOT / 'cases-v1.json', ROOT / 'contract-v1.json', ROOT / 'runner-contract.json')}
    save(args.evidence / 'identity.json', identity)
    shutil.copyfile(args.build_manifest, args.evidence / 'build-manifest.json')
    shutil.copyfile(args.source_manifest, args.evidence / 'source-manifest.json')
    rows = [run_case(args, i, cases[i], args.evidence / f'{i:02d}-{cases[i]["name"]}') for i in ids]
    summary = dict(version=1, profile=args.profile, identity=identity,
                   rows=[{key: row.get(key) for key in ('id', 'case', 'profile', 'status', 'error', 'din_capture', 'transition_capture', 'physical_bidirectional_qualified')} for row in rows],
                   all_68_cases_qualified=len(rows) == 68 and all(row['physical_bidirectional_qualified'] and
                       row['status'] == 'PASS_ACTUAL_PEER_BIDIRECTIONAL' for row in rows),
                   hardware_qualified=False, converter_qualified=False,
                   attribution='actual FIFO LOAD origin and COMPLETE emission epochs; no phase/payload search')
    save(args.evidence / 'summary.json', summary)
    print(json.dumps(dict(evidence=str(args.evidence), failed=sum(r['status'] == 'FAIL' for r in rows), all_68_cases_qualified=summary['all_68_cases_qualified'])))
    if any(r['status'] == 'FAIL' for r in rows):
        return 1
    return 0 if summary['all_68_cases_qualified'] or args.profile != 'connected' else 2


if __name__ == '__main__':
    raise SystemExit(main())
