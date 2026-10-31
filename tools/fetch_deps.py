# Usage: python -I tools/fetch_deps.py [--toolchain]
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PINS = json.loads((ROOT / 'dependencies.json').read_text())


def fetch_archive(name):
    pin = PINS[name]
    destination = ROOT / ('toolchain' if name in ('llvm_mingw', 'cmake') else 'deps')
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / pin['url'].rsplit('/', 1)[-1]
    if not archive.exists():
        print('Downloading ' + name, flush=True)
        started = time.monotonic()
        partial = archive.with_suffix(archive.suffix + '.part')
        with urllib.request.urlopen(pin['url'], timeout=30) as response, partial.open('wb') as output:
            downloaded = 0
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
                downloaded += len(chunk)
                print('%s: %.1f MiB' % (name, downloaded / 1048576), flush=True)
                if time.monotonic() - started > 300:
                    raise TimeoutError('Download exceeded five minutes: ' + name)
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        raise ValueError('Checksum mismatch; remove the failed archive before retrying: ' + str(archive))
    extracted = destination / pin['directory']
    marker = destination / (pin['directory'] + '.verified')
    if not marker.exists():
        if archive.suffix == '.zip':
            with zipfile.ZipFile(archive) as bundle:
                for entry in bundle.namelist():
                    if not (destination / entry).resolve().is_relative_to(destination.resolve()):
                        raise ValueError('Unsafe archive entry: ' + entry)
                bundle.extractall(destination)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(destination, filter='data')
        marker.write_text(pin['sha256'] + '\n')
    if not extracted.is_dir():
        raise ValueError('Archive directory missing: ' + str(extracted))
    return {'name': name, 'directory': str(extracted), 'sha256': pin['sha256']}


def fetch_music_source():
    source = ROOT / 'deps/ogg-winmm'
    pin = PINS['ogg_winmm']
    if not source.exists():
        subprocess.run(['git', 'clone', '--no-checkout', pin['url'], str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', pin['commit']], check=True)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True)
    if actual != pin['commit'] or dirty:
        raise ValueError('ogg-winmm must be a clean checkout of pinned commit ' + pin['commit'])
    return {'name': 'ogg_winmm', 'commit': actual}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--toolchain', action='store_true')
    args = parser.parse_args()
    names = ['libogg', 'libvorbis']
    if args.toolchain:
        names.insert(0, 'llvm_mingw')
    result = [fetch_archive(name) for name in names]
    result.append(fetch_music_source())
    print(json.dumps({'status': 'ok', 'result': result, 'errors': [], 'warnings': []}, indent=2))
