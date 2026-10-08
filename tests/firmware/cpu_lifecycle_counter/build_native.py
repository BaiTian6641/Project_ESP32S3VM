#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build only this fixture on the locked native SDK; retain immutable inputs."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

SDK = Path('/home/polar/.cache/esp32s3vm/idf/idf-6.1-fff9895c82d744c7237be8847347bdd1b07c6643')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    original = Path(__file__).resolve().parent
    files = [original / 'CMakeLists.txt', original / 'sdkconfig.defaults']
    files += sorted(p for p in (original / 'main').rglob('*') if p.is_file())
    inputs = {str(p.relative_to(original)): sha(p) for p in files}
    identity = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    root = SDK / 'builds/cpu_lifecycle_counter' / identity
    root.mkdir(parents=True, exist_ok=False)
    source, build = root / 'source', root / 'build'
    for file in files:
        target = source / file.relative_to(original)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(file, target)
    receipt = {'source_files': inputs, 'source_sha256': identity,
               'source': str(source), 'sdk': str(SDK), 'commands': [], 'passed': False}
    try:
        activation = subprocess.run(['bash', '-c', 'source "$1" >&2 && env -0',
                                     'counter-sdk', str(SDK / 'activate.sh')],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        (root / 'activation.log').write_bytes(activation.stderr)
        environment = {k.decode(): v.decode() for k, v in
                       (entry.split(b'=', 1) for entry in activation.stdout.split(b'\0') if entry)}
        environment['CMAKE_BUILD_PARALLEL_LEVEL'] = '4'
        python = str(Path(environment['IDF_PYTHON_ENV_PATH']) / 'bin/python')
        (root / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n')

        def run(name, command, cwd):
            with (root / (name + '.log')).open('wb') as log:
                completed = subprocess.run(command, cwd=cwd, env=environment,
                                           stdout=log, stderr=subprocess.STDOUT)
            receipt['commands'].append({'name': name, 'command': command, 'cwd': str(cwd),
                                        'returncode': completed.returncode,
                                        'log_sha256': sha(root / (name + '.log'))})
            if completed.returncode:
                raise RuntimeError(name + ' failed')

        run('configure', [python, str(SDK / 'source/tools/idf.py'), '-B', str(build),
                          '-DIDF_TARGET=esp32s3', '-DSDKCONFIG=' + str(build / 'sdkconfig'),
                          'reconfigure'], source)
        run('build', ['ninja', '-j4'], build)
        run('merge', [python, '-m', 'esptool', '--chip', 'esp32s3', 'merge-bin',
                      '--fill-flash-size', '4MB', '-o', 'counter.merged.bin', '@flash_args'], build)
        receipt['artifacts'] = {str(p): sha(p) for p in
            [build / 'counter.merged.bin', build / 'esp32s3_cpu_lifecycle_counter.elf',
             build / 'sdkconfig', SDK / 'sdk-manifest.json']}
        receipt['passed'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        (root / 'build-result.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(root / 'build-result.json')
    return 0 if receipt['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
