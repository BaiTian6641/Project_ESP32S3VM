#!/usr/bin/env python3
"""Prepare a new UART checkout from locked, hash-recorded additive inputs.

Run under WSL. --dependencies is the integration owner's frozen common source
record/profile, excluding UART controller inputs (shared UART/GDMA API allowed).
Existing checkouts are never edited.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

ROOT = Path(__file__).resolve().parents[3]
EXT = ROOT / 'qemu-extensions'
LANE = Path(__file__).resolve().parent
BASE = '40edccac415693c5130f91c01d84176ae6008566'
PREFIX = ROOT / 'build-runtime-state/runtime-dbe2a8a5ad4e07a6-source.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def relative(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or '..' in path.parts or '\\' in value or ':' in value:
        raise ValueError(f'Unsafe relative input: {value}')
    return path.as_posix()


def git(source, *args, data=None):
    result = subprocess.run(['git', '-C', str(source), *args], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependencies', type=Path, required=True,
                        help='Frozen common profile/source record with every input SHA256')
    parser.add_argument('--cache-root', type=Path,
                        default=Path.home() / '.cache/esp32s3vm')
    parser.add_argument('--evidence', type=Path,
                        default=ROOT / 'build-runtime-state/uart-native-2026-10-07')
    args = parser.parse_args()
    frozen = json.loads(PREFIX.read_text())
    dependency_record = json.loads(args.dependencies.read_text())
    common = dependency_record.get('profile', dependency_record)
    if frozen['base_commit'] != BASE or common['base_commit'] != BASE:
        raise ValueError('Both profiles must use the locked official QEMU base')
    if frozen['fingerprint'] != 'dbe2a8a5ad4e07a607e76ab751e77ac85fe3bf30974359bd25a69354499f9b32':
        raise ValueError('Wrong frozen26 prerequisite record')
    mapping = json.loads((LANE / 'source-map.json').read_text())
    inputs, patches, copies = {}, [], []
    contexts, scopes = {}, {}
    prefix_names = set()

    def load(name, expected, normalize=False):
        name = relative(name)
        path = (EXT / name).resolve(strict=True)
        if not path.is_relative_to(EXT.resolve()):
            raise ValueError(f'Input escapes extensions: {name}')
        data = path.read_bytes()
        if normalize:
            data = data.replace(b'\r\n', b'\n')
        actual = digest(data)
        if not expected or expected != actual:
            raise ValueError(f'Unfrozen or changed input {name}: expected {expected}, actual {actual}')
        if name in inputs and inputs[name] != actual:
            raise ValueError(f'Conflicting identities: {name}')
        inputs[name] = actual
        return name, data

    for item in frozen['profile']['patches']:
        name, data = load(item['source'], frozen['inputs'].get(item['source']))
        prefix_names.add(name)
        contexts[name] = 'strict'
        scopes[name] = {relative(p) for p in item['targets']}
        patches.append((name, data))
    for item in frozen['profile']['copies']:
        name, data = load(item['source'], frozen['inputs'].get(item['source']), True)
        prefix_names.add(name)
        copies.append((name, relative(item['destination']), data))
    common_hashes = dependency_record.get('inputs', {})
    shared_uart_api = 'prototypes/uart/' + mapping['gdma_additive_patch']
    for item in common.get('patches', []):
        name = relative(item['source'])
        if name in prefix_names:
            if common_hashes.get(name, item.get('sha256')) != inputs[name]:
                raise ValueError(f'Common record changes frozen26 input: {name}')
            if item.get('patch_context', 'strict') != 'strict':
                raise ValueError(f'Common record changes frozen26 context policy: {name}')
            continue
        if name.startswith('prototypes/uart/') and name != shared_uart_api:
            raise ValueError('Common dependency profile must exclude UART controller inputs')
        name, data = load(name, common_hashes.get(name, item.get('sha256')))
        policy = item.get('patch_context', 'strict')
        if name == shared_uart_api:
            # The lane's own reviewed policy for this shared artifact governs its
            # matching operation even when the dependency record supplies it; the
            # common record cannot silently re-tighten it to a strict apply.
            policy = mapping.get('patch_context', {}).get(name[len('prototypes/uart/'):],
                                                          policy)
        if policy not in {'strict', 'ignore-space-change'}:
            raise ValueError(f'Unsupported explicit patch context policy: {policy}')
        contexts[name] = policy
        scopes[name] = {relative(p) for p in item['targets']}
        patches.append((name, data))
    for item in common.get('copies', []):
        name = relative(item['source'])
        if name in prefix_names:
            continue
        if name.startswith('prototypes/uart/'):
            raise ValueError('Common dependency profile must exclude UART-owned inputs')
        name, data = load(name, common_hashes.get(name, item.get('sha256')), True)
        copies.append((name, relative(item['destination']), data))

    def owned_patch(name):
        full = 'prototypes/uart/' + name
        data = (LANE / name).read_bytes()
        inputs[full] = digest(data)
        contexts[full] = mapping.get('patch_context', {}).get(name, 'strict')
        scopes[full] = {relative(p) for p in mapping['patch_targets'][name]}
        return full, data

    # Common profile includes SPI packet-end and GDMA owner0004 before this API.
    if shared_uart_api not in inputs:
        patches.append(owned_patch(mapping['gdma_additive_patch']))
    if inputs[shared_uart_api] != mapping['gdma_additive_sha256']:
        raise ValueError('Shared UART/GDMA API differs from the independently reviewed frozen identity')
    patches.append(owned_patch('0002-parent-uart-defer-fifo.patch'))
    for item in mapping['copies']:
        name = 'prototypes/uart/' + relative(item['source'])
        data = (EXT / name).read_bytes().replace(b'\r\n', b'\n')
        inputs[name] = digest(data)
        copies.append((name, relative(item['destination']), data))
    tail = [owned_patch(mapping['integration_patch']),
            owned_patch('integration-electrical.patch'),
            owned_patch('integration-qtest.patch'),
            owned_patch(mapping['unit_integration_patch'])]
    identity = {'base_commit': BASE, 'frozen_prefix': frozen['fingerprint'],
                'dependency_record_sha256': digest(args.dependencies.read_bytes()),
                'inputs': inputs, 'copies': [(n, d) for n, d, _ in copies],
                'patches': [{'source': n, 'patch_context': contexts[n],
                             'targets': sorted(scopes[n])} for n, _ in patches + tail]}
    fingerprint = digest(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode())
    source = (args.cache_root / f'qemu-uart-{BASE[:12]}-{fingerprint[:16]}').resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing UART checkout: {source}')
    baseline = ROOT / 'build-qemu-official-base'
    if git(baseline, 'rev-parse', 'HEAD').decode().strip() != BASE:
        raise ValueError('Official base revision is not the lock; leaving it untouched')
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--no-hardlinks',
                    '--no-checkout', str(baseline), str(source)], check=True)
    git(source, '-c', 'core.autocrlf=false', 'checkout', '--detach', BASE)

    def apply(name, data):
        stats = git(source, 'apply', '--numstat', '-', data=data).decode()
        actual = {relative(line.split('\t', 2)[2]) for line in stats.splitlines()}
        if actual != scopes[name]:
            raise ValueError(f'Patch scope differs from recorded targets: {name}')
        options = ['--ignore-space-change'] if contexts[name] == 'ignore-space-change' else []
        print('Applying', name, contexts[name], flush=True)
        git(source, 'apply', *options, '-', data=data)

    for name, data in patches:
        apply(name, data)
    for _, destination, data in copies:
        path = source / destination
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for name, data in tail:
        apply(name, data)
    applied = {p for _, p, _ in copies}
    for _, data in patches + tail:
        for line in data.decode('utf-8').splitlines():
            if line.startswith('+++ b/'):
                applied.add(relative(line[6:]))
    record = {**identity, 'fingerprint': fingerprint, 'source': str(source),
              'qualification': 'not built or exercised by preparation',
              'applied_files': {p: digest((source / p).read_bytes())
                                if (source / p).is_file() else None
                                for p in sorted(applied)}}
    args.evidence.mkdir(parents=True, exist_ok=True)
    (args.evidence / f'prepared-{fingerprint[:16]}-source.json').write_text(
        json.dumps(record, indent=2) + '\n')
    (source / 'uart-source.json').write_text(json.dumps(record, indent=2) + '\n')
    print(source)


if __name__ == '__main__':
    main()
