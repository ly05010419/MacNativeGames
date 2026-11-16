import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from language_catalogue import language_patch


class LanguageCatalogueTests(unittest.TestCase):
    def patch(self, title, chinese=True):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'romfs.bin'
            original = b'prefix' + title + b'original Chinese resources'
            image.write_bytes(original)
            entries = [{'path': 'title.dat', 'offset': 6, 'size': len(title)}]
            if chinese:
                entries += [{'path': name, 'offset': 6 + len(title), 'size': 1}
                            for name in ('Content/Global/Loc/Chinese.zpbn',
                                         'Content/Global/Loc/ChineseTrad.zpbn')]
            result = language_patch(image, entries)
            self.assertEqual(image.read_bytes(), original)
            return result

    def test_enables_both_original_chinese_resources_without_changing_default(self):
        original = b'en\r\nen|fr|ja|de|sp|it|pt-br\r\n'
        patch = self.patch(original)
        title = patch['title.dat']
        self.assertEqual(title, b'en\r\nen|fr|ja|de|sp|it|pt-br|cn|zh-hant\r\n')
        name, size, sha, source = patch['manifest.tsv'].decode().strip().split('\t')
        self.assertEqual((name, int(size)), ('title.dat', len(title)))
        self.assertEqual(sha, hashlib.sha256(title).hexdigest())
        self.assertEqual(source, hashlib.sha256(original).hexdigest())

    def test_existing_chinese_entries_and_default_are_preserved(self):
        title = b'cn\ncn|en|zh-hant\n'
        self.assertEqual(self.patch(title)['title.dat'], title)

    def test_missing_resources_and_malformed_catalogues_fail(self):
        with self.assertRaisesRegex(ValueError, 'resource missing'):
            self.patch(b'en\nen\n', chinese=False)
        for title in (b'en\n', b'en\nfr\n', b'en\nen||fr\n', b'en\nen'):
            with self.subTest(title=title), self.assertRaises(ValueError):
                self.patch(title)


if __name__ == '__main__':
    unittest.main()
