"""Verify that music build inputs match the sources shipped in releases."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(directory):
    return {path.relative_to(directory).as_posix(): digest(path)
            for path in sorted(directory.rglob('*'))
            if path.is_file() and '.git' not in path.relative_to(directory).parts}


def music_inputs():
    pins = json.loads((ROOT / 'dependencies.json').read_text())
    upstream = ROOT / 'deps/ogg-winmm'
    revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(upstream), 'status', '--porcelain'], text=True)
    if revision != pins['ogg_winmm']['commit'] or dirty:
        raise ValueError('Music source must match the clean pinned commit')
    tracked = subprocess.check_output(['git', '-C', str(upstream), 'ls-files', '-z']).decode().split('\0')[:-1]
    upstream_files = inventory(upstream)
    if set(upstream_files) != set(tracked):
        raise ValueError('Unexpected files in music source checkout, including ignored files')
    records = {'ogg_winmm': upstream_files}
    for name in ('libogg', 'libvorbis'):
        pin = pins[name]
        archive = ROOT / 'deps' / pin['url'].rsplit('/', 1)[-1]
        if digest(archive) != pin['sha256']:
            raise ValueError('Codec source archive changed: ' + name)
        expected = {}
        with tarfile.open(archive) as bundle:
            for entry in bundle:
                relative = Path(entry.name).relative_to(pin['directory']).as_posix()
                if entry.isfile():
                    expected[relative] = hashlib.sha256(bundle.extractfile(entry).read()).hexdigest()
                elif not entry.isdir():
                    raise ValueError('Unsupported codec archive entry: ' + entry.name)
        if inventory(ROOT / 'deps' / pin['directory']) != expected:
            raise ValueError('Extracted codec tree differs from pinned archive: ' + name)
        records[name] = expected
    records['build_tools'] = {name: digest(ROOT / name) for name in (
        'dependencies.json', 'build.py', 'tools/build_music.py', 'tools/provenance.py',
        'tools/pe.py', 'music/game_vorbisfile.def')}
    return {name: hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
            for name, files in records.items()}
