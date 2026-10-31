# Usage: python -I tools/package.py
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import input_hashes
from pe import inspect_dll


def tracked_files(repository):
    return subprocess.check_output(['git', '-C', str(repository), 'ls-files', '-z']).decode().split('\0')[:-1]


def archive(path, files, metadata):
    manifest = dict(metadata, sha256={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in sorted(files.items()):
            output.writestr(name, data)
        output.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
    with zipfile.ZipFile(path) as output:
        if output.testzip():
            raise ValueError('Archive CRC check failed')
        for name, digest in manifest['sha256'].items():
            if hashlib.sha256(output.read(name)).hexdigest() != digest:
                raise ValueError('Packaged hash mismatch: ' + name)


def package():
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?', version):
        raise ValueError('Invalid VERSION')
    tracked = tracked_files(ROOT)
    forbidden = {'.dll', '.exe', '.bin', '.imp', '.gob', '.ogg', '.a', '.lib'}
    if invalid := [name for name in tracked if Path(name).suffix.lower() in forbidden]:
        raise ValueError('Tracked binary/game files: ' + ', '.join(invalid))
    display_build = json.loads((ROOT / 'build/build-info.json').read_text())
    if display_build['source_sha256'] != input_hashes():
        raise ValueError('Display source changed since compilation; rebuild first')
    pins = json.loads((ROOT / 'dependencies.json').read_text())
    music_build = json.loads((ROOT / 'build/music/build-info.json').read_text())
    if music_build['source_commit'] != pins['ogg_winmm']['commit']:
        raise ValueError('Music source pin differs from compiled build')
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True))
    metadata = {'version': version, 'source_commit': commit, 'source_tree_dirty': dirty}
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    common = {name: (ROOT / name).read_bytes() for name in ('README.md', 'LICENSE', 'ORIGIN.md', 'CHANGELOG.md')}
    corresponding = {'imperialism-compat/' + name: (ROOT / name).read_bytes() for name in tracked}
    upstream = ROOT / 'deps/ogg-winmm'
    for name in tracked_files(upstream):
        if Path(name).suffix.lower() not in {'.a', '.dll', '.exe', '.lib', '.zip'}:
            corresponding['dependencies/ogg-winmm/' + name] = (upstream / name).read_bytes()
    for name in ('libogg', 'libvorbis'):
        pin = pins[name]
        source = ROOT / 'deps' / pin['url'].rsplit('/', 1)[-1]
        if hashlib.sha256(source.read_bytes()).hexdigest() != pin['sha256']:
            raise ValueError('Codec source archive changed: ' + name)
        corresponding['dependencies/' + source.name] = source.read_bytes()
    source_archive = ROOT / 'build/corresponding-source.zip'
    archive(source_archive, corresponding, metadata)
    outputs = []
    display = ROOT / 'build/msvfw32.dll'
    inspect_dll(display, ['MCIWndCreateA'])
    if hashlib.sha256(display.read_bytes()).hexdigest() != display_build['dll_sha256']:
        raise ValueError('Display DLL changed since compilation')
    display_files = dict(common, **{'msvfw32.dll': display.read_bytes(),
                         'imperialism_shim.ini': (ROOT / 'imperialism_shim.ini').read_bytes(),
                         'test-report.json': (ROOT / 'build/display-test-report.json').read_bytes()})
    display_zip = dist / ('imperialism-compat-display-' + version + '-win32.zip')
    archive(display_zip, display_files, dict(metadata, component='display', build=display_build))
    outputs.append(display_zip)
    for variant in ('gog-codecs', 'current-codecs'):
        dll = ROOT / 'build/music' / variant / 'winmm.dll'
        inspect_dll(dll, ['mciSendStringA', 'mciSendCommandA', 'waveOutOpen'])
        if hashlib.sha256(dll.read_bytes()).hexdigest() != music_build['variants'][variant]['sha256']:
            raise ValueError('Music DLL changed since compilation: ' + variant)
        files = dict(common, **{'winmm.dll': dll.read_bytes(),
                     'winmm.ini': (ROOT / 'music/winmm.ini').read_bytes(),
                     'COPYING-ogg-winmm.txt': (upstream / 'LICENSE.txt').read_bytes(),
                     'NOTICE-ogg-winmm.txt': (upstream / 'ogg-winmm.c').read_text().split('*/', 1)[0].encode() + b'*/\n',
                     'COPYING-libogg.txt': (ROOT / 'deps' / pins['libogg']['directory'] / 'COPYING').read_bytes(),
                     'COPYING-libvorbis.txt': (ROOT / 'deps' / pins['libvorbis']['directory'] / 'COPYING').read_bytes(),
                     'corresponding-source.zip': source_archive.read_bytes(),
                     'test-report.json': (ROOT / 'build/music/test-report.json').read_bytes()})
        destination = dist / ('imperialism-compat-music-' + variant + '-' + version + '-win32.zip')
        # Deliberately omit the local game path and hash from distributable metadata.
        archive(destination, files, dict(metadata, component='music', codec_variant=variant,
                upstream_commit=pins['ogg_winmm']['commit'],
                static_codec_versions=({'libogg': pins['libogg']['version'],
                                        'libvorbis': pins['libvorbis']['version']}
                                       if variant == 'current-codecs' else None)))
        outputs.append(destination)
    (dist / 'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(path.read_bytes()).hexdigest() +
                                               '  ' + path.name + '\n' for path in outputs))
    return {'status': 'ok', 'result': [str(path) for path in outputs], 'errors': [],
            'warnings': ['Working tree differs from source commit'] if dirty else []}


if __name__ == '__main__':
    print(json.dumps(package(), indent=2))
