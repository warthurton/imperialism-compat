# Usage: python -I tools/release.py (matching version-tag CI publishes the release)
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def release_assets(root, version):
    names = ['imperialism-compat-' + component + '-' + version + '-win32.zip'
             for component in ('display', 'music-gog-codecs', 'music-current-codecs')]
    assets = [root / 'dist' / name for name in names]
    if {path.name for path in (root / 'dist').glob('*.zip')} != set(names):
        raise ValueError('Expected exactly the display and two music ZIPs for VERSION')
    checksums = root / 'dist/SHA256SUMS.txt'
    expected = dict(line.split('  ', 1)[::-1] for line in checksums.read_text().splitlines())
    if set(expected) != set(names):
        raise ValueError('Checksum list must cover exactly the release ZIPs')
    for path in assets:
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected[path.name]:
            raise ValueError('Release checksum mismatch: ' + path.name)
    return [*assets, checksums]


def publish(root=ROOT, environment=None):
    environment = os.environ if environment is None else environment
    version = (root / 'VERSION').read_text().strip()
    tag = environment.get('GITHUB_REF_NAME')
    if tag != 'v' + version:
        raise ValueError('Release tag must match VERSION')
    assets = release_assets(root, version)
    existing = subprocess.run(['gh', 'release', 'view', tag, '--json', 'isDraft,url,assets'],
                              capture_output=True, text=True, timeout=90)
    if existing.returncode == 0:
        release = json.loads(existing.stdout)
        if not release['isDraft']:
            if {asset['name'] for asset in release['assets']} != {path.name for path in assets}:
                raise ValueError('Published release is missing expected assets; review it without overwriting downloads')
            # Published downloads remain immutable when somebody reruns the tag workflow.
            return {'status': 'ok', 'result': release['url'], 'errors': [],
                    'warnings': ['Release already published; assets were not overwritten.']}
        if {asset['name'] for asset in release['assets']} - {path.name for path in assets}:
            raise ValueError('Existing draft contains unexpected assets; review it before retrying')
    else:
        subprocess.run(['gh', 'release', 'create', tag, '--draft', '--verify-tag',
                        '--title', 'Imperialism Compat ' + version,
                        '--notes-file', str(root / 'docs/release-notes.md')], check=True, timeout=90)
    # Keep the draft private until every tested package and its checksum list is uploaded.
    subprocess.run(['gh', 'release', 'upload', tag, '--clobber', *(str(path) for path in assets)],
                   check=True, timeout=180)
    preview = '-' in version
    subprocess.run(['gh', 'release', 'edit', tag, '--draft=false',
                    '--prerelease=' + str(preview).lower(), '--latest=' + str(not preview).lower(),
                    '--title', 'Imperialism Compat ' + version,
                    '--notes-file', str(root / 'docs/release-notes.md')], check=True, timeout=90)
    result = subprocess.check_output(['gh', 'release', 'view', tag, '--json', 'url'], text=True, timeout=90)
    return {'status': 'ok', 'result': json.loads(result)['url'], 'errors': [], 'warnings': []}


if __name__ == '__main__':
    print(json.dumps(publish(), indent=2))
