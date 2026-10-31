# Usage: python -I tools/build_music.py [--game-codec PATH] [--compiler PATH]
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_compiler
from pe import inspect_dll

PINS = json.loads((ROOT / 'dependencies.json').read_text())
REQUIRED = ['ov_clear', 'ov_fopen', 'ov_info', 'ov_read', 'ov_time_seek', 'ov_time_total']


def run(arguments):
    subprocess.run([str(argument) for argument in arguments], check=True)


def build_codecs(compiler):
    prefix = ROOT / 'build/codecs/install'
    ogg = ROOT / 'deps' / PINS['libogg']['directory']
    vorbis = ROOT / 'deps' / PINS['libvorbis']['directory']
    include = ['-I' + str(ogg / 'include'), '-I' + str(vorbis / 'include'), '-I' + str(vorbis / 'lib')]
    sources = {
        'ogg': [ogg / 'src' / name for name in ('bitwise.c', 'framing.c')],
        'vorbis': [vorbis / 'lib' / (name + '.c') for name in (
            'mdct', 'smallft', 'block', 'envelope', 'window', 'lsp', 'lpc', 'analysis',
            'synthesis', 'psy', 'info', 'floor1', 'floor0', 'res0', 'mapping0',
            'registry', 'codebook', 'sharedbook', 'lookup', 'bitrate', 'vorbisenc')],
        'vorbisfile': [vorbis / 'lib/vorbisfile.c'],
    }
    (prefix / 'lib').mkdir(parents=True, exist_ok=True)
    for library, paths in sources.items():
        objects = []
        for source in paths:
            output = ROOT / 'build/codecs' / library / (source.stem + '.o')
            output.parent.mkdir(parents=True, exist_ok=True)
            run([compiler, '-std=gnu99', '-O2', '-c', source, '-o', output, *include])
            objects.append(output)
        run([Path(compiler).parent / 'llvm-ar.exe', 'rcs', prefix / 'lib' / ('lib' + library + '.a'), *objects])
    return prefix


def build_music(compiler, game_codec=None):
    source = ROOT / 'deps/ogg-winmm'
    pin = PINS['ogg_winmm']['commit']
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != pin or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True):
        raise ValueError('Music source must match the clean pinned commit')
    prefix = build_codecs(compiler)
    output = ROOT / 'build/music'
    output.mkdir(parents=True, exist_ok=True)
    dlltool = Path(compiler).parent / 'llvm-dlltool.exe'
    definition = ROOT / 'music/game_vorbisfile.def'
    import_library = output / 'game-vorbisfile.a'
    run([dlltool, '-m', 'i386', '-d', definition, '-l', import_library])
    codec_info = inspect_dll(game_codec, REQUIRED) if game_codec else None
    records = {}
    for variant in ('gog-codecs', 'current-codecs'):
        destination = output / variant / 'winmm.dll'
        destination.parent.mkdir(parents=True, exist_ok=True)
        libraries = ([import_library] if variant == 'gog-codecs' else
                     [prefix / 'lib' / name for name in ('libvorbisfile.a', 'libvorbis.a', 'libogg.a')])
        include = (source / 'libs/include' if variant == 'gog-codecs' else
                   ROOT / 'deps' / PINS['libvorbis']['directory'] / 'include')
        ogg_include = ROOT / 'deps' / PINS['libogg']['directory'] / 'include'
        run([compiler, '-std=gnu99', '-O2', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
             '-Wno-format', '-shared', '-static', '-Wl,--enable-stdcall-fixup', '-o', destination,
             *(source / name for name in ('ogg-winmm.c', 'player.c', 'stubs.c', 'ogg-winmm.def')),
             '-I' + str(include), '-I' + str(ogg_include), *libraries, '-lwinmm'])
        information = inspect_dll(destination, ['mciSendCommandA', 'mciSendStringA', 'waveOutOpen'])
        has_external_codec = any('vorbis' in name or 'ogg' in name for name in information['imports'])
        if has_external_codec != (variant == 'gog-codecs'):
            raise ValueError('Unexpected codec linkage: ' + variant)
        records[variant] = {'sha256': hashlib.sha256(destination.read_bytes()).hexdigest(),
                            **information}
    information = {'source_commit': pin, 'libogg': PINS['libogg']['version'],
                   'libvorbis': PINS['libvorbis']['version'], 'variants': records,
                   'game_codec_checked': str(game_codec) if game_codec else None,
                   'game_codec_sha256': hashlib.sha256(game_codec.read_bytes()).hexdigest() if game_codec else None}
    (output / 'build-info.json').write_text(json.dumps(information, indent=2) + '\n')
    summary = {variant: {'sha256': record['sha256'], 'export_count': len(record['exports']),
                         'imports': record['imports']} for variant, record in records.items()}
    return {'status': 'ok', 'result': {'variants': summary, 'source_commit': pin,
                                      'game_codec_checked': bool(codec_info)}, 'errors': [],
            'warnings': [] if codec_info else ['GOG codec ABI exports were not checked against an installed game DLL']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    parser.add_argument('--game-codec', type=Path)
    args = parser.parse_args()
    print(json.dumps(build_music(find_compiler(args.compiler), args.game_codec), indent=2))
