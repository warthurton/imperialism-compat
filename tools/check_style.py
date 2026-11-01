# Usage: python -I tools/check_style.py [--fix]
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from build import find_compiler


def check(fix=False):
    files = sorted([*ROOT.glob('src/*.c'), *ROOT.glob('src/*.h')])
    formatter = Path(find_compiler()).parent / 'clang-format.exe'
    arguments = ['-i'] if fix else ['--dry-run', '--Werror']
    subprocess.run([str(formatter), *arguments, *(str(path) for path in files)], check=True)
    # Long strings and comments must be wrapped too, rather than ignored by the formatter.
    too_long = [str(path.relative_to(ROOT)) + ':' + str(index)
                for path in files for index, line in enumerate(path.read_text().splitlines(), 1)
                if len(line.expandtabs(4)) > 100]
    if too_long:
        raise ValueError('Shim source exceeds 100 columns: ' + ', '.join(too_long))
    return {'status': 'ok', 'result': {'files': len(files), 'column_limit': 100},
            'errors': [], 'warnings': []}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fix', action='store_true')
    print(json.dumps(check(parser.parse_args().fix), indent=2))
