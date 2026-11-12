"""prepare_shader_seed.py without game data: a stand-in translator service, shaders made up in the test."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('eow_prepare_shader_seed', HERE / 'scripts/prepare_shader_seed.py')
SEED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SEED)

# The stand-in service: answers like totk_xlate --service. Its reflection names texture slot 0 with the kind asked
# (2d by default), so a listed cube row needs its own variant. FAKE_FAIL: keys it fails; FAKE_DIE: keys it exits on;
# FAKE_MSL: text added to every MSL (FAIL makes the stand-in compiler fail).
FAKE_TRANSLATOR = '''#!{python}
import json, os, sys
out = sys.argv[sys.argv.index('--out') + 1]
fail, die = os.environ.get('FAKE_FAIL', '').split(','), os.environ.get('FAKE_DIE', '').split(',')
for line in sys.stdin:
    rq = json.loads(line)
    key = rq['variant'].split('-')[0]
    if key in die:
        sys.exit(3)
    if key in fail:
        print(json.dumps(dict(ok=False, error='fake error')), flush=True)
        continue
    deps = dict(tex={{'0': rq['tex'].get('0', '2d')}})
    msl = '// ' + key + ' ' + json.dumps(deps) + ' ' + os.environ.get('FAKE_MSL', '') + '\\n'
    open(os.path.join(out, rq['variant'] + '_b.metal'), 'w').write(msl)
    open(os.path.join(out, rq['variant'] + '_b.json'), 'w').write(json.dumps(dict(stage=rq['stage'], deps=deps)))
    print(json.dumps(dict(ok=True)), flush=True)
'''

FAKE_METAL = '''#!{python}
import os, sys
if '--version' in sys.argv:   # FAKE_METAL_VERSION: another compiler build (its libraries are not kept)
    print('Fake metal version ' + os.environ.get('FAKE_METAL_VERSION', '1')); sys.exit(0)
src, out = sys.argv[-1], sys.argv[sys.argv.index('-o') + 1]
data = open(src, 'rb').read()
if b'FAIL' in data:
    sys.exit(1)
open(out, 'wb').write(b'lib:' + data)
'''


def control(stage, length=0x40, c1=0x30):
    c = bytearray(0x880)
    struct.pack_into('<III', c, 0x700, c1, length, 0)
    struct.pack_into('<I', c, 0x714, {'vs': 0, 'ps': 1, 'cs': 5}[stage])
    return bytes(c)


def shader(seed_byte, stage='ps'):
    ctl, code = control(stage), bytes([seed_byte]) * 0x40
    return hashlib.sha256(ctl + code).hexdigest(), (stage, ctl, code, 'region_common/shader/test.bfsha')


class PrepareTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        tools = self.work / 'build/shader-service'
        tools.mkdir(parents=True)
        (tools / 'totk_xlate').write_text(FAKE_TRANSLATOR.format(python=sys.executable))
        (tools / 'totk_xlate').chmod(0o755)
        (self.work / 'build/spirv-cross').mkdir()
        (self.work / 'build/spirv-cross/spirv-cross').write_text('#!/bin/sh\n')
        (self.work / 'build/spirv-cross/spirv-cross').chmod(0o755)
        self.metal = self.work / 'metal'
        self.metal.write_text(FAKE_METAL.format(python=sys.executable))
        self.metal.chmod(0o755)
        self.libs = self.work / 'build/shader-libs'
        romfs = self.work / 'extracted/v110/romfs'
        romfs.mkdir(parents=True)
        (romfs / 'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256='0' * 64, files=[])))
        self.k1, s1 = shader(1)
        self.k2, s2 = shader(2, 'vs')
        self.shaders = {self.k1: s1, self.k2: s2}
        self.list = self.work / 'list.tsv'
        self.list.write_text(SEED.HEADER + f'V\t{self.k1}\t' + '{"tex":{"0":"cube"}}\n')
        self.output = self.work / 'build/shader-seed'
        for patch in (mock.patch.object(SEED, 'inventory', return_value=(self.shaders, [])),
                      mock.patch.object(SEED, 'census', return_value=dict(shaders=2, vs=1, ps=1, cs=0)),
                      mock.patch.object(SEED, 'EXCLUDED', {}),
                      mock.patch.dict(os.environ, FAKE_FAIL='', FAKE_DIE='', FAKE_MSL='', FAKE_METAL_VERSION='1')):
            patch.start()
            self.addCleanup(patch.stop)

    def prepare(self):
        return SEED.prepare(self.work, self.output, 2, self.list, metal=str(self.metal))

    def check_pair(self):
        """build_shader_libs.py --check: the published libraries serve the published seed."""
        SEED.libraries_module().main(['--work', str(self.work), '--metal', str(self.metal), '--check'])

    def pair_state(self):
        return {str(p.relative_to(self.work)): p.read_bytes() for d in (self.output, self.libs)
                for p in sorted(d.rglob('*')) if p.is_file()}

    def new_translator(self):
        """Another translator build: the translation cache is emptied (its identity names the translator)."""
        with open(self.work / 'build/shader-service/totk_xlate', 'a') as f:
            f.write('# another build\n')

    def test_publishes_a_complete_seed(self):
        manifest = self.prepare()
        self.assertEqual((manifest['shaders'], manifest['variants'], manifest['listed_jobs']), (2, 3, 1))
        names = set(p.name for p in self.output.iterdir()) - {'seed-manifest.json'}
        self.assertEqual(names, set(manifest['files']))
        safe = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json))$')
        for name, sha in manifest['files'].items():   # the runtime's import accepts every name and hash
            self.assertRegex(name, safe)
            self.assertEqual(hashlib.sha256((self.output / name).read_bytes()).hexdigest(), sha)
        rows = json.loads((self.output / f'{self.k1}.variants').read_text())
        self.assertEqual([r['deps']['tex']['0'] for r in rows], ['2d', 'cube'])
        self.assertEqual(rows[0]['variant'], SEED.variant_name(self.k1, SEED.ANSWERS))
        self.assertFalse(self.output.with_name('shader-seed.partial').exists())
        self.check_pair()

    def test_a_metal_failure_after_a_new_seed_keeps_the_previous_pair(self):
        self.prepare()
        before = self.pair_state()
        os.environ['FAKE_MSL'] = 'FAIL'   # a translator build whose MSL the compiler rejects: a different seed
        self.new_translator()
        with self.assertRaises(SEED.SeedError) as failed:
            self.prepare()
        self.assertIn('unchanged', str(failed.exception))
        self.assertEqual(self.pair_state(), before)
        for leftover in ('shader-seed.partial', 'shader-libs.partial'):
            self.assertFalse((self.work / 'build' / leftover).exists())
        self.check_pair()   # the previous pair is still usable

    def test_a_new_pair_replaces_both(self):
        self.prepare()
        old_seed, old_libs = self.output.stat().st_ino, self.libs.stat().st_ino
        os.environ['FAKE_MSL'] = 'v2'
        self.new_translator()
        self.prepare()
        self.assertNotEqual((self.output.stat().st_ino, self.libs.stat().st_ino), (old_seed, old_libs))
        self.check_pair()
        self.assertTrue(all('v2' in p.read_text() for p in self.output.glob('*_b.metal')))

    def new_seed(self, compiler='1'):
        """The next run makes a different seed (another translator build), optionally with another compiler."""
        os.environ['FAKE_MSL'] = 'v2'
        os.environ['FAKE_METAL_VERSION'] = compiler
        self.new_translator()

    def failing_seed_exchange(self, error):
        """build_shader_libs.exchange, failing when it publishes the seed (after the libraries were published)."""
        built = SEED.libraries_module()
        real = built.exchange

        def exchange(staging, target):
            if target.name == 'shader-seed':
                raise error
            return real(staging, target)
        return mock.patch.object(built, 'exchange', side_effect=exchange)

    def test_a_failed_seed_exchange_puts_the_old_libraries_back(self):
        for compiler in ('1', '2'):   # the same compiler (old libraries kept in the new folder) and another one
            with self.subTest(compiler=compiler):
                self.prepare()
                before = self.pair_state()
                self.new_seed(compiler)
                with self.failing_seed_exchange(OSError(5, 'test')), self.assertRaises(SEED.SeedError) as failed:
                    self.prepare()
                self.assertIn('unchanged', str(failed.exception))
                self.assertEqual(self.pair_state(), before)
                for leftover in ('shader-seed.partial', 'shader-libs.partial'):
                    self.assertFalse((self.work / 'build' / leftover).exists())
                os.environ['FAKE_METAL_VERSION'], os.environ['FAKE_MSL'] = '1', ''
                self.check_pair()
                self.new_translator()

    def test_an_interrupt_at_the_seed_exchange_puts_the_old_libraries_back(self):
        self.prepare()
        before = self.pair_state()
        self.new_seed()
        with self.failing_seed_exchange(KeyboardInterrupt()), self.assertRaises(KeyboardInterrupt):
            self.prepare()
        self.assertEqual(self.pair_state(), before)

    def test_an_interrupt_just_after_an_exchange_puts_the_old_pair_back(self):
        built = SEED.libraries_module()
        real = built.exchange
        for published in ('shader-libs', 'shader-seed'):   # the exchange done, the interrupt before the call returned
            with self.subTest(published=published):
                self.prepare()
                before = self.pair_state()
                self.new_seed()

                fired = []

                def exchange(staging, target, published=published):
                    real(staging, target)
                    if target.name == published and not fired:   # once: the rollback's own exchanges run
                        fired.append(1)
                        raise KeyboardInterrupt()
                with mock.patch.object(built, 'exchange', side_effect=exchange), self.assertRaises(KeyboardInterrupt):
                    self.prepare()
                self.assertEqual(self.pair_state(), before)
                for leftover in ('shader-seed.partial', 'shader-libs.partial'):
                    self.assertFalse((self.work / 'build' / leftover).exists())
                os.environ['FAKE_MSL'] = ''
                self.new_translator()

    def test_a_first_publish_interrupted_after_the_seed_exchange_leaves_nothing(self):
        built = SEED.libraries_module()
        real = built.exchange

        fired = []

        def exchange(staging, target):
            real(staging, target)
            if target.name == 'shader-seed' and not fired:
                fired.append(1)
                raise KeyboardInterrupt()
        with mock.patch.object(built, 'exchange', side_effect=exchange), self.assertRaises(KeyboardInterrupt):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.libs.exists())

    def test_what_a_kill_between_the_exchanges_leaves(self):
        """Only the libraries published (the process killed before the seed exchange): with the same compiler the new
        libraries serve the old seed; after a compiler change they lack its shaders and --check says so."""
        built = SEED.libraries_module()
        for compiler, served in (('1', True), ('2', False)):
            with self.subTest(compiler=compiler):
                self.prepare()
                self.new_seed(compiler)
                other = self.work / 'build/next-seed'
                SEED.prepare(self.work, other, 2, self.list, libs=False)
                staging, _ = built.stage_seed_libs(self.libs, other, 2, str(self.metal))
                built.exchange(staging, self.libs)
                old = built.seed_sources(self.output)
                self.assertEqual(all((self.libs / f'{d}.metallib').is_file() for d in old), served)
                with self.assertRaises(SystemExit):
                    self.check_pair()   # built for another seed (and, after a compiler change, libraries missing)
                shutil.rmtree(staging)
                shutil.rmtree(other)
                os.environ['FAKE_METAL_VERSION'], os.environ['FAKE_MSL'] = '1', ''
                self.new_translator()

    def test_a_listed_state_the_default_serves_is_not_translated(self):
        self.list.write_text(SEED.HEADER + f'V\t{self.k1}\t' + '{"tex":{"0":"2d"}}\n')
        self.assertEqual(self.prepare()['variants'], 2)

    def test_a_failed_rebuild_leaves_the_published_seed(self):
        self.prepare()
        before = self.pair_state()
        os.environ['FAKE_FAIL'] = self.k2
        self.new_translator()
        with self.assertRaises(SEED.SeedError):
            self.prepare()
        self.assertEqual(self.pair_state(), before)
        self.assertFalse(self.output.with_name('shader-seed.partial').exists())

    def test_a_service_that_exits_is_a_failure(self):
        self.prepare()
        before = self.pair_state()
        os.environ['FAKE_DIE'] = self.k1
        self.new_translator()
        with self.assertRaises(SEED.SeedError):
            self.prepare()
        self.assertEqual(self.pair_state(), before)

    def test_an_excluded_key_is_recorded_with_its_result(self):
        os.environ['FAKE_FAIL'] = self.k2
        with mock.patch.object(SEED, 'EXCLUDED', {self.k2: 'test reason'}):
            manifest = self.prepare()
        self.assertEqual(manifest['shaders'], 1)
        self.assertEqual(manifest['excluded'], [dict(key=self.k2, reason='test reason', result='fake error')])
        self.assertFalse((self.output / f'{self.k2}.variants').exists())
        os.environ['FAKE_FAIL'] = ''
        self.new_translator()
        with mock.patch.object(SEED, 'EXCLUDED', {self.k2: 'test reason'}):
            manifest = self.prepare()
        self.assertEqual(manifest['shaders'], 2)
        self.assertEqual(manifest['excluded'][0]['result'], 'translated: now included')

    def test_a_grown_list_reuses_the_translations(self):
        self.prepare()
        with mock.patch('subprocess.Popen', side_effect=AssertionError('translated again')):   # no compiler run either
            self.assertEqual(SEED.prepare(self.work, self.output, 2, self.list, libs=False)['variants'], 3)

    def test_add_listed_takes_only_unserved_states(self):
        self.list.write_text(SEED.HEADER)
        self.prepare()
        played = self.work / 'played'
        played.mkdir()
        (played / f'{self.k1}.variants').write_text(json.dumps([
            dict(variant='a', deps=dict(tex={'0': '2d'})), dict(variant='b', deps=dict(tex={'0': '3d'}))]))
        (played / ('f' * 64 + '.variants')).write_text(json.dumps([dict(variant='c', deps=dict(tex={'0': '3d'}))]))
        SEED.add_listed(self.output, [played], self.list)
        self.assertEqual(SEED.read_list(self.list), [(self.k1, dict(tex={'0': '3d'}))])
        SEED.add_listed(self.output, [played], self.list)   # again: nothing new
        self.assertEqual(len(SEED.read_list(self.list)), 1)


class RuleTests(unittest.TestCase):
    def test_swap_in_exchanges_the_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            target, staging = Path(tmp) / 'shader-seed', Path(tmp) / 'shader-seed.partial'
            target.mkdir()
            (target / 'old').write_text('old')
            staging.mkdir()
            (staging / 'new').write_text('new')
            inode = staging.stat().st_ino
            SEED.libraries_module().swap_in(staging, target)
            self.assertEqual(target.stat().st_ino, inode)
            self.assertEqual([p.name for p in target.iterdir()], ['new'])
            self.assertFalse(staging.exists())
            other = Path(tmp) / 'fresh.partial'
            other.mkdir()
            SEED.libraries_module().swap_in(other, Path(tmp) / 'fresh')   # nothing published yet: a plain rename
            self.assertTrue((Path(tmp) / 'fresh').is_dir())

    def test_variant_name_is_the_runtimes(self):
        answers = dict(tex={'0': 'cube'}, img={}, attr={}, fout={})
        tag = hashlib.sha256(b'{"attr":{},"fout":{},"img":{},"tex":{"0":"cube"}}').hexdigest()[:12]
        self.assertEqual(SEED.variant_name('a' * 64, answers), 'a' * 64 + '-' + tag)
        self.assertEqual(SEED.variant_name('a' * 64, SEED.ANSWERS), 'a' * 64 + '-b7530f698e76')

    def test_deps_match_uses_the_runtimes_defaults(self):
        self.assertTrue(SEED.deps_match(dict(tex={'1': '2d'}, attr={'0': 'float'}), SEED.ANSWERS))
        self.assertFalse(SEED.deps_match(dict(tex={'1': 'cube'}), SEED.ANSWERS))
        self.assertTrue(SEED.deps_match(dict(fout={'0': 'uint'}), SEED.answers_for(dict(fout={'0': 'uint'}))))

    def test_list_rows_are_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'list.tsv'
            for bad in ('V\tnotakey\t{}\n', 'S\t' + 'a' * 64 + '\t{}\n', 'V\t' + 'a' * 64 + '\t{"pso":{}}\n'):
                path.write_text(SEED.HEADER + bad)
                with self.assertRaises(SEED.SeedError):
                    SEED.read_list(path)

    def test_only_listed_keys_are_excused(self):
        failures = [dict(key='a' * 64, variant='a' * 64 + '-x', source='s', error='e')]
        with mock.patch.object(SEED, 'EXCLUDED', {'b' * 64: 'r'}):
            with self.assertRaises(SEED.SeedError):
                SEED.judge(failures)
        with mock.patch.object(SEED, 'EXCLUDED', {'a' * 64: 'r'}):
            self.assertEqual(SEED.judge(failures), [dict(key='a' * 64, reason='r', result='e')])

    def test_census_must_match(self):
        k, s = shader(1)
        with self.assertRaises(SEED.SeedError):
            SEED.census({k: s})

    def test_bnsh_keys_are_control_and_code(self):
        ctl, code = control('ps', length=0x40), bytes(range(0x40))
        bnsh = bytearray(0x1200)
        bnsh[0:4], bnsh[12:14], bnsh[96:100] = b'BNSH', b'\xff\xfe', b'grsc'
        struct.pack_into('<I', bnsh, 28, len(bnsh))
        struct.pack_into('<IQ', bnsh, 124, 1, 0x100)            # one variation table at 0x100
        struct.pack_into('<Q', bnsh, 0x100 + 16, 0x200)          # its program at 0x200
        struct.pack_into('<Q', bnsh, 0x200 + 40, 0x300)          # the pixel stage record at 0x300
        struct.pack_into('<QQII', bnsh, 0x308, 0x400, 0xd00, len(code), 0x880)
        bnsh[0x400:0xc80], bnsh[0xd00:0xd40] = ctl, code
        found = list(SEED.bnsh_shaders(bytes(bnsh)))
        self.assertEqual([(k, s) for k, s, _, _ in found], [(hashlib.sha256(ctl + code).hexdigest(), 'ps')])

    def test_chain_is_the_runtimes(self):
        header = (HERE / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
        self.assertIn(f'#define DMSH_CHAIN "{SEED.CHAIN}"', header)

    def test_excluded_keys_are_exact(self):
        self.assertEqual(len(SEED.EXCLUDED), 2)
        for key, reason in SEED.EXCLUDED.items():
            self.assertRegex(key, '^[0-9a-f]{64}$')
            self.assertTrue(reason)

    def test_the_shipped_list_reads(self):
        rows = SEED.read_list(SEED.LIST)
        self.assertEqual(len(rows), len(set((k, SEED.canonical(d)) for k, d in rows)))


if __name__ == '__main__':
    unittest.main()
