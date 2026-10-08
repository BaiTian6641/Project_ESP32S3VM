#!/usr/bin/env python3
"""Prepare an isolated, provenance-recorded LCD_CAM source checkout; never build it.

The frozen source record pins every prefix input and resulting prefix target.
The frozen checkout supplies Git objects only: its worktree and build products
are not overlay sources. Existing destination checkouts are never modified.
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
    return sorted({target.decode() for target in
                   re.findall(rb'^\+\+\+ b/(.+?)\r?$', data, re.MULTILINE)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path.home() / '.cache/esp32s3vm/qemu-lcd-cam-40edccac4156-dbe2a8a5ad4e07a6')
    parser.add_argument('--prepare-prefix-only', action='store_true')
    parser.add_argument('--prepare-clock-prefix-only', action='store_true',
                        help='Snapshot the frozen prefix plus pinned coreclk 0010..0013 only')
    parser.add_argument('--frozen-input-checkout', type=Path,
                        help='Use only hash-pinned immutable input snapshots, never its worktree')
    parser.add_argument('--frozen-input-pack', type=Path,
                        help='Exact immutable input pack from another prepared lane; hashes remain authoritative')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing LCD_CAM checkout: {source}')
    frozen_bytes = FROZEN.read_bytes()
    frozen = json.loads(frozen_bytes)
    if frozen['base_commit'] != BASE or frozen['fingerprint'] != PREFIX:
        raise SystemExit('Frozen LCD_CAM prefix record does not match the pinned base/fingerprint')
    operations = []
    archived_inputs = {}
    if args.frozen_input_checkout and args.frozen_input_pack:
        raise SystemExit('Select either frozen-input-checkout or frozen-input-pack')
    if args.frozen_input_checkout or args.frozen_input_pack:
        pack = (args.frozen_input_pack if args.frozen_input_pack else
                args.frozen_input_checkout / '.esp32s3vm-lcd-cam-inputs')
        archived_record = json.loads((pack / 'prepared-source.json').read_bytes())
        if (archived_record['base_commit'] != BASE or
                archived_record['prefix_fingerprint'] != PREFIX or
                archived_record['prefix_record_sha256'] != sha256(frozen_bytes)):
            raise SystemExit('Archived input pack does not match the frozen prefix')
        archived_inputs = {entry['source']: pack / Path(entry['snapshot']).name
                           for entry in archived_record['operations']}

    def add(kind, name, destination=None, expected=None, apply_options=()):
        data = (archived_inputs[name] if name in frozen['inputs'] and name in archived_inputs
                else EXT / name).read_bytes()
        if expected is not None and sha256(data) != expected:
            raise SystemExit(f'Pinned input hash mismatch: {name}')
        if kind == 'copy':
            data = data.replace(b'\r\n', b'\n')
        operations.append((kind, name, destination, data, apply_options))

    profile = frozen['profile']
    for entry in profile['copies']:
        add('copy', entry['source'], entry['destination'], frozen['inputs'][entry['source']])
    for entry in profile['patches']:
        add('patch', entry['source'], expected=frozen['inputs'][entry['source']])
    prefix_count = len(operations)
    mapping_bytes = None
    if not args.prepare_prefix_only:
        mapping_bytes = (LANE / 'source-map.json').read_bytes()
        mapping = json.loads(mapping_bytes)
        if mapping['base_commit'] != BASE or mapping['prefix_fingerprint'] != PREFIX:
            raise SystemExit('LCD_CAM source map does not match the pinned prefix')
        if not args.prepare_clock_prefix_only and mapping.get('status') != 'source-ready':
            raise SystemExit('LCD_CAM dependencies are not SOURCE_READY: ' +
                             '; '.join(mapping.get('blockers', [])))
        entries = (mapping['clock_prefix_operations'] if args.prepare_clock_prefix_only
                   else mapping['preparation_order'])
        for entry in entries:
            if entry['kind'] == 'patch':
                add('patch', entry['source'], expected=entry['sha256'],
                    apply_options=entry.get('apply_options', []))
            elif entry['kind'] == 'copy':
                add('copy', entry['source'], entry['destination'], entry['sha256'])
            else:
                raise SystemExit(f'Unknown preparation operation: {entry["kind"]}')
    # Read every authoritative input before mutating the new checkout. Save
    # the exact consumed bytes, not a reference to a mutable sibling checkout.
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--no-hardlinks',
                    '--no-checkout', frozen['source'], str(source)], check=True)
    subprocess.run(['git', '-C', str(source), '-c', 'core.autocrlf=false',
                    'checkout', '--detach', BASE], check=True)
    inputs = source / '.esp32s3vm-lcd-cam-inputs'
    inputs.mkdir()
    (inputs / 'frozen-source-record.json').write_bytes(frozen_bytes)
    if mapping_bytes is not None:
        (inputs / 'source-map.json').write_bytes(mapping_bytes)
    records = []
    targets = set()
    for index, (kind, name, destination, data, apply_options) in enumerate(operations):
        snapshot = inputs / f'{index:03d}-{Path(name).name}'
        snapshot.write_bytes(data)
        snapshot.chmod(0o444)
        if kind == 'patch':
            affected = patch_targets(data)
            subprocess.run(['git', '-C', str(source), 'apply', *apply_options, '-'],
                           input=data, check=True)
        else:
            affected = [destination]
            target = source / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        targets.update(affected)
        records.append({'order': index, 'kind': kind, 'source': name,
                        'snapshot': str(snapshot.relative_to(source)),
                        'sha256': sha256(data), 'targets': affected,
                        'apply_options': list(apply_options)})
        if index + 1 == prefix_count:
            for target, expected in frozen['applied_files'].items():
                if sha256((source / target).read_bytes()) != expected:
                    raise SystemExit(f'Prepared frozen prefix target mismatch: {target}')
    prepared = {'schema_version': 1, 'base_commit': BASE, 'prefix_fingerprint': PREFIX,
                'prefix_record_sha256': sha256(frozen_bytes), 'checkout': str(source),
                'status': 'source-prepared-unqualified', 'prefix_only': args.prepare_prefix_only,
                'clock_prefix_only': args.prepare_clock_prefix_only,
                'operations': records,
                'source_map_sha256': sha256(mapping_bytes) if mapping_bytes is not None else None,
                'targets': {target: sha256((source / target).read_bytes())
                            for target in sorted(targets)},
                'qualification': 'No build, test, runtime, firmware or hardware qualification.'}
    evidence = ROOT / 'build-runtime-state/lcd-cam-native-2026-10-07' / source.name
    evidence.mkdir(parents=True, exist_ok=False)
    record_bytes = (json.dumps(prepared, indent=2) + '\n').encode()
    (evidence / 'prepared-source.json').write_bytes(record_bytes)
    (inputs / 'prepared-source.json').write_bytes(record_bytes)
    for target in targets:
        (source / target).chmod(0o444)
    for snapshot in inputs.iterdir():
        snapshot.chmod(0o444)
    print(record_bytes.decode(), end='')


if __name__ == '__main__':
    main()
