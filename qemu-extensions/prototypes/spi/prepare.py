#!/usr/bin/env python3
"""Prepare the additive SPI lane without modifying any existing checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
LANE = Path(__file__).resolve().parent
BASE = '40edccac415693c5130f91c01d84176ae6008566'
FROZEN = ROOT / 'build-runtime-state/runtime-dbe2a8a5ad4e07a6-source.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path.home() / '.cache/esp32s3vm/qemu-spi-40edccac4156')
    parser.add_argument('--prepare-prefix-only', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()
    if source.exists():
        raise SystemExit(f'Preserving existing SPI checkout: {source}')
    record = json.loads(FROZEN.read_text())
    frozen = Path(record['source'])
    subprocess.run(['git', 'clone', '--no-hardlinks', '--no-checkout', str(frozen), str(source)], check=True)
    subprocess.run(['git', '-C', str(source), '-c', 'core.autocrlf=false', 'checkout', '--detach', BASE], check=True)
    delta = subprocess.run(['git', '-C', str(frozen), 'diff', '--binary', 'HEAD'], check=True, stdout=subprocess.PIPE).stdout
    subprocess.run(['git', '-C', str(source), 'apply', '-'], input=delta, check=True)
    # New files introduced by git-apply are untracked in this frozen prefix.
    # Transfer every declared target, not just the explicit copy list.
    targets = {e['destination'] for e in record['profile']['copies']}
    targets.update(t for p in record['profile']['patches'] for t in p['targets'])
    for target in sorted(targets):
        destination = source / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(frozen / target, destination)
    evidence = ROOT / 'build-runtime-state/spi-native-2026-10-07'
    evidence.mkdir(parents=True, exist_ok=True)
    prepared = {'base_commit': BASE, 'prefix_fingerprint': record['fingerprint'],
                'prefix_delta_sha256': hashlib.sha256(delta).hexdigest(), 'checkout': str(source)}
    if not args.prepare_prefix_only:
        mapping = json.loads((LANE / 'source-map.json').read_text())
        for entry in mapping['dependency_patches']:
            data = (ROOT / 'qemu-extensions' / entry['source']).read_bytes()
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise SystemExit(f"Preserving checkout: dependency changed: {entry['source']}")
            subprocess.run(['git', '-C', str(source), 'apply', '-'], input=data, check=True)
        for entry in mapping['copies']:
            destination = source / entry['destination']
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((LANE / entry['source']).read_bytes().replace(b'\r\n', b'\n'))
        for name in mapping['patches']:
            subprocess.run(['git', '-C', str(source), 'apply', '-'], input=(LANE / name).read_bytes(), check=True)
        for entry in mapping['after_patches']:
            data = (ROOT / 'qemu-extensions' / entry['source']).read_bytes()
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise SystemExit(f"Preserving checkout: dependency changed: {entry['source']}")
            subprocess.run(['git', '-C', str(source), 'apply', '-'], input=data, check=True)
        prepared['overlay'] = mapping
    (evidence / 'prepared-source.json').write_text(json.dumps(prepared, indent=2) + '\n')
    print(source)


if __name__ == '__main__':
    main()
