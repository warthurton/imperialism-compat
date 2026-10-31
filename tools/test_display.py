# Usage: python -I tools/test_display.py [--compiler PATH]
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


def test(compiler):
    dll = ROOT / 'build/msvfw32.dll'
    information = inspect_dll(dll, ['MCIWndCreateA'])
    if information['exports'] != ['MCIWndCreateA']:
        raise ValueError('Unexpected display exports')
    host = ROOT / 'build/load-shim.exe'
    subprocess.run([compiler, '-O2', '-Wall', '-Werror', '-static', '-o', str(host),
                    str(ROOT / 'tests/load_shim.c'), '-luser32', '-lgdi32'], check=True)
    for enabled in (0, 1):
        with tempfile.TemporaryDirectory(prefix='smoke space ', dir=ROOT / 'build') as directory:
            location = Path(directory)
            target = location / dll.name
            shutil.copyfile(dll, target)
            (location / 'imperialism_shim.ini').write_text('[text_log]\nenabled=%d\n' % enabled)
            subprocess.run([str(host), str(target)], cwd=location, check=True, timeout=15)
            log = (location / 'imperialism_gdi_log.txt').read_text()
            if 'ChangeDisplaySettingsA' not in log or '-> blocked' not in log:
                raise ValueError('Display-mode import was not intercepted')
            text = location / 'imperialism_text.log'
            if text.exists() != bool(enabled):
                raise ValueError('Text-log setting was not honored')
            if enabled and 'shim smoke' not in text.read_text():
                raise ValueError('TextOutA capture missing')
    report = {'status': 'ok', 'result': {'abi': information, 'smoke_cases': 2}, 'errors': [], 'warnings': []}
    (ROOT / 'build/display-test-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    args = parser.parse_args()
    print(json.dumps(test(find_compiler(args.compiler)), indent=2))
