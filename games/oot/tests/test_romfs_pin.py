"""The RomFS check goes file by file (#2009, #2011): two retail dumps of update 131072 hold the same files but pack to
different images, so the build checks every listed file's size and hash, not the hash of one image."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('oot_romfs_pin', GAME / 'scripts/romfs_pin.py')
pin = importlib.util.module_from_spec(spec); spec.loader.exec_module(pin)


def image(folder, files, **index_fields):
    """romfs.bin + index.json as the importers write them (16-byte aligned, sorted by path), without per-file hashes;
    index_fields: what an importer adds (import_nup.py: the NUP's own per-file check)."""
    rows, data = [], b''
    for path, payload in sorted(files.items()):
        data += bytes(-len(data) % 16)
        rows.append(dict(path=path, offset=len(data), size=len(payload)))
        data += payload
    (folder / 'romfs.bin').write_bytes(data)
    (folder / 'index.json').write_text(json.dumps(dict(status='pass', romfs_size=len(data),
                                                      romfs_sha256=hashlib.sha256(data).hexdigest(), files=rows, **index_fields)))


class RomfsPinTests(unittest.TestCase):
    FILES = {'Pack/a.pack': b'a' * 40, 'Sound/b.bwav': b'bb' * 9, 'Tex/c.bntx': b'c'}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name); self.romfs = self.dir / 'romfs'; self.romfs.mkdir()
        self.pin = self.dir / 'pin.tsv.gz'
        reference = self.dir / 'reference'; reference.mkdir()
        image(reference, self.FILES)
        pin.write_pin(reference, self.pin)

    def test_the_pin_is_made_from_an_image_and_names_every_file(self):
        rows = gzip.decompress(self.pin.read_bytes()).decode().splitlines()
        self.assertEqual([r.split('\t')[0] for r in rows], sorted(self.FILES))
        self.assertEqual(rows[0].split('\t')[1:], ['40', hashlib.sha256(b'a' * 40).hexdigest()[:16]])

    def test_the_same_files_in_another_layout_pass(self):
        image(self.romfs, self.FILES)
        index = json.loads((self.romfs / 'index.json').read_text())
        index['files'] = [dict(r, offset=r['offset']) for r in index['files']]
        report = pin.check(self.romfs, self.pin)
        self.assertEqual((report['files'], report['extra']), (3, []))

    def test_extra_files_pass_and_are_named(self):
        image(self.romfs, {**self.FILES, 'Extra/new.bin': b'x' * 2112})
        report = pin.check(self.romfs, self.pin)
        self.assertEqual(report['extra'], ['Extra/new.bin'])

    def test_a_missing_or_changed_file_fails_and_is_named(self):
        image(self.romfs, {k: v for k, v in self.FILES.items() if k != 'Tex/c.bntx'})
        with self.assertRaisesRegex(ValueError, 'missing 1.*Tex/c.bntx'):
            pin.check(self.romfs, self.pin)
        image(self.romfs, {**self.FILES, 'Sound/b.bwav': b'bc' * 9})
        with self.assertRaisesRegex(ValueError, 'different 1.*Sound/b.bwav'):
            pin.check(self.romfs, self.pin)

    def test_a_dumped_folder_with_a_changed_file_says_it_cannot_be_checked(self):
        # a folder carries no hashes of its own, so a changed file cannot be told from a damaged one
        image(self.romfs, {**self.FILES, 'Sound/b.bwav': b'bc' * 9})
        with self.assertRaisesRegex(ValueError, r'different 1 \(Sound/b.bwav\).*\.nup'):
            pin.check(self.romfs, self.pin)

    def test_a_retail_variant_the_package_itself_verified_passes_and_is_named(self):
        # #2011 (11-11): another retail NUP of 131072 holds 9 files of the same size with other bytes, each matching the
        # SHA-256 its own table of contents records (import_nup.py), and the game plays
        image(self.romfs, {**self.FILES, 'Sound/b.bwav': b'bc' * 9}, files_verified_by_source=True)
        report = pin.check(self.romfs, self.pin)
        self.assertEqual(report['variant'], ['Sound/b.bwav'])
        self.assertEqual(report['files'], 3)

    def test_an_older_nup_import_counts_as_verified_by_its_record(self):
        # an index.json imported before the flag existed: its integrity line says what was checked
        image(self.romfs, {**self.FILES, 'Sound/b.bwav': b'bc' * 9},
              integrity='NUP TOC and every uncompressed file SHA256 verified')
        self.assertEqual(pin.check(self.romfs, self.pin)['variant'], ['Sound/b.bwav'])

    def test_a_verified_import_still_needs_every_file_at_its_size(self):
        image(self.romfs, {k: v for k, v in self.FILES.items() if k != 'Tex/c.bntx'}, files_verified_by_source=True)
        with self.assertRaisesRegex(ValueError, 'missing 1.*Tex/c.bntx'):
            pin.check(self.romfs, self.pin)
        image(self.romfs, {**self.FILES, 'Sound/b.bwav': b'bc' * 10}, files_verified_by_source=True)
        with self.assertRaisesRegex(ValueError, 'other size 1.*Sound/b.bwav'):
            pin.check(self.romfs, self.pin)

    def test_the_nup_import_records_its_check(self):
        source = (GAME / 'scripts/import_nup.py').read_text()
        self.assertIn('files_verified_by_source=True', source)

    def test_hashes_missing_from_the_index_are_read_from_the_image_once(self):
        image(self.romfs, self.FILES)
        pin.check(self.romfs, self.pin)
        rows = json.loads((self.romfs / 'index.json').read_text())['files']
        self.assertEqual(rows[0]['sha256'], hashlib.sha256(b'a' * 40).hexdigest())
        (self.romfs / 'romfs.bin').write_bytes(b'')   # the next check takes the recorded hashes, not the image
        self.assertEqual(pin.check(self.romfs, self.pin)['files'], 3)

    def test_the_repository_pin_lists_update_131072(self):
        rows = gzip.decompress((GAME / 'assets/romfs-v131072.tsv.gz').read_bytes()).decode().splitlines()
        self.assertEqual(len(rows), 186583)
        self.assertIn('Event/Movie/Dm_Kokiri_0000.webm', {r.split('\t')[0] for r in rows})


class BuildUsesTheImportedImageTests(unittest.TestCase):
    """build.py: the gate is the file check; the seed and the game-data marker name the image actually imported."""
    SOURCE = (GAME / 'build.py').read_text()

    def test_no_whole_image_pin_is_compared(self):
        self.assertNotIn("expected['romfs']", self.SOURCE)
        self.assertNotIn('romfs', json.loads((GAME / 'assets/expected-v131072.json').read_text()))

    def test_the_gate_is_the_file_check(self):
        self.assertIn('romfs_pin.check(', self.SOURCE)


class NupReaderTests(unittest.TestCase):
    def test_zstd_members_need_no_extra_package(self):
        # #2009: the vendored reader imported zstandard, which requirements.txt lacked; Python 3.14+ has it in the
        # standard library (compression.zstd), which build_app.py already uses
        source = (GAME / 'third_party/nup/nup/read.py').read_text()
        self.assertIn('from compression import zstd', source)


if __name__ == '__main__':
    unittest.main()
