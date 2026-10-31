# Usage: python -I tools/release.py (tag-triggered CI only; creates an unpublished draft)
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / 'VERSION').read_text().strip()
tag = os.environ['GITHUB_REF_NAME']
if tag != 'v' + version:
    raise ValueError('Release tag must match VERSION')
assets = sorted((ROOT / 'dist').glob('*.zip'))
if len(assets) != 3:
    raise ValueError('Expected display plus two music-variant ZIPs')
subprocess.run(['gh', 'release', 'create', tag, '--draft', '--prerelease', '--verify-tag',
                '--title', 'Imperialism Compat ' + version,
                '--notes-file', str(ROOT / 'docs/release-notes.md'),
                *(str(path) for path in assets), str(ROOT / 'dist/SHA256SUMS.txt')], check=True)
