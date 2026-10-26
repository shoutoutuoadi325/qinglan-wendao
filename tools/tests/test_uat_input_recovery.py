"""Offline tests for minimal UAT recovery, corruption checks, and no-overwrite behavior."""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('uat_recovery', ROOT / 'tools/build/restore-uat-inputs.py')
RECOVERY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECOVERY)


class UatInputRecovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.engine, self.cache = self.base / 'engine', self.base / 'cache'
        self.evidence = self.base / 'evidence.json'
        self.contents = [b'assembly fixture', b'green fixture', b'grey fixture']
        raw = b''.join(self.contents)
        self.pack_hash = hashlib.sha1(raw).hexdigest()
        self.compressed = gzip.compress(raw)
        self.cache_file = self.cache / self.pack_hash[:2] / self.pack_hash
        self.cache_file.parent.mkdir(parents=True)
        self.cache_file.write_bytes(self.compressed)
        root = ET.Element('DependencyManifest', BaseUrl=RECOVERY.OFFICIAL_BASE_URL)
        files, blobs, packs = [ET.SubElement(root, name) for name in ('Files', 'Blobs', 'Packs')]
        offset = 0
        for name, data in zip(RECOVERY.REQUIRED_FILES, self.contents):
            digest = hashlib.sha1(data).hexdigest()
            ET.SubElement(files, 'File', Name=name, Hash=digest)
            ET.SubElement(blobs, 'Blob', Hash=digest, Size=str(len(data)), PackHash=self.pack_hash, PackOffset=str(offset))
            offset += len(data)
        ET.SubElement(packs, 'Pack', Hash=self.pack_hash, Size=str(len(raw)),
                      CompressedSize=str(len(self.compressed)), RemotePath='UnrealEngine-1234')
        self.manifest = self.engine / 'Engine/Build/Commit.gitdeps.xml'
        self.manifest.parent.mkdir(parents=True)
        ET.ElementTree(root).write(self.manifest)

    def tearDown(self):
        self.tmp.cleanup()

    def restore(self, verify_only=False):
        return RECOVERY.restore_inputs(self.engine, self.cache, self.evidence, verify_only)

    def test_cached_recovery_then_read_only_verification(self):
        with mock.patch.object(RECOVERY.urllib.request, 'urlopen', side_effect=AssertionError('unexpected network')):
            first = self.restore()
            for name, data in zip(RECOVERY.REQUIRED_FILES, self.contents):
                self.assertEqual((self.engine / name).read_bytes(), data)
            before = [(self.engine / name).stat().st_mtime_ns for name in RECOVERY.REQUIRED_FILES]
            second = self.restore(verify_only=True)
        self.assertEqual(first['download_bytes'], 0)
        self.assertEqual(second['packs'], [])
        self.assertEqual(before, [(self.engine / name).stat().st_mtime_ns for name in RECOVERY.REQUIRED_FILES])
        self.assertTrue(json.loads(self.evidence.read_text())['verify_only'])

    def test_existing_conflict_is_never_overwritten_and_other_inputs_stay_absent(self):
        target = self.engine / RECOVERY.REQUIRED_FILES[-1]
        target.parent.mkdir(parents=True)
        target.write_bytes(b'owner change')
        with self.assertRaisesRegex(ValueError, 'refusing overwrite'):
            self.restore()
        self.assertEqual(target.read_bytes(), b'owner change')
        self.assertFalse((self.engine / RECOVERY.REQUIRED_FILES[0]).exists())

    def test_corrupt_pack_or_blob_is_rejected_before_publishing(self):
        original = self.cache_file.read_bytes()
        self.cache_file.write_bytes(original[:-1])
        with self.assertRaisesRegex(ValueError, 'pack size mismatch'):
            self.restore()
        self.cache_file.write_bytes(original)
        tree = ET.parse(self.manifest)
        tree.getroot().find('Blobs')[0].set('PackOffset', '1')
        tree.write(self.manifest)
        with self.assertRaisesRegex(ValueError, 'file SHA1 mismatch'):
            self.restore()
        self.assertFalse((self.engine / RECOVERY.REQUIRED_FILES[0]).exists())

    def test_download_is_official_bounded_and_hash_checked(self):
        self.cache_file.unlink()
        response = io.BytesIO(self.compressed)
        response.geturl = lambda: f'{RECOVERY.OFFICIAL_BASE_URL}/UnrealEngine-1234/{self.pack_hash}'
        with mock.patch.object(RECOVERY.urllib.request, 'urlopen', return_value=response) as fetch:
            result = self.restore()
        self.assertEqual(result['download_bytes'], len(self.compressed))
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual(fetch.call_args.args[0],
                         f'{RECOVERY.OFFICIAL_BASE_URL}/UnrealEngine-1234/{self.pack_hash}')
        self.assertEqual(self.cache_file.read_bytes(), self.compressed)

    def test_untrusted_origin_and_oversize_manifest_cannot_trigger_download(self):
        for origin, size, message in [('https://example.com/dependencies', '100', 'official HTTPS'),
                                      (RECOVERY.OFFICIAL_BASE_URL, str(RECOVERY.MAX_COMPRESSED_PACK + 1), 'size limit')]:
            with self.subTest(origin=origin):
                tree = ET.parse(self.manifest)
                tree.getroot().set('BaseUrl', origin)
                tree.getroot().find('Packs')[0].set('CompressedSize', size)
                tree.write(self.manifest)
                with mock.patch.object(RECOVERY.urllib.request, 'urlopen', side_effect=AssertionError('unexpected network')):
                    with self.assertRaisesRegex(ValueError, message):
                        self.restore()
        tree = ET.parse(self.manifest)
        tree.getroot().set('BaseUrl', RECOVERY.OFFICIAL_BASE_URL)
        tree.getroot().find('Packs')[0].set('CompressedSize', str(len(self.compressed)))
        tree.write(self.manifest)
        self.cache_file.unlink()
        redirected = io.BytesIO(self.compressed)
        redirected.geturl = lambda: 'https://example.com/redirected-pack'
        with mock.patch.object(RECOVERY.urllib.request, 'urlopen', return_value=redirected):
            with self.assertRaisesRegex(ValueError, 'redirected'):
                self.restore()
        self.assertFalse((self.engine / RECOVERY.REQUIRED_FILES[0]).exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
