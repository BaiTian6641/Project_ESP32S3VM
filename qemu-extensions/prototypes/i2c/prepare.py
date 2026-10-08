#!/usr/bin/env python3
"""Build an isolated initial-I2C source tree from the pinned combined-v3 prefix.

Original upstream/lane checkouts and the aggregate runtime profile are read-only.
An existing destination is never reconciled or overwritten.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
LANE = Path(__file__).resolve().parent
BASE = '40edccac415693c5130f91c01d84176ae6008566'
FROZEN_RECORD = ROOT / 'build-runtime-state/runtime-105a156e5dec5fc6-source.json'
OVERLAPS = {'hw/misc/meson.build', 'hw/xtensa/esp32s3.c', 'tests/qtest/meson.build'}


def filtered_patch(data):
    # These three original-base hunks are replaced by the reviewed combined-v3
    # restack. Every other hunk is preserved byte-for-byte, including CRLF.
    blocks = data.split(b'diff --git ')
    kept = []
    for block in blocks[1:]:
        destination = block.split(b'\n', 1)[0].split(b' b/', 1)[1].decode()
        if destination not in OVERLAPS:
            kept.append(b'diff --git ' + block)
    return b''.join(kept)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path.home() / '.cache/esp32s3vm/qemu-i2c-40edccac4156')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing I2C checkout: {source}')
    ext = ROOT / 'qemu-extensions'
    frozen = json.loads(FROZEN_RECORD.read_text(encoding='utf-8'))
    profile = frozen['profile']
    assert profile['base_commit'] == BASE
    mapping = json.loads((LANE / 'source-map.json').read_text(encoding='utf-8'))
    patches = [(p['source'], (ext / p['source']).read_bytes()) for p in profile['patches']]
    copies = [(p['source'], p['destination'], (ext / p['source']).read_bytes())
              for p in profile['copies']]
    for lane in ['gpio', 'adc']:
        prefix = 'prototypes/' + lane + '/'
        dependency = json.loads((ext / prefix / 'source-map.json').read_text(encoding='utf-8'))
        name = prefix + dependency['integration_patch']
        patches.append((name + ' (non-overlapping hunks)', filtered_patch((ext / name).read_bytes())))
        copies.extend((prefix + p['source'], p['destination'],
                       (ext / prefix / p['source']).read_bytes()) for p in dependency['copies'])
    patches.append(('prototypes/i2c/dependencies-restack.patch',
                    (LANE / 'dependencies-restack.patch').read_bytes()))
    patches.extend((name, (ext / name).read_bytes())
                   for name in mapping['clock_prerequisite_patches'])
    copies.extend((p['source'], p['destination'], (ext / p['source']).read_bytes())
                  for p in mapping['dependency_copies'])
    copies.extend(('prototypes/i2c/' + p['source'], p['destination'],
                   (LANE / p['source']).read_bytes()) for p in mapping['copies'])
    subprocess.run(['git', '-c', 'core.autocrlf=false', 'clone', '--no-hardlinks',
                    '--no-checkout', str(ROOT / 'build-qemu-official-base'), str(source)], check=True)

    def git(*commands, data=None):
        return subprocess.run(['git', '-C', str(source), *commands], input=data,
                              check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE).stdout

    git('-c', 'core.autocrlf=false', 'checkout', '--detach', BASE)
    for name, data in patches:
        print('Applying', name, flush=True)
        git('apply', '-', data=data)
    for name, destination, data in copies:
        path = source / destination
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.replace(b'\r\n', b'\n'))
    integration = (LANE / mapping['integration_patch']).read_bytes()
    git('apply', '-', data=integration)
    record = {'base_commit': BASE, 'combined_v3_fingerprint': frozen['fingerprint'],
              'source_checkout': str(source),
              'patches': [{'source': n, 'sha256': hashlib.sha256(d).hexdigest()}
                          for n, d in patches] + [{'source': 'prototypes/i2c/integration.patch',
                                                   'sha256': hashlib.sha256(integration).hexdigest()}],
              'copies': [{'source': n, 'destination': dst,
                          'sha256': hashlib.sha256(d.replace(b'\r\n', b'\n')).hexdigest()}
                         for n, dst, d in copies]}
    evidence = ROOT / 'build-runtime-state/i2c-native-2026-10-07'
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / 'prepared-source.json').write_text(
        json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(source)


if __name__ == '__main__':
    main()
