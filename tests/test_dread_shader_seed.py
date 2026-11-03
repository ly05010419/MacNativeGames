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
spec.loader.exec_module(seed)
# The names totk_shader_seed.h totk_seed_import_into accepts; anything else makes it refuse the whole seed.
SAFE = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$')


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

    def prepare(self, root, listed=()):
        """prepare() with a synthetic translator service; returns (manifest, the variants it was asked for)."""
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
                self.reply = '{"ok":true}\n'

            def flush(self): pass
            def close(self): pass
            def wait(self): return 0
            def readline(self): return self.reply
        Service.requests = self.requests = []

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


if __name__ == '__main__':
    unittest.main()
