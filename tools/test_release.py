# Usage: python -I tools/test_release.py
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release import publish


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / 'VERSION').write_text('0.1.0-preview.1')
        (self.root / 'dist').mkdir()
        self.assets = []
        for component in ('display', 'music-gog-codecs', 'music-current-codecs'):
            asset = self.root / 'dist' / ('imperialism-compat-' + component + '-0.1.0-preview.1-win32.zip')
            asset.write_bytes(b'test fixture only')
            self.assets.append(asset)
        (self.root / 'dist/SHA256SUMS.txt').write_text(''.join(
            hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name + '\n' for path in self.assets))
        self.environment = {'GITHUB_REF_NAME': 'v0.1.0-preview.1'}

    def test_wrong_tag_never_contacts_github(self):
        with patch('release.subprocess.run') as command:
            with self.assertRaisesRegex(ValueError, 'tag must match'):
                publish(self.root, {'GITHUB_REF_NAME': 'main'})
        command.assert_not_called()

    def test_changed_asset_never_contacts_github(self):
        self.assets[0].write_bytes(b'changed after packaging')
        with patch('release.subprocess.run') as command:
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                publish(self.root, self.environment)
        command.assert_not_called()

    def test_upload_precedes_publication(self):
        missing = subprocess.CompletedProcess([], 1, '', 'release not found')
        success = subprocess.CompletedProcess([], 0, '', '')
        with patch('release.subprocess.run', side_effect=[missing, success, success, success]) as command, \
                patch('release.subprocess.check_output', return_value='{"url":"https://example.invalid/release"}'):
            result = publish(self.root, self.environment)
        arguments = [call.args[0] for call in command.call_args_list]
        self.assertIn('--draft', arguments[1])
        self.assertEqual(arguments[2][2], 'upload')
        self.assertIn('--draft=false', arguments[3])
        self.assertIn('--prerelease=true', arguments[3])
        self.assertEqual(result['status'], 'ok')

    def test_published_assets_are_not_overwritten(self):
        existing = subprocess.CompletedProcess([], 0, json.dumps(
            {'isDraft': False, 'url': 'https://example.invalid/release',
             'assets': [{'name': path.name} for path in self.assets] + [{'name': 'SHA256SUMS.txt'}]}), '')
        with patch('release.subprocess.run', return_value=existing) as command:
            result = publish(self.root, self.environment)
        self.assertEqual(command.call_count, 1)
        self.assertTrue(result['warnings'])

    def test_incomplete_published_release_is_not_reported_as_success(self):
        existing = subprocess.CompletedProcess([], 0, json.dumps(
            {'isDraft': False, 'url': 'https://example.invalid/release', 'assets': []}), '')
        with patch('release.subprocess.run', return_value=existing) as command:
            with self.assertRaisesRegex(ValueError, 'missing expected assets'):
                publish(self.root, self.environment)
        self.assertEqual(command.call_count, 1)


if __name__ == '__main__':
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ReleaseTests))
    print(json.dumps({'status': 'ok' if result.wasSuccessful() else 'error',
                      'result': {'tests': result.testsRun}, 'errors': len(result.errors) + len(result.failures),
                      'warnings': []}))
    sys.exit(not result.wasSuccessful())
