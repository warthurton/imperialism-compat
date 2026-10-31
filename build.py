# Usage: python -I build.py [--compiler PATH] [--output DIRECTORY]
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
FLAGS = ['-O2', '-Wall', '-Werror', '-shared', '-static']
INPUTS = ['src/shim.c', 'src/msvfw32.def']


def find_compiler(explicit=None):
    candidates = [explicit]
    if os.environ.get('LLVM_MINGW'):
        candidates.append(str(Path(os.environ['LLVM_MINGW']) / 'i686-w64-mingw32-clang'))
    pin = json.loads((ROOT / 'dependencies.json').read_text())['llvm_mingw']['directory']
    candidates.append(str(ROOT / 'toolchain' / pin / 'bin/i686-w64-mingw32-clang.exe'))
    candidates.extend(['i686-w64-mingw32-clang', str(Path.home() / 'scoop/apps/'
                       'mingw-mstorsjo-llvm-ucrt/current/bin/i686-w64-mingw32-clang.exe')])
    for candidate in candidates:
        if candidate and (resolved := shutil.which(candidate)):
            return resolved
        if candidate == explicit and explicit:
            raise FileNotFoundError('Compiler not found: ' + explicit)
    raise FileNotFoundError('Install llvm-mingw, add its bin directory to PATH, or set LLVM_MINGW')


def input_hashes():
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}


def build(compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    destination = output / 'msvfw32.dll'
    subprocess.run([compiler, *FLAGS, '-o', str(destination),
                    *(str(ROOT / name) for name in INPUTS), '-luser32', '-lgdi32', '-ldwmapi'], check=True)
    information = {
        'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'target': subprocess.check_output([compiler, '-dumpmachine'], text=True).strip(),
        'flags': FLAGS, 'source_sha256': input_hashes(),
        'dll_sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
    }
    (output / 'build-info.json').write_text(json.dumps(information, indent=2) + '\n', encoding='utf-8')
    return {'status': 'ok', 'result': {'dll': str(destination), **information}, 'errors': [], 'warnings': []}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    parser.add_argument('--output', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    print(json.dumps(build(find_compiler(args.compiler), args.output.resolve()), indent=2))
