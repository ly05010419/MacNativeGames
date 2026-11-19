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

    def test_metal4_files_join_the_index_and_take_their_keys_when_newer(self):
        # 2026-11-17 (nvn_pso4.h): Ocarina of Time on the Mac writes Metal 4 files (GPU code only, up to 2,000 pipelines a
        # file) beside the old folder, in pipelines-mtl4/. The App carries them as pipelines-<arch>-m4-<name>.mtl4a, the
        # m4- name telling the runtime which API opens them; a key in both kinds belongs to the newer file.
        import os
        m4 = self.made.parent / 'pipelines-mtl4'; m4.mkdir()
        (self.made / 'aa.metalarchive').write_bytes(b'old batch')
        (self.made / 'aa.keys').write_text('key1\nkey2\n')
        (m4 / 'key2-2000.mtl4a').write_bytes(b'metal 4 period')
        (m4 / 'key2-2000.keys').write_text('key2\nkey3\n')
        (m4 / 'cut.mtl4a').write_bytes(b'no keys: a write cut short')
        os.utime(self.made / 'aa.keys', (1000, 1000)); os.utime(m4 / 'key2-2000.keys', (2000, 2000))
        n = build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'c')
        index = dict(line.split('\t') for line in (self.seed / 'pipelines-applegpu_g16s.tsv').read_text().splitlines())
        self.assertEqual(index, {'key1': 'aa', 'key2': 'm4-key2-2000', 'key3': 'm4-key2-2000'})
        self.assertEqual(n, 3)
        self.assertEqual((self.seed / 'pipelines-applegpu_g16s-m4-key2-2000.mtl4a').read_bytes(), b'metal 4 period')
        self.assertFalse(any(p.name.startswith('pipelines-applegpu_g16s-m4-cut') for p in self.seed.iterdir()))
        # the stamp tells a file the tool did not write again by its time, for both kinds (pipeline_files_stamp)
        before = build_app.pipeline_files_stamp(self.seed)
        os.utime(self.seed / 'pipelines-applegpu_g16s-m4-key2-2000.mtl4a', (3000, 3000))
        self.assertNotEqual(build_app.pipeline_files_stamp(self.seed), before)

    def test_the_runtime_opens_m4_names_with_metal4_only(self):
        nvn = HEADER.parent
        pso4 = (nvn / 'nvn_pso4.h').read_text()
        self.assertIn('[name hasPrefix:@"m4-"]', pso4)                       # the App's names (par4_name)
        self.assertIn('stringByAppendingFormat:@"%@.mtl4a", name]', pso4)    # pipelines-<arch>-m4-<name>.mtl4a
        self.assertIn('if (name && par4_name(name))', HEADER.read_text())    # the old API never opens them
        self.assertIn('TOTK_SHADER_DEVICE_LIBS', pso4)                       # an iPad build's run keeps the old files

    def test_the_runtime_reports_a_key_listed_twice(self):
        self.assertIn('重复', HEADER.read_text())   # par_load_prebuilt: a duplicate key in the App's index is red

    def test_nothing_made_leaves_no_index(self):
        (self.seed / 'pipelines-applegpu_g16s.json').write_text('{}')
        self.assertEqual(build_app.mac_pipeline_files(self.made, self.seed, 'applegpu_g16s', 'c'), 0)
        self.assertEqual(list(self.seed.iterdir()), [])

    def test_schema_is_the_runtime_s(self):
        self.assertEqual(int(re.search(r'#define PAR_SCHEMA (\d+)', HEADER.read_text()).group(1)), build_app.PAR_SCHEMA)


class MacPipelinesStamp(unittest.TestCase):
    """The archive is checked again when the compiled libraries the tool loads change (build/shader-libs), not only the
    lists, the seed or the binary: one made while most shaders were compiled from source may miss for the offline
    libraries (totk_pipeline_archive.h: "a wrong file is a miss")."""
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.tree, self.logs, self.res = root / 'tree', root / 'logs', root / 'Game.app/Contents/Resources/totk'
        self.build = self.res / 'build'
        (self.build / 'shader-seed').mkdir(parents=True)
        (self.build / 'shader-seed/seed-manifest.json').write_text('{"chain": "c"}')
        (self.build / 'pso_manifest.tsv').write_text('R\tkey\n')
        (self.build / 'totk-native').write_bytes(b'binary')
        (self.build / 'shader-libs').mkdir()

    def stamp(self):
        """The stamp mac_pipelines writes, and whether the tool was asked to check the last archive (TOTK_PAR_RECHECK)."""
        import contextlib
        from unittest import mock
        with mock.patch.object(build_app, 'run_outside_bundle', return_value=contextlib.nullcontext('tool')), \
                mock.patch.object(build_app, 'run') as ran, mock.patch.object(build_app, 'last_json', return_value={}), \
                mock.patch.object(build_app, 'say'):
            build_app.mac_pipelines(self.tree, self.res, 'v143', self.logs)
        recheck = ran.call_args.kwargs['env']['TOTK_PAR_RECHECK']
        return (self.tree / 'build/mac-pipelines/v143/inputs.txt').read_text().split()[0], recheck

    def test_a_new_library_set_checks_the_archive_again(self):
        import hashlib
        none, _ = self.stamp()
        inputs = [self.build / 'shader-seed/seed-manifest.json', self.build / 'pso_manifest.tsv', self.build / 'totk-native']
        self.assertEqual(none, hashlib.sha256(b''.join(p.read_bytes() for p in inputs)).hexdigest(),
                         'an app without libraries keeps the stamp it had')
        self.assertEqual(self.stamp(), (none, '0'))
        (self.build / 'shader-libs' / ('a' * 64 + '.metallib')).write_bytes(b'lib')
        one, recheck = self.stamp()
        self.assertNotEqual(one, none)
        self.assertEqual(recheck, '1')
        self.assertEqual(self.stamp(), (one, '0'))
        (self.build / 'shader-libs' / ('b' * 64 + '.metallib')).write_bytes(b'lib')
        self.assertNotEqual(self.stamp()[0], one)


if __name__ == '__main__':
    unittest.main()
