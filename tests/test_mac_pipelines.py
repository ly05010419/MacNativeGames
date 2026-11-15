"""The Mac app's pipeline archive made while packaging (build_app.py mac_pipeline_files, 2026-10-29): the pipeline-archive
mode's files go into the app's seed under the names totk_pipeline_archive.h par_load_prebuilt reads."""
import json
import re
from pathlib import Path
import tempfile
import unittest
import build_app

HEADER = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_pipeline_archive.h'


class MacPipelineFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.made, self.seed = root / 'pipelines', root / 'seed'
        self.made.mkdir(); self.seed.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_files_index_and_stamp_as_the_runtime_reads_them(self):
        (self.made / 'aa.metalarchive').write_bytes(b'archive a')
        (self.made / 'aa.keys').write_text('key1\nkey2\n')
        (self.made / 'bb.metalarchive').write_bytes(b'archive b')
        (self.made / 'bb.keys').write_text('key3\n')
        (self.made / 'cc.metalarchive').write_bytes(b'no keys: a file still being written')
        (self.seed / 'pipelines-applegpu_old-zz.metalarchive').write_bytes(b'last build')
        (self.seed / 'pipelines-applegpu_old.tsv').write_text('old\tzz\n')
        n = build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'totk-native-v9-unverified')
        self.assertEqual(n, 3)
        self.assertEqual(sorted(p.name for p in self.seed.iterdir()),
                         ['pipelines-applegpu_g16s-aa.metalarchive', 'pipelines-applegpu_g16s-bb.metalarchive',
                          'pipelines-applegpu_g16s.json', 'pipelines-applegpu_g16s.tsv'])
        self.assertEqual((self.seed / 'pipelines-applegpu_g16s-aa.metalarchive').read_bytes(), b'archive a')
        self.assertEqual((self.seed / 'pipelines-applegpu_g16s.tsv').read_text(), 'key1\taa\nkey2\taa\nkey3\tbb\n')
        stamp = json.loads((self.seed / 'pipelines-applegpu_g16s.json').read_text())
        self.assertEqual(stamp, {'schema': build_app.PAR_SCHEMA, 'chain': 'totk-native-v9-unverified', 'arch': 'applegpu_g16s',
                                 'pipelines': 3})

    def test_a_key_written_again_belongs_to_its_newest_file_only(self):
        # 2026-11-15: the tool wrote 531 pipelines again into new files (par_stale) while their old files kept other keys.
        # The index listed such a key twice, the old file first by name; the game takes a key's first row
        # (par_load_prebuilt), so it used the stale pipeline and Metal refused it. The index now gives each key the file
        # the tool itself takes (par_pub_scan: the key list's time, then the name; the later wins).
        import os
        (self.made / 'aa.metalarchive').write_bytes(b'old batch')
        (self.made / 'aa.keys').write_text('key1\nkey2\n')
        (self.made / 'bb.metalarchive').write_bytes(b'only superseded keys')
        (self.made / 'bb.keys').write_text('key2\n')
        (self.made / 'zz.metalarchive').write_bytes(b'new batch')
        (self.made / 'zz.keys').write_text('key2\nkey3\n')
        for name, when in (('aa', 1000), ('bb', 1500), ('zz', 2000)):
            os.utime(self.made / f'{name}.keys', (when, when))
        n = build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'c')
        index = [line.split('\t') for line in (self.seed / 'pipelines-applegpu_g16s.tsv').read_text().splitlines()]
        self.assertEqual(dict(index), {'key1': 'aa', 'key2': 'zz', 'key3': 'zz'})
        self.assertEqual(len(index), 3)   # one row a key
        self.assertEqual(n, 3)
        self.assertFalse((self.seed / 'pipelines-applegpu_g16s-bb.metalarchive').exists())   # it holds no key any more

    def test_the_runtime_reports_a_key_listed_twice(self):
        self.assertIn('重复', HEADER.read_text())   # par_load_prebuilt: a duplicate key in the App's index is red

    def test_nothing_made_leaves_no_index(self):
        (self.seed / 'pipelines-applegpu_g16s.json').write_text('{}')
        self.assertEqual(build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'c'), 0)
        self.assertEqual(list(self.seed.iterdir()), [])

    def test_schema_is_the_runtime_s(self):
        self.assertEqual(int(re.search(r'#define PAR_SCHEMA (\d+)', HEADER.read_text()).group(1)), build_app.PAR_SCHEMA)


if __name__ == '__main__':
    unittest.main()
