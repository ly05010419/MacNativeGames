import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from prepare_resources import prepare


class ResourcePreparationTests(unittest.TestCase):
    def test_index_matches_verified_image_and_rejects_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'romfs'
            romfs.mkdir()
            image = b'one\0two\0'
            (romfs / 'romfs.bin').write_bytes(image)
            manifest = {'status': 'pass', 'romfs_size': len(image),
                        'romfs_sha256': hashlib.sha256(image).hexdigest(),
                        'files': [{'path': 'a', 'offset': 0, 'size': 3},
                                  {'path': 'b', 'offset': 4, 'size': 3}]}
            (romfs / 'index.json').write_text(json.dumps(manifest))
            result = prepare(romfs, root / 'out')
            self.assertEqual(result['files'], 2)
            self.assertEqual(hashlib.sha256((root / 'out/runtime-index.bin').read_bytes()).hexdigest(),
                             result['index_sha256'])
            self.assertIn(manifest['romfs_sha256'], (root / 'out/romfs_config.h').read_text())
            with self.assertRaisesRegex(ValueError, 'exists'):
                prepare(romfs, root / 'out')
            (romfs / 'romfs.bin').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'differs'):
                prepare(romfs, root / 'other')

    def test_unsafe_or_unsorted_paths_fail_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root / 'romfs'
            romfs.mkdir()
            (romfs / 'romfs.bin').write_bytes(b'abc')
            manifest = {'status': 'pass', 'romfs_size': 3,
                        'romfs_sha256': hashlib.sha256(b'abc').hexdigest(),
                        'files': [{'path': '../escape', 'offset': 0, 'size': 1}]}
            (romfs / 'index.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'invalid'):
                prepare(romfs, root / 'out')
            self.assertFalse((root / 'out').exists())
            manifest['files'] = [{'path': 'b', 'offset': 0, 'size': 1},
                                 {'path': 'a', 'offset': 1, 'size': 1}]
            (romfs / 'index.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'unsorted'):
                prepare(romfs, root / 'out')

    def test_bad_font_manifest_does_not_leave_partial_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs, fonts = root / 'romfs', root / 'fonts'
            romfs.mkdir()
            fonts.mkdir()
            (romfs / 'romfs.bin').write_bytes(b'a')
            (romfs / 'index.json').write_text(json.dumps({
                'status': 'pass', 'romfs_size': 1,
                'romfs_sha256': hashlib.sha256(b'a').hexdigest(),
                'files': [{'path': 'a', 'offset': 0, 'size': 1}]}))
            (fonts / 'manifest.json').write_text(json.dumps({
                'status': 'pass', 'fonts': [{'type': i, 'name': '../escape'} for i in range(6)]}))
            with self.assertRaisesRegex(ValueError, 'unsafe shared font name'):
                prepare(romfs, root / 'out', fonts)
            self.assertFalse((root / 'out').exists())

    def test_font_header_uses_bundle_relative_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs, fonts = root / 'romfs', root / 'private-font-input'
            romfs.mkdir()
            fonts.mkdir()
            (romfs / 'romfs.bin').write_bytes(b'a')
            (romfs / 'index.json').write_text(json.dumps({
                'status': 'pass', 'romfs_size': 1,
                'romfs_sha256': hashlib.sha256(b'a').hexdigest(),
                'files': [{'path': 'a', 'offset': 0, 'size': 1}]}))
            rows = []
            for i in range(6):
                name = f'{i}-test.bfttf'
                data = b'12345678'
                (fonts / name).write_bytes(data)
                rows.append({'type': i, 'name': 'test', 'bfttf_bytes': 8,
                             'font_bytes': 0, 'sha256': hashlib.sha256(data).hexdigest()})
            (fonts / 'manifest.json').write_text(json.dumps({'status': 'pass', 'fonts': rows}))
            prepare(romfs, root / 'out', fonts)
            header = (root / 'out/shared_fonts_config.h').read_text()
            self.assertIn('shared-fonts/0-test.bfttf', header)
            self.assertNotIn(str(root), header)


if __name__ == '__main__':
    unittest.main()
