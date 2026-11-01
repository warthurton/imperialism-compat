# Usage: python -I tools/test_fetch.py
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fetch_deps import download


class DownloadTests(unittest.TestCase):
    def test_transient_connection_recovers(self):
        data = b'pinned source archive'
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'source.tar.xz'
            with patch('fetch_deps.urllib.request.urlopen', side_effect=[
                    urllib.error.URLError('TLS EOF'), io.BytesIO(data)]) as request, \
                    patch('fetch_deps.time.sleep'):
                download('https://example.invalid/source', target, hashlib.sha256(data).hexdigest())
            self.assertEqual(request.call_count, 2)
            self.assertEqual(target.read_bytes(), data)

    def test_checksum_failure_is_not_retried_or_promoted(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'source.tar.xz'
            with patch('fetch_deps.urllib.request.urlopen', return_value=io.BytesIO(b'wrong')) as request:
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    download('https://example.invalid/source', target, '0' * 64)
            self.assertEqual(request.call_count, 1)
            self.assertFalse(target.exists())

    def test_missing_resource_is_not_retried(self):
        with tempfile.TemporaryDirectory() as directory:
            error = urllib.error.HTTPError('https://example.invalid/source', 404, 'missing', {}, None)
            with patch('fetch_deps.urllib.request.urlopen', side_effect=error) as request:
                with self.assertRaises(urllib.error.HTTPError):
                    download(error.url, Path(directory) / 'source.tar.xz', '0' * 64)
            self.assertEqual(request.call_count, 1)

    def test_connection_retries_are_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch('fetch_deps.urllib.request.urlopen', side_effect=urllib.error.URLError('offline')) as request, \
                    patch('fetch_deps.time.sleep'):
                with self.assertRaises(urllib.error.URLError):
                    download('https://example.invalid/source', Path(directory) / 'source.tar.xz', '0' * 64)
            self.assertEqual(request.call_count, 3)


if __name__ == '__main__':
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(DownloadTests))
    print(json.dumps({'status': 'ok' if result.wasSuccessful() else 'error',
                      'result': {'tests': result.testsRun}, 'errors': len(result.errors) + len(result.failures),
                      'warnings': []}))
    sys.exit(not result.wasSuccessful())
