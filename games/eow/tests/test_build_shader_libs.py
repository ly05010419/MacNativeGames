"""build_shader_libs.py --seed / --check without Metal: a stand-in compiler that fails on sources marked FAIL."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('eow_build_shader_libs', HERE / 'scripts/build_shader_libs.py')
LIBS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIBS)

FAKE_METAL = '''#!{python}
import sys
if '--version' in sys.argv:
    print('Fake metal version 1'); sys.exit(0)
src, out = sys.argv[-1], sys.argv[sys.argv.index('-o') + 1]
data = open(src, 'rb').read()
if b'FAIL' in data:
    sys.stderr.write('error: fake'); sys.exit(1)
open(out, 'wb').write(b'lib:' + data)
'''


class SeedLibraryTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.metal = self.work / 'metal'
        self.metal.write_text(FAKE_METAL.format(python=sys.executable))
        self.metal.chmod(0o755)
        self.seed = self.work / 'build/shader-seed'
        self.seed.mkdir(parents=True)
        self.libs = self.work / 'build/shader-libs'

    def write_seed(self, sources):
        for p in self.seed.iterdir():
            p.unlink()
        files = {}
        for i, text in enumerate(sources):
            name = f'{i:064x}-000000000000_b.metal'
            (self.seed / name).write_text(text)
            files[name] = hashlib.sha256(text.encode()).hexdigest()
        (self.seed / 'seed-manifest.json').write_text(json.dumps(dict(files=files)))

    def run_libs(self, *mode):
        return LIBS.main(['--work', str(self.work), '--jobs', '2', '--metal', str(self.metal), *mode])

    def state(self):
        files = sorted(p for p in self.libs.rglob('*') if p.is_file())
        return {p.relative_to(self.libs).as_posix(): p.read_bytes() for p in files}

    def test_every_seed_source_gets_its_library(self):
        self.write_seed(['a', 'b', 'a'])
        self.run_libs('--seed')
        for text in ('a', 'b'):
            self.assertTrue((self.libs / f'{hashlib.sha256(text.encode()).hexdigest()}.metallib').is_file())
        self.assertEqual((self.libs / 'flags.txt').read_text(), ' '.join(LIBS.FLAGS) + '\n')
        self.run_libs('--check')
        self.assertFalse((self.work / 'build/shader-libs.partial').exists())

    def test_a_failed_rebuild_leaves_the_published_libraries(self):
        self.write_seed(['a', 'b'])
        (self.libs / 'src').mkdir(parents=True)
        (self.libs / 'src/played.metal').write_text('p')
        self.run_libs('--seed')
        before = self.state()
        self.write_seed(['a', 'c FAIL'])
        with self.assertRaises(SystemExit) as failed:
            self.run_libs('--seed')
        self.assertIn('unchanged', str(failed.exception.code))
        self.assertEqual(self.state(), before)
        self.assertFalse((self.work / 'build/shader-libs.partial').exists())
        with self.assertRaises(SystemExit) as stale:   # the published libraries were built for the earlier seed
            self.run_libs('--check')
        self.assertEqual(stale.exception.code, 1)

    def test_kept_libraries_and_played_sources_survive_a_rebuild(self):
        self.write_seed(['a'])
        self.run_libs('--seed')
        (self.libs / 'src').mkdir()
        (self.libs / 'src/played.metal').write_text('p')
        kept = self.libs / f'{hashlib.sha256(b"a").hexdigest()}.metallib'
        inode = kept.stat().st_ino
        self.write_seed(['a', 'b'])
        self.run_libs('--seed')
        self.assertEqual(kept.stat().st_ino, inode)
        self.assertEqual((self.libs / 'src/played.metal').read_text(), 'p')
        self.run_libs('--check')

    def test_a_seed_that_is_not_its_manifest_is_refused(self):
        self.write_seed(['a', 'b'])
        (self.seed / f'{0:064x}-000000000000_b.metal').write_text('changed')
        with self.assertRaises(SystemExit):
            self.run_libs('--seed')
        self.assertFalse(self.libs.exists())

    def test_check_names_a_missing_library(self):
        self.write_seed(['a', 'b'])
        self.run_libs('--seed')
        (self.libs / f'{hashlib.sha256(b"b").hexdigest()}.metallib').unlink()
        with self.assertRaises(SystemExit):
            self.run_libs('--check')


if __name__ == '__main__':
    unittest.main()
