# Usage: python -I tools/test_backdrop.py [--game-executable PATH] [--compiler PATH]
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from build import find_compiler


def test(compiler, game_executable=None):
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    host = build / 'backdrop-smoke.exe'
    subprocess.run([compiler, '-O2', '-Wall', '-Werror', '-static', '-municode', '-o', str(host),
                    str(ROOT / 'tests/backdrop_smoke.c'), str(ROOT / 'src/backdrop.c'),
                    '-luser32', '-lgdi32'], check=True)
    cases = []
    with tempfile.TemporaryDirectory(prefix='backdrop space ', dir=build) as directory:
        location = Path(directory)
        # Entirely synthetic colors/pixels. No original game resource enters hosted CI.
        palette = bytes([8, 46, 84, 0, 26, 63, 110, 0]) + bytes(254 * 4)
        pixels = bytearray(128 * 128)
        pixels[127 * 128 + 1] = 1
        header = struct.pack('<IiiHHIIiiII', 40, 128, 128, 1, 8, 0, len(pixels), 0, 0, 256, 0)
        bitmap = struct.pack('<2sIHHI', b'BM', 14 + len(header) + len(palette) + len(pixels), 0, 0, 14 + 40 + 1024)
        (location / 'tile.bmp').write_bytes(bitmap + header + palette + pixels)
        (location / 'truncated.bin').write_bytes(b'bad')
        # If mapping accidentally executes a fixture DLL, make it observable.
        (location / 'resource.c').write_text(
            '#include <windows.h>\nBOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {\n'
            'if (reason == DLL_PROCESS_ATTACH) { HANDLE marker = CreateFileA("entrypoint-ran.txt", '
            'GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); '
            'if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker); } return TRUE; }\n')
        for name, declaration, mode in (
            ('valid', '281 BITMAP "tile.bmp"', 'fixture'),
            ('truncated', '281 2 "truncated.bin"', 'fallback'),
            ('missing', '282 BITMAP "tile.bmp"', 'fallback'),
        ):
            (location / 'resource.rc').write_text(declaration + '\n')
            subprocess.run([str(Path(compiler).parent / 'llvm-rc.exe'), '/fo', 'resource.res', 'resource.rc'],
                           cwd=location, check=True)
            module = location / (name + '.gob')
            subprocess.run([compiler, '-shared', '-static', '-o', str(module), 'resource.c', 'resource.res'],
                           cwd=location, check=True)
            subprocess.run([str(host), str(module), mode], cwd=location, check=True, timeout=20)
            cases.append(name)
        subprocess.run([str(host), str(location / 'absent.gob'), 'fallback'], cwd=location, check=True, timeout=20)
        cases.append('absent-file')
        if (location / 'entrypoint-ran.txt').exists():
            raise ValueError('Resource-only mapping unexpectedly executed fixture code')
        if game_executable:
            subprocess.run([str(host), str(game_executable.resolve()), 'game'], cwd=location, check=True, timeout=20)
            cases.append('installed-game-resource')
    return {'status': 'ok', 'result': {'cases': cases}, 'errors': [], 'warnings': []}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    parser.add_argument('--game-executable', type=Path)
    args = parser.parse_args()
    print(json.dumps(test(find_compiler(args.compiler), args.game_executable), indent=2))
