#!/usr/bin/env python3
"""Prepare an immutable memory qualification candidate; never build or qualify it.

Frozen prefix inputs come from a recorded immutable snapshot pack, not from a
sibling worktree. New candidates preserve all previous sources and failed runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
EXT = ROOT / 'qemu-extensions'
LANE = Path(__file__).resolve().parent
BASE = '40edccac415693c5130f91c01d84176ae6008566'
PREFIX = 'dbe2a8a5ad4e07a607e76ab751e77ac85fe3bf30974359bd25a69354499f9b32'
FROZEN = ROOT / 'build-runtime-state/runtime-dbe2a8a5ad4e07a6-source.json'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patch_targets(data):
    return sorted({p.decode() for p in
                   re.findall(rb'^\+\+\+ b/(.+?)\r?$', data, re.MULTILINE)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--prefix-input-pack', type=Path,
                        default=Path.home() / '.cache/esp32s3vm/'
                        'qemu-pulse-clockprefix-40edccac4156-dbe2a8a5ad4e07a6/'
                        '.esp32s3vm-pulse-inputs')
    parser.add_argument('--negative-without-spi1-fix', action='store_true',
                        help='Prepare a separately recorded transport regression control')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing memory checkout: {source}')
    frozen_bytes = FROZEN.read_bytes()
    frozen = json.loads(frozen_bytes)
    mapping_bytes = (LANE / 'source-map.json').read_bytes()
    mapping = json.loads(mapping_bytes)
    pack_record = json.loads((args.prefix_input_pack / 'prepared-source.json').read_bytes())
    if (frozen['base_commit'] != BASE or frozen['fingerprint'] != PREFIX or
            pack_record['base_commit'] != BASE or
            pack_record['prefix_fingerprint'] != PREFIX or
            pack_record['prefix_record_sha256'] != sha256(frozen_bytes)):
        raise SystemExit('Memory frozen base/prefix input record mismatch')
    if (mapping['base_commit'] != BASE or mapping['prefix_fingerprint'] != PREFIX or
            mapping['status'] != 'source-ready'):
        raise SystemExit('Memory overlay is not SOURCE_READY')
    archived = {entry['source']: entry for entry in pack_record['operations']}
    operations = []

    def add(kind, name, destination=None, expected=None, archived_only=False):
        if name in archived:
            entry = archived[name]
            data = (args.prefix_input_pack / Path(entry['snapshot']).name).read_bytes()
            if sha256(data) != entry['sha256']:
                raise SystemExit(f'Archived input hash mismatch: {name}')
        elif archived_only:
            raise SystemExit(f'Missing frozen input snapshot: {name}')
        else:
            data = (EXT / name).read_bytes()
        if expected is not None and sha256(data) != expected:
            raise SystemExit(f'Pinned input hash mismatch: {name}')
        if kind == 'copy':
            data = data.replace(b'\r\n', b'\n')
        operations.append((kind, name, destination, data))

    for entry in frozen['profile']['copies']:
        add('copy', entry['source'], entry['destination'],
            frozen['inputs'][entry['source']], archived_only=True)
    for entry in frozen['profile']['patches']:
        add('patch', entry['source'], expected=frozen['inputs'][entry['source']],
            archived_only=True)
    prefix_count = len(operations)
    for entry in mapping['preparation_order']:
        if args.negative_without_spi1_fix and entry['source'] == \
                'prototypes/memory/spi1-transport.patch':
            continue
        add(entry['kind'], entry['source'], entry.get('destination'), entry['sha256'])

    subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--no-hardlinks',
                    '--no-checkout', frozen['source'], str(source)], check=True)
    subprocess.run(['git', '-C', str(source), '-c', 'core.autocrlf=false',
                    'checkout', '--detach', BASE], check=True)
    inputs = source / '.esp32s3vm-memory-inputs'
    inputs.mkdir()
    (inputs / 'frozen-source-record.json').write_bytes(frozen_bytes)
    (inputs / 'source-map.json').write_bytes(mapping_bytes)
    records = []
    targets = set()
    for index, (kind, name, destination, data) in enumerate(operations):
        snapshot = inputs / f'{index:03d}-{Path(name).name}'
        snapshot.write_bytes(data)
        if kind == 'patch':
            affected = patch_targets(data)
            subprocess.run(['git', '-C', str(source), 'apply', '-'],
                           input=data, check=True)
        elif kind == 'copy':
            affected = [destination]
            target = source / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        else:
            raise SystemExit(f'Unknown operation: {kind}')
        targets.update(affected)
        records.append({'order': index, 'kind': kind, 'source': name,
                        'snapshot': str(snapshot.relative_to(source)),
                        'sha256': sha256(data), 'targets': affected})
        if index + 1 == prefix_count:
            for target, expected in frozen['applied_files'].items():
                if sha256((source / target).read_bytes()) != expected:
                    raise SystemExit(f'Frozen prefix target mismatch: {target}')
    record = {'schema_version': 1, 'base_commit': BASE, 'prefix_fingerprint': PREFIX,
              'prefix_record_sha256': sha256(frozen_bytes), 'checkout': str(source),
              'source_map_sha256': sha256(mapping_bytes), 'operations': records,
              'negative_without_spi1_fix': args.negative_without_spi1_fix,
              'status': 'source-prepared-unqualified',
              'targets': {target: sha256((source / target).read_bytes())
                          for target in sorted(targets)},
              'qualification': 'No build, test, firmware, runtime or hardware claim.'}
    record['source_fingerprint'] = sha256(json.dumps(record['targets'], sort_keys=True).encode())
    evidence = ROOT / 'build-runtime-state/memory-native-2026-10-07' / source.name
    evidence.mkdir(parents=True, exist_ok=False)
    encoded = (json.dumps(record, indent=2) + '\n').encode()
    (evidence / 'prepared-source.json').write_bytes(encoded)
    (inputs / 'prepared-source.json').write_bytes(encoded)
    for target in targets:
        (source / target).chmod(0o444)
    for snapshot in inputs.iterdir():
        snapshot.chmod(0o444)
    print(encoded.decode(), end='')


if __name__ == '__main__':
    main()
