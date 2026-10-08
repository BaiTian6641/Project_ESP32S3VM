#!/usr/bin/env python3
"""Build immutable normal-IDF SPI inputs with the canonical native SDK.

The SDK owner prepares the complete source/submodules/tools. This lane copies
only authored firmware inputs to ext4 and records actual build/merge provenance.
No QEMU peripheral qualification is inferred from successful compilation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
LANE = Path(__file__).resolve().parent
COMMIT = 'fff9895c82d744c7237be8847347bdd1b07c6643'
MODES = ('connected', 'loopback', 'wrong_cs', 'disconnected_miso', 'owner_error')
NATIVE_FS = {'ext2/ext3', 'ext4', 'btrfs', 'xfs', 'overlayfs', 'tmpfs'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=MODES, required=True)
    parser.add_argument('--negative-host', type=int, choices=(2, 3))
    parser.add_argument('--activation', type=Path, default=Path.home() / '.cache/esp32s3vm/idf' /
                        ('idf-6.1-' + COMMIT) / 'activate.sh')
    parser.add_argument('--evidence', type=Path, default=ROOT / 'build-runtime-state/spi-native-2026-10-07')
    args = parser.parse_args()
    if args.negative_host is None:
        args.negative_host = 3 if args.mode == 'owner_error' else 2;
    if args.mode == 'owner_error' and args.negative_host != 3:
        parser.error('owner_error uses the fixture\'s actual SPI3 controller')
    if args.mode not in ('wrong_cs', 'disconnected_miso', 'owner_error') and args.negative_host != 2:
        parser.error('--negative-host applies only to negative profiles')
    args.evidence.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%dT%H%M%S') + '-' + str(os.getpid())
    evidence = args.evidence / ('native-build-' + args.mode + '-' + stamp)
    evidence.mkdir()
    activation = args.activation.resolve(strict=True)
    activation_command = ['bash', '-c', 'set +eu; source "$1"; rc=$?; test "$rc" = 0 || exit "$rc"; '
                          'python3 -c "import json,os; print(json.dumps(dict(os.environ)))"',
                          '--', str(activation)]
    env_result = subprocess.run(activation_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, check=True)
    (evidence / 'activation.log').write_text(env_result.stdout)
    environment = json.loads(env_result.stdout.splitlines()[-1])
    idf = Path(environment['IDF_PATH']).resolve(strict=True)
    build_root = Path(environment['ESP32S3_IDF_BUILD_ROOT']).resolve(strict=True)
    actual_commit = subprocess.check_output(['git', '-C', str(idf), 'rev-parse', 'HEAD'], text=True).strip()
    if actual_commit != COMMIT:
        raise SystemExit('Canonical activation does not provide the pinned IDF6.1 commit')
    for path in (idf, build_root, Path(environment['IDF_TOOLS_PATH']),
                 Path(environment['IDF_PYTHON_ENV_PATH'])):
        filesystem = subprocess.check_output(['stat', '-f', '-c', '%T', str(path)], text=True).strip()
        if filesystem not in NATIVE_FS:
            raise SystemExit('SDK, tools, environment and output must be native Linux: ' + str(path))
    freeze = json.loads((LANE / 'firmware-source-freeze.json').read_text())
    if freeze['idf_commit'] != COMMIT:
        raise SystemExit('Firmware source freeze has a different SDK identity')
    identity = hashlib.sha256(json.dumps(freeze['sources'], sort_keys=True).encode()).hexdigest()
    if identity != freeze['identity']:
        raise SystemExit('Firmware source freeze identity differs from its actual source hashes')
    family = build_root / 'spi_native' / freeze['identity']
    source = family / 'source'
    source.mkdir(parents=True, exist_ok=True)
    fixture_prefix = Path('tests/firmware/spi_native')
    for name, expected in freeze['sources'].items():
        original = ROOT / name
        if digest(original) != expected:
            raise SystemExit('Firmware changed after source freeze: ' + name)
        relative = Path(name).relative_to(fixture_prefix)
        target = source / relative
        if target.exists():
            if digest(target) != expected:
                raise SystemExit('Preserving changed native fixture snapshot: ' + str(target))
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
    (family / 'source-manifest.json').write_text(json.dumps(freeze, indent=2) + '\n')
    suffix = args.mode + ('-spi3' if args.negative_host == 3 and args.mode != 'owner_error' else '')
    build = family / suffix
    build.mkdir(exist_ok=True)
    defaults = [source / 'sdkconfig.defaults']
    if args.mode != 'connected':
        defaults.append(source / ('sdkconfig.' + args.mode + '.defaults'))
    if args.negative_host == 3 and args.mode in ('wrong_cs', 'disconnected_miso'):
        defaults.append(source / 'sdkconfig.negative_spi3.defaults')
    commands = []
    results = {'status': 'FAIL', 'idf_commit': actual_commit, 'source_identity': freeze['identity'],
               'native_project': str(source), 'build': str(build), 'mode': args.mode,
               'negative_host': args.negative_host, 'activation': str(activation), 'commands': commands,
               'qualification': 'Compile/image provenance only; no SPI runtime PASS'}

    def run(command, cwd, log):
        commands.append({'argv': command, 'cwd': str(cwd), 'log': log})
        started = time.monotonic()
        with (evidence / log).open('wb') as output:
            done = subprocess.run(command, cwd=cwd, env=environment, stdout=output, stderr=subprocess.STDOUT)
        commands[-1]['seconds'] = time.monotonic() - started
        commands[-1]['exit'] = done.returncode
        if done.returncode:
            raise RuntimeError('Actual command failed: ' + log)

    try:
        # Ninja job count is explicit, independent of an unsupported IDF env hint.
        if not (build / 'build.ninja').is_file():
            run(['python', str(idf / 'tools/idf.py'), '-C', str(source), '-B', str(build),
                 '-DIDF_TARGET=esp32s3', '-DSDKCONFIG=' + str(build / 'sdkconfig'),
                 '-DSDKCONFIG_DEFAULTS=' + ';'.join(map(str, defaults)), 'reconfigure'], source, 'configure.log')
        run(['ninja', '-j4', '-C', str(build)], source, 'build.log')
        image = build / 'spi-native.merged.bin'
        run(['python', '-m', 'esptool', '--chip', 'esp32s3', 'merge-bin', '--fill-flash-size', '4MB',
             '-o', str(image), '@flash_args'], build, 'merge.log')
        run(['python3', str(LANE / 'run-native-fixture.py'), '--mode', args.mode,
             '--flash', str(image), '--sdkconfig', str(build / 'sdkconfig'), '--idf', str(idf),
             '--record-build-pin', '--evidence', str(args.evidence)], source, 'build-pin.log')
        results['outputs'] = {str(path): digest(path) for path in
                              (image, build / 'spi_native.elf', build / 'sdkconfig', build / 'build-pin.json')}
        results['status'] = 'COMPILED_MERGED_PINNED'
    except Exception as exc:
        results['error'] = str(exc)
    finally:
        (evidence / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    return 0 if results['status'] == 'COMPILED_MERGED_PINNED' else 1


if __name__ == '__main__':
    raise SystemExit(main())
