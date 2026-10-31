# Usage: python -I tools/test_music.py [--game-directory PATH] [--compiler PATH]
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_compiler
from pe import inspect_dll


def test(compiler, game_directory=None):
    pins = json.loads((ROOT / 'dependencies.json').read_text())
    codec = ROOT / 'build/codecs/install/lib'
    host = ROOT / 'build/music-smoke.exe'
    subprocess.run([compiler, '-std=gnu99', '-O2', '-Wall', '-Werror', '-static', '-o', str(host),
                    str(ROOT / 'tests/music_smoke.c'),
                    '-I' + str(ROOT / 'deps' / pins['libvorbis']['directory'] / 'include'),
                    '-I' + str(ROOT / 'deps' / pins['libogg']['directory'] / 'include'),
                    *(str(codec / name) for name in ('libvorbisfile.a', 'libvorbis.a', 'libogg.a'))], check=True)
    records = {}
    for variant in ('current-codecs', 'gog-codecs'):
        dll = ROOT / 'build/music' / variant / 'winmm.dll'
        records[variant] = inspect_dll(dll, ['mciSendStringA', 'mciSendCommandA', 'waveOutOpen'])
        if variant == 'gog-codecs' and not game_directory:
            records[variant]['native_smoke'] = 'not run: installed game codec DLLs required locally'
            continue
        with tempfile.TemporaryDirectory(prefix='music space ', dir=ROOT / 'build') as directory:
            location = Path(directory)
            (location / 'Music').mkdir()
            target = location / 'winmm.dll'
            shutil.copyfile(dll, target)
            shutil.copyfile(ROOT / 'music/winmm.ini', location / 'winmm.ini')
            if variant == 'gog-codecs':
                for name in ('libvorbisfile-3.dll', 'libvorbis-0.dll', 'libogg-0.dll'):
                    shutil.copyfile(game_directory / name, location / name)
            subprocess.run([str(host), str(target), str(location / 'Music/Track02.ogg')],
                           cwd=location, timeout=20, check=True)
            records[variant]['native_smoke'] = 'passed'
    for record in records.values():
        record['export_count'] = len(record.pop('exports'))
    report = {'status': 'ok', 'result': records, 'errors': [],
            'warnings': ['No playback or audible-output claim: hosted CI may have no audio device.']}
    (ROOT / 'build/music/test-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    parser.add_argument('--game-directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(test(find_compiler(args.compiler), args.game_directory), indent=2))
