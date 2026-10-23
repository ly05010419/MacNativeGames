"""Persistent libraries select current-game sources and publish only successful exact inputs."""
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/prepare_shader_libraries.py'
spec = importlib.util.spec_from_file_location('prepare_shader_libraries', SCRIPT)
libraries = importlib.util.module_from_spec(spec)
spec.loader.exec_module(libraries)


class ShaderLibraries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache, self.output = self.root/'cache', self.root/'libraries'
        self.cache.mkdir()
        self.inventory = self.root/'shader-keys.json'
        self.keys = ['a'*64, 'b'*64, 'c'*64]
        self.write_inventory()
        self.compiler = self.root/'metal'
        self.compiler.write_text('#!'+sys.executable+'\n'+'''import json, pathlib, sys
root = pathlib.Path(__file__).parent
if sys.argv[1:] == ['--version']:
    print((root/'version').read_text()); sys.exit(0)
with (root/'commands.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:])+'\\n')
source = pathlib.Path(sys.argv[-1]).read_bytes()
output = pathlib.Path(sys.argv[sys.argv.index('-o')+1])
output.write_bytes(b'broken' if b'FAIL' in source else b'MTLB'+source)
if b'FAIL' in source:
    print('synthetic compile failure', file=sys.stderr); sys.exit(1)
''')
        self.compiler.chmod(0o755)
        (self.root/'version').write_text('synthetic Metal 1.0')

    def write_inventory(self, **updates):
        value = dict(schema=1, chain=libraries.CHAIN, version='v121',
                     source_romfs_sha256='d'*64, keys=self.keys, stages={self.keys[0]: 'vs'})
        value.update(updates)
        self.inventory.write_text(json.dumps(value))

    def pair(self, key=None, suffix='1'*12, data=b'// shader', stage='vs', **updates):
        name = (key or self.keys[0])+'-'+suffix+'_b'
        path = self.cache/(name+'.metal')
        path.write_bytes(data)
        reflection = dict(key=name, chain=libraries.CHAIN, stage=stage,
                          msl_entry_kind=libraries.KINDS[stage])
        reflection.update(updates)
        path.with_suffix('.json').write_text(json.dumps(reflection))
        return path

    def run_preparation(self, **options):
        return libraries.prepare(self.cache, self.inventory, self.output, compiler=self.compiler, **options)

    def manifest(self):
        return json.loads((self.output/'manifest.json').read_text())

    def test_current_game_filter_deduplication_and_incremental_reuse(self):
        first = self.pair(data=b'// shared')
        self.pair(key=self.keys[1], data=b'// shared')
        self.pair(key='f'*64, data=b'// previous game version')
        source_files = {p.name: p.read_bytes() for p in self.cache.iterdir()}
        report = self.run_preparation(jobs=2)
        self.assertEqual((report['selected_pairs'], report['unique_sources'], report['unrelated_sources']), (2, 1, 1))
        self.assertEqual(report['compiled_sources'], 1)
        manifest = self.manifest()
        sha = hashlib.sha256(first.read_bytes()).hexdigest()
        self.assertEqual(manifest['options']['flags'], ['-fmetal-math-mode=safe'])
        self.assertEqual(manifest['libraries'], {sha: f"{manifest['directory']}/{sha}.metallib"})
        self.assertEqual((self.output/manifest['libraries'][sha]).read_bytes(), b'MTLB// shared')
        commands = [json.loads(row) for row in (self.root/'commands.jsonl').read_text().splitlines()]
        self.assertEqual(len(commands), 1)
        self.assertIn('-fmetal-math-mode=safe', commands[0])
        self.assertIn('-fmodules-cache-path='+str(self.output.resolve()/manifest['directory']/'module-cache'), commands[0])
        again = self.run_preparation()
        self.assertEqual((again['compiled_sources'], again['reused_sources']), (0, 1))
        self.assertEqual({p.name: p.read_bytes() for p in self.cache.iterdir()}, source_files)

    def test_only_current_game_observations_extend_the_inventory(self):
        self.pair(key='f'*64)
        observed = self.cache/'observed'/('d'*64+'.keys')
        observed.parent.mkdir()
        identity = {k: v for k, v in json.loads(self.inventory.read_text()).items() if k != 'keys' and k != 'stages'}
        for field, wrong in [('version', 'v0'), ('chain', 'obsolete'), ('source_romfs_sha256', 'e'*64)]:
            observed.write_text(json.dumps({**identity, field: wrong})+'\n'+'f'*64+'\n')
            report = self.run_preparation(dry_run=True)
            self.assertEqual((report['selected_pairs'], report['observed_keys']), (0, 0))
        observed.write_text(json.dumps(identity)+'\n'+'f'*64+'\n'+'bad\n')
        report = self.run_preparation()
        self.assertEqual((report['selected_pairs'], report['observed_keys'], report['compiled_sources']), (1, 1, 1))

    def test_incomplete_or_wrong_identity_pairs_are_never_compiled(self):
        self.pair(chain='obsolete-translator')
        self.pair(suffix='2'*12, stage='ps')  # the verified inventory says this raw key is a vertex shader
        self.pair(suffix='3'*12, msl_entry_kind='fragment')
        missing = self.pair(suffix='4'*12)
        missing.with_suffix('.json').unlink()
        wrong = self.pair(suffix='5'*12)
        reflection = json.loads(wrong.with_suffix('.json').read_text())
        reflection['key'] = 'another-key'
        wrong.with_suffix('.json').write_text(json.dumps(reflection))
        report = self.run_preparation()
        self.assertEqual(report['compiled_sources'], 0)
        self.assertEqual(len(report['invalid_pairs']), 5)
        self.assertEqual(self.manifest()['libraries'], {})
        self.assertFalse((self.root/'commands.jsonl').exists())

    def test_dry_run_and_bundled_sources_do_not_compile(self):
        path = self.pair()
        bundled = self.root/'bundled'
        bundled.mkdir()
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        (bundled/(sha+'.metallib')).write_bytes(b'MTLBbundled')
        report = self.run_preparation(app_libraries=bundled, dry_run=True)
        self.assertEqual((report['bundled_sources'], report['pending_sources']), (1, 0))
        self.assertFalse(self.output.exists())
        self.assertFalse((self.root/'commands.jsonl').exists())
        report = self.run_preparation(app_libraries=bundled)
        self.assertEqual(report['compiled_sources'], 0)
        self.assertEqual(self.manifest()['libraries'], {})

    def test_source_changes_get_new_hashes_and_keep_existing_library(self):
        source = self.pair(data=b'// first')
        self.run_preparation()
        first = self.manifest()
        source.write_bytes(b'// changed')
        report = self.run_preparation()
        self.assertEqual(report['compiled_sources'], 1)
        self.assertEqual(len(self.manifest()['libraries']), 2)
        self.assertTrue(set(first['libraries']).issubset(self.manifest()['libraries']))

    def test_compiler_identity_isolation_preserves_old_outputs(self):
        self.pair()
        self.run_preparation()
        old = self.manifest()
        (self.root/'version').write_text('synthetic Metal 2.0')
        report = self.run_preparation()
        current = self.manifest()
        self.assertEqual(report['compiled_sources'], 1)
        self.assertNotEqual(old['directory'], current['directory'])
        self.assertTrue(all(p.startswith(current['directory']+'/') for p in current['libraries'].values()))
        self.assertTrue(all((self.output/p).is_file() for p in old['libraries'].values()))

    def test_failed_outputs_never_enter_manifest_or_replace_valid_library(self):
        self.pair(data=b'// valid')
        self.run_preparation()
        original = self.manifest()
        self.pair(key=self.keys[1], data=b'FAIL')
        self.pair(key=self.keys[2], data=b'// also valid')
        report = self.run_preparation(jobs=2)
        self.assertEqual((report['compiled_sources'], len(report['failures'])), (1, 1))
        current = self.manifest()
        self.assertEqual(len(current['libraries']), 2)
        self.assertTrue(set(original['libraries']).issubset(current['libraries']))
        failed_sha = hashlib.sha256(b'FAIL').hexdigest()
        self.assertNotIn(failed_sha, current['libraries'])
        self.assertFalse((self.output/current['directory']/(failed_sha+'.metallib')).exists())
        self.assertEqual(list((self.output/current['directory']).glob('.compile-*')), [])
        for path in current['libraries'].values():
            self.assertTrue((self.output/path).read_bytes().startswith(b'MTLB'))

    def test_manifest_paths_are_restricted_and_previous_manifest_is_not_overwritten(self):
        self.pair()
        self.run_preparation()
        bad = self.manifest()
        sha = next(iter(bad['libraries']))
        bad['libraries'][sha] = '../outside.metallib'
        (self.output/'manifest.json').write_text(json.dumps(bad))
        with self.assertRaisesRegex(ValueError, 'unsafe path'):
            self.run_preparation()
        self.assertEqual(self.manifest(), bad)

    def test_parallel_writer_is_rejected(self):
        self.output.mkdir()
        with (self.output/'.prepare.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, 'another shader library preparation'):
                self.run_preparation()
        self.assertFalse((self.output/'manifest.json').exists())

    def test_invalid_inventory_stops_before_any_output(self):
        for changes in [dict(schema=2), dict(chain='old'), dict(keys=self.keys[::-1]),
                        dict(keys=[self.keys[0]]*2), dict(source_romfs_sha256='../unknown')]:
            with self.subTest(changes=changes):
                self.write_inventory(**changes)
                with self.assertRaises(ValueError):
                    self.run_preparation()
                self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
