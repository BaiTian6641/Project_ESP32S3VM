#!/usr/bin/env python3
"""Prepare an isolated, provenance-recorded I2S source checkout; never build it.

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
                        default=Path.home() / '.cache/esp32s3vm/qemu-i2s-40edccac4156-dbe2a8a5ad4e07a6')
    parser.add_argument('--prepare-prefix-only', action='store_true')
    parser.add_argument('--prepare-clock-prefix-only', action='store_true',
                        help='Snapshot the frozen prefix plus pinned coreclk 0010..0013 only')
    parser.add_argument('--prepare-foundations-only', action='store_true',
                        help='Prepare owner-frozen native foundations without I2S copies; never build')
    parser.add_argument('--frozen-input-checkout', type=Path,
                        help='Use only hash-pinned immutable input snapshots, never its worktree')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing I2S checkout: {source}')
    frozen_bytes = FROZEN.read_bytes()
    frozen = json.loads(frozen_bytes)
    if frozen['base_commit'] != BASE or frozen['fingerprint'] != PREFIX:
        raise SystemExit('Frozen I2S prefix record does not match the pinned base/fingerprint')
    operations = []
    archived_inputs = {}
    archived_overlay_sources = set()
    archived_record_bytes = None
    if args.frozen_input_checkout:
        pack = args.frozen_input_checkout / '.esp32s3vm-i2s-inputs'
        archived_record_bytes = (pack / 'prepared-source.json').read_bytes()
        archived_record = json.loads(archived_record_bytes)
        if (archived_record['base_commit'] != BASE or
                archived_record['prefix_fingerprint'] != PREFIX or
                archived_record['prefix_record_sha256'] != sha256(frozen_bytes)):
            raise SystemExit('Archived input pack does not match the frozen prefix')
        archived_inputs = {entry['source']: pack / Path(entry['snapshot']).name
                           for entry in archived_record['operations']}

    def add(kind, name, destination=None, expected=None, apply_options=(),
            expected_targets=None):
        data = (archived_inputs[name]
                if name in archived_inputs and
                   (name in frozen['inputs'] or name in archived_overlay_sources)
                else EXT / name).read_bytes()
        if expected is not None and sha256(data) != expected:
            raise SystemExit(f'Pinned input hash mismatch: {name}')
        if kind == 'patch' and expected_targets is not None:
            numstat = subprocess.run(
                ['git', '-C', frozen['source'], 'apply', '--numstat', '-'],
                input=data, check=True, stdout=subprocess.PIPE).stdout
            actual_targets = sorted({line.split(b'\t', 2)[2].decode()
                                     for line in numstat.splitlines()})
            if actual_targets != sorted(expected_targets):
                raise SystemExit(f'Pinned patch target scope mismatch: {name}')
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
            raise SystemExit('I2S source map does not match the pinned prefix')
        archived_overlay_sources = set(mapping.get('archived_overlay_sources', []))
        if archived_overlay_sources:
            if (archived_record_bytes is None or
                    sha256(archived_record_bytes) != mapping['archive_receipt_sha256']):
                raise SystemExit('Historical overlay archive receipt does not match')
            if not archived_overlay_sources.issubset(archived_inputs):
                raise SystemExit('Historical overlay input snapshots are incomplete')
        if args.prepare_foundations_only:
            if mapping.get('foundations_status') != 'source-ready':
                raise SystemExit('I2S native foundations are not SOURCE_READY: ' +
                                 '; '.join(mapping.get('foundation_blockers', [])))
            entries = mapping['foundation_operations']
        elif args.prepare_clock_prefix_only:
            entries = mapping['clock_prefix_operations']
        else:
            if mapping.get('status') != 'source-ready':
                raise SystemExit('I2S dependencies are not SOURCE_READY: ' +
                                 '; '.join(mapping.get('blockers', [])))
            entries = mapping['preparation_order']
        for entry in entries:
            if entry['kind'] == 'patch':
                add('patch', entry['source'], expected=entry['sha256'],
                    apply_options=entry.get('apply_options', []),
                    expected_targets=entry['targets'])
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
    inputs = source / '.esp32s3vm-i2s-inputs'
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
    target_hashes = {target: sha256((source / target).read_bytes())
                     for target in sorted(targets)}
    historical_equality = None
    if archived_overlay_sources:
        changed = sorted(target for target in target_hashes
                         if target_hashes[target] != archived_record['targets'].get(target))
        if (set(target_hashes) != set(archived_record['targets']) or
                changed != sorted(mapping['historical_allowed_changed_targets'])):
            raise SystemExit('Historical pack target equality differs beyond the approved source delta')
        historical_equality = {
            'archive_receipt_sha256': sha256(archived_record_bytes),
            'unchanged_targets': len(target_hashes) - len(changed), 'changed_targets': changed,
            'scope': 'I2S-specific historical pack; not current parent foundation qualification'}
    prepared = {'schema_version': 1, 'base_commit': BASE, 'prefix_fingerprint': PREFIX,
                'prefix_record_sha256': sha256(frozen_bytes), 'checkout': str(source),
                'status': 'source-prepared-unqualified', 'prefix_only': args.prepare_prefix_only,
                'clock_prefix_only': args.prepare_clock_prefix_only,
                'foundations_only': args.prepare_foundations_only,
                'verification_scope': mapping.get('verification_scope') if mapping_bytes else None,
                'operations': records,
                'source_map_sha256': sha256(mapping_bytes) if mapping_bytes is not None else None,
                'targets': target_hashes, 'historical_pack_equality': historical_equality,
                'qualification': 'No build, test, runtime, firmware or hardware qualification.'}
    evidence = ROOT / 'build-runtime-state/i2s-native-2026-10-07' / source.name
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
