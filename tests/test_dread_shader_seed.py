"""Metroid Dread's shader seed (games/dread/scripts/prepare_shader_seed.py): shaders found in MSHD files, variants named and
matched as Dread's runtime does, and a seed whose files the runtime's import accepts."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT/'games/dread/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('dread_prepare_shader_seed', SCRIPTS/'prepare_shader_seed.py')
seed = importlib.util.module_from_spec(spec)
# prepare_shader_seed imports its sibling shader_list.py by name. In the whole suite Tears of the Kingdom's shader_list.py
# (which normalizes pipeline keys; Dread's does not) can already be in sys.modules under that name, so Dread's is loaded
# by path for this import and the earlier entry is put back afterwards.
_list_spec = importlib.util.spec_from_file_location('dread_shader_list', SCRIPTS/'shader_list.py')
_dread_list = importlib.util.module_from_spec(_list_spec)
_list_spec.loader.exec_module(_dread_list)
_earlier = sys.modules.get('shader_list')
sys.modules['shader_list'] = _dread_list
try:
    spec.loader.exec_module(seed)
finally:
    if _earlier is None:
        sys.modules.pop('shader_list', None)
    else:
        sys.modules['shader_list'] = _earlier
def safe_names():
    """The names totk_seed_import_into accepts, taken from the runtime header itself. An unknown filename there makes it
    refuse the whole seed, so a file added to the seed without widening that regex would ship one no iPhone or iPad can
    import. Reading the pattern instead of copying it keeps the two from drifting apart."""
    header = (ROOT/'games/dread/third_party/d2r_gfx/gfx/nvn/totk_shader_seed.h').read_text()
    m = re.search(r'regularExpressionWithPattern:\s*@"(\^\(\?:.*?\)\$)"', header, re.S)
    if not m:
        raise AssertionError('totk_shader_seed.h: the seed filename check is not where this test looks for it')
    return re.compile(m.group(1).replace('\\\\', '\\'))


SAFE = safe_names()


def control(stage, length, c1=0x30):
    c = bytearray(0x880)
    c[0:4] = seed.CONTROL_MAGIC
    struct.pack_into('<II', c, 0x700, c1, length)
    struct.pack_into('<I', c, 0x714, stage)
    return bytes(c)


def code(tag, length):
    body = seed.CODE_MAGIC + tag
    return body + bytes(length - len(body))


def mshd(*shaders):
    """An MSHD file: per shader its control section, assembly text (of a length that misaligns nothing) and code."""
    out = bytearray(b'MSHD' + struct.pack('<I', 0x130001) + bytes(0x38))
    for stage, tag, length in shaders:
        out += control(stage, length) + b'!!NVvp5.0\nOPTION NV_internal;\nEND\n'.ljust(64, b'\0') + code(tag, length)
    return bytes(out)


def key_of(stage, tag, length):
    return hashlib.sha256(control(stage, length) + code(tag, length)).hexdigest()


class MshdTests(unittest.TestCase):
    def test_each_control_section_pairs_with_the_next_code_and_hashes_as_the_runtime_does(self):
        data = mshd((0, b'vs-a', 0x200), (1, b'ps-a', 0x180), (0, b'vs-a', 0x200))   # the last repeats the first
        found = list(seed.mshd_shaders(data))
        self.assertEqual([(k, s) for k, s, _, _ in found], [(key_of(0, b'vs-a', 0x200), 'vs'), (key_of(1, b'ps-a', 0x180), 'ps')])
        _, _, ctl, body = found[1]
        self.assertEqual((len(ctl), body), (0x880, code(b'ps-a', 0x180)))

    def test_misaligned_magic_and_impossible_bounds_are_not_shaders(self):
        data = bytearray(mshd((1, b'ps-a', 0x180)))
        data += b'\0' + seed.CONTROL_MAGIC + bytes(0x900)     # not 4-aligned
        data += control(1, 0x30, c1=0x40) + code(b'x', 0x30)  # c1 beyond the code
        data += control(7, 0x100) + code(b'y', 0x100)         # a stage without a translation chain
        self.assertEqual([k for k, *_ in seed.mshd_shaders(bytes(data))], [key_of(1, b'ps-a', 0x180)])
        with self.assertRaises(ValueError):
            list(seed.mshd_shaders(b'BNSH' + bytes(64)))


class MatchingTests(unittest.TestCase):
    def test_variant_names_are_the_key_and_the_hash_of_the_answers_as_compact_sorted_json(self):
        # totk_shader_rt.m: SHA-256 of NSJSONSerialization's sorted output; empty answers give the default suffix.
        self.assertEqual(seed.variant_name('a'*64, seed.ANSWERS), 'a'*64 + '-b7530f698e76')
        answers = dict(tex={'2': 'cube'}, img={}, attr={}, fout={'0': 'float'})
        text = '{"attr":{},"fout":{"0":"float"},"img":{},"tex":{"2":"cube"}}'
        self.assertEqual(seed.variant_name('a'*64, answers), 'a'*64 + '-' + hashlib.sha256(text.encode()).hexdigest()[:12])

    def test_deps_match_counts_unanswered_slots_as_their_defaults_without_wildcards(self):
        deps = dict(tex={'1': '2d'}, fout={'0': 'float'}, attr={'0': 'float'})
        self.assertTrue(seed.deps_match(deps, seed.ANSWERS))
        self.assertTrue(seed.deps_match(deps, dict(tex={'1': '2d', '5': 'cube'})))
        self.assertFalse(seed.deps_match(deps, dict(tex={'1': 'cube'})))
        self.assertFalse(seed.deps_match(deps, dict(attr={'0': 'sint'})))
        self.assertFalse(seed.deps_match(dict(tex={'1': '*'}), dict(tex={'1': '2d'})))


class PrepareTests(unittest.TestCase):
    SHADERS = ((0, b'vs-a', 0x200), (1, b'ps-a', 0x180), (1, b'ps-b', 0x140))

    def romfs(self, root):
        romfs = root/'romfs'; romfs.mkdir()
        blob = mshd(*self.SHADERS)
        other = b'not a shader file'
        (romfs/'romfs.bin').write_bytes(other + blob)
        (romfs/'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256='c'*64, files=[
            dict(path='config.ini', offset=0, size=len(other)),
            dict(path='system/shd/test.bshdat', offset=len(other), size=len(blob))])))
        return romfs

    def prepare(self, root, listed=(), fail=()):
        """prepare() with a synthetic translator service; returns (manifest, the variants it was asked for).
        fail: shader keys whose translation the service refuses, as a real one does for a shader it cannot translate."""
        asked = []

        class Service:
            def __init__(self, *args, **kwargs):
                self.stdin = self.stdout = self
                self.reply = ''

            def write(self, line):
                request = json.loads(line)
                asked.append(request['variant'])
                self.requests.append(request)
                base = Path(request['code']).with_suffix('')
                deps = {k: request[k] for k in ('tex', 'img', 'attr', 'fout')}
                deps['tex'] = {'2': '2d', **deps['tex']}   # as if the code sampled slot 2
                base.with_name(base.name+'_b.metal').write_text('// synthetic '+request['variant'])
                base.with_name(base.name+'_b.json').write_text(json.dumps(dict(stage=request['stage'], deps=deps)))
                base.with_name(base.name+'.spv').write_bytes(b'spv')
                if request['variant'].partition('-')[0] in self.refuse:
                    self.reply = '{"ok":false,"error":"synthetic refusal"}\n'
                    return
                self.reply = '{"ok":true}\n'

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return self.reply
        Service.requests = self.requests = []
        Service.refuse = set(fail)

        translator, cross, shared = [root/name for name in ('translator', 'spirv-cross', 'list.tsv')]
        translator.write_text(''); cross.write_text('')
        shared.write_text(''.join(f'V\t{key}\t{deps}\n' for key, deps in sorted(listed)))
        romfs = root/'romfs' if (root/'romfs').is_dir() else self.romfs(root)
        with mock.patch.object(seed, 'MIN_SHADERS', 1), mock.patch.object(seed.subprocess, 'Popen', side_effect=Service):
            manifest = seed.prepare(romfs, root/'shader-seed', translator, cross, 'v100', shared)
        return manifest, asked

    @staticmethod
    def deps(**kinds):
        return json.dumps(kinds, sort_keys=True, separators=(',', ':'))

    def test_every_shader_gets_its_default_variant_and_the_seed_holds_only_names_the_runtime_imports(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            manifest, asked = self.prepare(root)
            keys = sorted(key_of(*s) for s in self.SHADERS)
            self.assertEqual(sorted(asked), [seed.variant_name(k, seed.ANSWERS) for k in keys])
            out = root/'shader-seed'
            names = {p.name for p in out.iterdir()} - {'seed-manifest.json'}
            self.assertEqual(set(manifest['files']), names)
            self.assertTrue(all(SAFE.match(n) for n in names), names)
            self.assertEqual((manifest['chain'], manifest['shaders'], manifest['discovered']), (seed.CHAIN, 3, 3))
            for name, sha in manifest['files'].items():
                self.assertEqual(hashlib.sha256((out/name).read_bytes()).hexdigest(), sha)
            rows = (out/'pso_manifest.tsv').read_text().splitlines()[1:]
            self.assertEqual(sorted(r.split('\t')[2] for r in rows), ['0', '1', '1'])
            vs = next(r for r in self.requests if r['stage'] == 'vs')
            self.assertTrue(vs['depth_minus_w'])   # Dread's depth mode: the Mac's own translations convert clip z

    def test_a_listed_state_the_default_serves_is_not_translated_and_another_is(self):
        ps = key_of(1, b'ps-a', 0x180)
        served = self.deps(attr={}, fout={}, img={}, tex={'2': '2d'})
        cube = self.deps(attr={}, fout={}, img={}, tex={'2': 'cube'})
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            manifest, asked = self.prepare(root, [(ps, served), (ps, cube), ('f'*64, cube)])   # f...: not this game's
            cube_variant = seed.variant_name(ps, seed.answers_for(json.loads(cube)))
            self.assertIn(cube_variant, asked)
            self.assertEqual((len(asked), manifest['shaders'], manifest['listed_variants']), (4, 4, 2))
            index = json.loads((root/'shader-seed'/f'{ps}.variants').read_text())
            self.assertEqual([r['variant'] for r in index], [seed.variant_name(ps, seed.ANSWERS), cube_variant])
            self.assertTrue(seed.builtin_serves(root/'shader-seed', ps, served))
            self.assertFalse(seed.builtin_serves(root/'shader-seed', ps, cube))

    def test_an_unchanged_seed_is_reused_and_a_longer_list_translates_only_its_new_state(self):
        ps = key_of(1, b'ps-b', 0x140)
        cube = self.deps(attr={}, fout={}, img={}, tex={'2': 'cube'})
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            first, _ = self.prepare(root)
            again, asked = self.prepare(root)
            self.assertEqual((again, asked), (first, []))
            grown, asked = self.prepare(root, [(ps, cube)])
            self.assertEqual(asked, [seed.variant_name(ps, seed.answers_for(json.loads(cube)))])
            self.assertEqual(grown['shaders'], 4)



class ShaderKeyScopeTests(PrepareTests):
    """shader-keys.json: the game's whole shader inventory, which the Launcher's "Check shaders" reads to tell this
    version's shared-list rows from another version's (shader_update.py new_shaders)."""

    def test_the_seed_names_every_shader_found_even_one_translation_refused(self):
        refused = key_of(*self.SHADERS[2])
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            manifest, _ = self.prepare(root, fail=[refused])
            scope = json.loads((root/'shader-seed'/'shader-keys.json').read_text())
            self.assertEqual(scope['keys'], sorted(key_of(*s) for s in self.SHADERS))
            self.assertEqual((scope['schema'], scope['chain'], scope['version']), (1, seed.CHAIN, 'v100'))
            self.assertEqual(scope['source_romfs_sha256'], 'c'*64)
            # The refused shader has no variants of its own, so a scope derived from the seed's translations would
            # leave it out and the check would pass its list rows over as another version's.
            self.assertIn(refused, scope['keys'])
            self.assertFalse((root/'shader-seed'/f'{refused}.variants').exists())
            self.assertEqual(manifest['discovered'], 3)
            self.assertEqual(hashlib.sha256((root/'shader-seed'/'shader-keys.json').read_bytes()).hexdigest(),
                             manifest['files']['shader-keys.json'])

    IDENTITY = ('schema', 'chain', 'version', 'game', 'source_romfs_sha256', 'translator_sha256',
                'spirv_cross_sha256', 'preparation_sha256', 'shader_list_sha256')

    def test_a_seed_prepared_before_the_inventory_is_made_again_rather_than_reused(self):
        with tempfile.TemporaryDirectory() as d:
            root, out = Path(d), Path(d)/'shader-seed'
            manifest, _ = self.prepare(root)
            identity = {k: manifest[k] for k in self.IDENTITY}
            reusable = lambda: seed.reusable_seed(out, identity)   # noqa: E731 - MIN_SHADERS as the test's prepare sets it
            with mock.patch.object(seed, 'MIN_SHADERS', 1):
                self.assertIsNotNone(reusable())   # as prepared, it is reused
            old = json.loads((out/'seed-manifest.json').read_text())
            old['files'].pop('shader-keys.json')
            (out/'seed-manifest.json').write_text(json.dumps(old))
            (out/'shader-keys.json').unlink()
            # Its translations are untouched and still match, but without the inventory the check cannot tell this
            # version's list rows from another's, so the seed is prepared again instead of shipped as it is.
            with mock.patch.object(seed, 'MIN_SHADERS', 1):
                self.assertIsNone(reusable())
            again, _ = self.prepare(root)
            self.assertIn('shader-keys.json', again['files'])
            self.assertTrue((out/'shader-keys.json').is_file())

    def test_the_scope_refuses_what_is_not_this_game_s_inventory(self):
        keys, romfs = [key_of(*self.SHADERS[0])], 'c'*64
        self.assertEqual(seed.shader_key_scope(keys, 'v100', romfs)['keys'], keys)
        for bad in (lambda: seed.shader_key_scope([], 'v100', romfs),
                    lambda: seed.shader_key_scope(['nope'], 'v100', romfs),
                    lambda: seed.shader_key_scope(keys[0], 'v100', romfs),
                    lambda: seed.shader_key_scope(keys, '', romfs),
                    lambda: seed.shader_key_scope(keys, 'v100', 'nope'),
                    lambda: seed.shader_key_scope(keys, 'v100', romfs, chain='another')):
            with self.assertRaises(ValueError):
                bad()


class WarmPipelineTests(unittest.TestCase):
    """Render pipelines of the list go into the seed's warm-up manifest as recorded, only when the seed holds their
    shaders: Dread's runtime looks pipelines up by the key as recorded (no pso_key_norm)."""

    def key(self, vs, fs, tail=b''):
        name = seed.NAME
        raw = vs.encode().ljust(name, b'\0') + fs.encode().ljust(name, b'\0') + tail
        return raw.ljust(544, b'\0').hex()

    def test_rows_whose_shaders_the_seed_holds_are_kept_as_recorded(self):
        a, b, c = ('%064x-%012x_b' % (i, i) for i in (1, 2, 3))
        # blend factors set on a target that does not blend: TOTK's pso_key_norm would clear them
        odd = self.key(a, b, bytes(272 - 160) + bytes([7] * 8))
        rows = [odd, self.key(a, c), self.key(a, '')]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'shader_list.tsv'
            path.write_text(''.join(f'R\t{r}\tm\n' for r in rows))
            self.assertEqual(seed.warm_pipelines(path, {a, b}), [odd, self.key(a, '')])
            self.assertEqual(seed.warm_pipelines(Path(tmp)/'missing.tsv', {a, b}), [])

    def test_the_shipped_list_reads_and_its_pipelines_name_listed_shader_keys(self):
        rows = [r for r in seed.ShaderList.load(seed.LIST).warm if r[0] == 'R']
        self.assertTrue(rows)
        for _, key in rows:
            self.assertEqual(len(key), 2 * 544)
            vs = bytes.fromhex(key)[:seed.NAME].partition(b'\0')[0].decode()
            self.assertRegex(vs, r'^[0-9a-f]{64}-[0-9a-f]{12}_b$')

if __name__ == '__main__':
    unittest.main()
