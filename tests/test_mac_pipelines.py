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
        n = build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'totk-native-v8-unverified')
        self.assertEqual(n, 3)
        self.assertEqual(sorted(p.name for p in self.seed.iterdir()),
                         ['pipelines-applegpu_g16s-aa.metalarchive', 'pipelines-applegpu_g16s-bb.metalarchive',
                          'pipelines-applegpu_g16s.json', 'pipelines-applegpu_g16s.tsv'])
        self.assertEqual((self.seed / 'pipelines-applegpu_g16s-aa.metalarchive').read_bytes(), b'archive a')
        self.assertEqual((self.seed / 'pipelines-applegpu_g16s.tsv').read_text(), 'key1\taa\nkey2\taa\nkey3\tbb\n')
        stamp = json.loads((self.seed / 'pipelines-applegpu_g16s.json').read_text())
        self.assertEqual(stamp, {'schema': build_app.PAR_SCHEMA, 'chain': 'totk-native-v8-unverified', 'arch': 'applegpu_g16s',
                                 'pipelines': 3})

    def test_nothing_made_leaves_no_index(self):
        (self.seed / 'pipelines-applegpu_g16s.json').write_text('{}')
        self.assertEqual(build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'c'), 0)
        self.assertEqual(list(self.seed.iterdir()), [])

    def test_schema_is_the_runtime_s(self):
        self.assertEqual(int(re.search(r'#define PAR_SCHEMA (\d+)', HEADER.read_text()).group(1)), build_app.PAR_SCHEMA)


if __name__ == '__main__':
    unittest.main()
