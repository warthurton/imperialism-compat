# Usage: python -I tools/test_package.py (after building and testing both components)
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package import ROOT, package


def test():
    packaged = package()
    cases = [
        ('build/display-test-report.json', 'Test report is stale', 'report'),
        ('build/music/test-report.json', 'Test report is stale', 'music-report'),
        ('deps/ogg-winmm/player.c', 'clean pinned commit', 'append'),
        ('deps/libogg-1.3.6/include/ogg/ogg.h', 'differs from pinned archive', 'append'),
        ('music/game_vorbisfile.def', 'inputs changed since compilation', 'append'),
    ]
    rejected = []
    for name, expected, operation in cases:
        path = ROOT / name
        original = path.read_bytes()
        try:
            if operation.endswith('report'):
                report = json.loads(original)
                result = report['result']['current-codecs'] if operation == 'music-report' else report['result']
                result['dll_sha256'] = '0' * 64
                path.write_text(json.dumps(report))
            else:
                path.write_bytes(original + b'\n/* packaging regression probe */\n')
            try:
                package()
            except ValueError as error:
                if expected not in str(error):
                    raise AssertionError('Unexpected rejection: ' + str(error)) from error
                rejected.append(name)
            else:
                raise AssertionError('Packaging accepted changed input: ' + name)
        finally:
            path.write_bytes(original)
    return {'status': 'ok', 'result': {'packages': packaged['result'], 'rejected_changes': rejected},
            'errors': [], 'warnings': packaged['warnings']}


if __name__ == '__main__':
    print(json.dumps(test(), indent=2))
