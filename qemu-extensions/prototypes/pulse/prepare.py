#!/usr/bin/env python3
"""Prepare a new hash-pinned pulse checkout; never build or alter an existing one."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
LANE = Path(__file__).resolve().parent
EXT = ROOT / 'qemu-extensions'
BASE = '40edccac415693c5130f91c01d84176ae6008566'
PREFIX = '486cb408a31afae28b1a93896efd0a2909de13300f6fc050c696f10269fa2a92'
FROZEN = ROOT / 'build-runtime-state/runtime-486cb408a31afae2-source.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path.home() / '.cache/esp32s3vm/qemu-pulse-40edccac4156-486cb408a31afae2')
    parser.add_argument('--prepare-prefix-only', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing pulse checkout: {source}')
    frozen_bytes = FROZEN.read_bytes()
    frozen = json.loads(frozen_bytes)
    mapping_bytes = (LANE / 'source-map.json').read_bytes()
    mapping = json.loads(mapping_bytes)
    if (frozen['base_commit'] != BASE or frozen['fingerprint'] != PREFIX or
            mapping['base_commit'] != BASE or mapping['prefix_fingerprint'] != PREFIX or
            digest(frozen_bytes) != mapping['foundation_prefix']['record_sha256']):
        raise SystemExit('Pulse source records do not match the pinned official prefix')
    if not args.prepare_prefix_only and mapping['status'] != 'source-ready':
        raise SystemExit('Pulse dependencies are not SOURCE_READY: ' + '; '.join(mapping['blockers']))
    operations = []

    def add(kind, name, expected, destination=None, options=()):
        data = (EXT / name).read_bytes()
        if digest(data) != expected:
            raise SystemExit(f'Pinned input hash mismatch: {name}')
        if kind == 'copy':
            data = data.replace(b'\r\n', b'\n')
        operations.append((kind, name, destination, data, list(options)))

    # Canonical frozen profile semantics: patches create/update files first;
    # final copies then install the hash-pinned complete implementations.
    for entry in frozen['profile']['patches']:
        add('patch', entry['source'], frozen['inputs'][entry['source']])
    for entry in frozen['profile']['copies']:
        add('copy', entry['source'], frozen['inputs'][entry['source']], entry['destination'])
    prefix_count = len(operations)
    entries = () if args.prepare_prefix_only else (
        mapping['preparation_order'] + mapping['pulse_operations'])
    for entry in entries:
        add(entry['kind'], entry['source'], entry['sha256'],
            entry.get('destination'), entry.get('apply_options', ()))
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--no-hardlinks',
                    '--no-checkout', frozen['source'], str(source)], check=True)
    subprocess.run(['git', '-C', str(source), '-c', 'core.autocrlf=false',
                    'checkout', '--detach', BASE], check=True)
    inputs = source / '.esp32s3vm-pulse-inputs'
    inputs.mkdir()
    (inputs / 'frozen-source-record.json').write_bytes(frozen_bytes)
    (inputs / 'source-map.json').write_bytes(mapping_bytes)
    records = []
    targets = set()
    for index, (kind, name, destination, data, options) in enumerate(operations):
        snapshot = inputs / f'{index:03d}-{Path(name).name}'
        snapshot.write_bytes(data)
        if kind == 'patch':
            affected = sorted({p.decode() for p in re.findall(rb'^\+\+\+ b/(.+?)\r?$', data, re.MULTILINE)})
            subprocess.run(['git', '-C', str(source), 'apply', *options, '-'], input=data, check=True)
        else:
            affected = [destination]
            target = source / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        targets.update(affected)
        records.append({'order': index, 'kind': kind, 'source': name,
                        'snapshot': str(snapshot.relative_to(source)), 'sha256': digest(data),
                        'targets': affected, 'apply_options': options})
        if index + 1 == prefix_count:
            for target, expected in frozen['applied_files'].items():
                if digest((source / target).read_bytes()) != expected:
                    raise SystemExit(f'Prepared frozen-prefix target mismatch: {target}')
    prepared = {'schema_version': 1, 'base_commit': BASE, 'prefix_fingerprint': PREFIX,
                'prefix_record_sha256': digest(frozen_bytes), 'checkout': str(source),
                'prefix_only': args.prepare_prefix_only,
                'status': 'source-prepared-unqualified', 'operations': records,
                'source_map_sha256': digest(mapping_bytes),
                'targets': {name: digest((source / name).read_bytes()) for name in sorted(targets)},
                'qualification': 'Preparation is not build, runtime, firmware or hardware proof.'}
    record = (json.dumps(prepared, indent=2) + '\n').encode()
    evidence = ROOT / 'build-runtime-state/pulse-native-2026-10-07' / source.name
    evidence.mkdir(parents=True, exist_ok=False)
    (evidence / 'prepared-source.json').write_bytes(record)
    (inputs / 'prepared-source.json').write_bytes(record)
    for item in inputs.iterdir():
        item.chmod(0o444)
    print(record.decode(), end='')


if __name__ == '__main__':
    main()
