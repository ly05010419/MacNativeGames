"""Ocarina of Time's translation cache (games/oot/scripts/prepare_ns2_seed.py; user 2026-11-11: "如何让 快速更新 不要这么容易
失效"): the cache folder belongs to what decides every translation at once (chain, depth mode, translator), never to the
runtime's source; each translation keeps the request it was made from, a seed run checks it against its runtime's requests
(totk_shader_rt.m seed_verified), and only what that run checked or made goes into the seed."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/oot/scripts'))
NVN = ROOT / 'games/oot/third_party/d2r_gfx/gfx/nvn'


def load_seed():
    spec = importlib.util.spec_from_file_location('oot_prepare_ns2_seed', ROOT / 'games/oot/scripts/prepare_ns2_seed.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TranslationContext(unittest.TestCase):
    def setUp(self):
        self.seed = load_seed()
        self.tmp = tempfile.TemporaryDirectory()
        self.translator = Path(self.tmp.name) / 'totk_xlate'
        self.translator.write_bytes(b'translator-v1')

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_translator_and_the_depth_mode_decide(self):
        base = self.seed.translation_context(self.translator, 'minus-w')
        self.assertEqual(base, self.seed.translation_context(self.translator, 'minus-w'))
        self.assertNotEqual(base, self.seed.translation_context(self.translator, 'zero'))
        self.translator.write_bytes(b'translator-v2')
        self.assertNotEqual(base, self.seed.translation_context(self.translator, 'minus-w'))

    def test_the_runtime_source_does_not(self):
        # 4b308d25 changed how the translator is started and every translation of the 15:19 build was made again
        self.assertFalse(hasattr(self.seed, 'request_sources'))
        body = Path(self.seed.__file__).read_text().split('def translation_context')[1].split('\n\n\n')[0]
        inputs = [line for line in body.splitlines() if line.strip().startswith('inputs = ')]
        self.assertEqual(inputs, ['    inputs = dict(schema=3, chain=CHAIN, depth_mode=depth_mode, translator_sha256=sha256(translator))'])


class SeedFiles(unittest.TestCase):
    """Only the variants the run checked or made: a cached one no request of this runtime reached stays out of the seed."""

    def setUp(self):
        self.seed = load_seed()
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name)
        self.shader = 'ab' * 32
        self.names = [f'{self.shader}-{s * 12}' for s in 'cde']
        entries = []
        for name in self.names:
            (self.cache / f'{name}_b.metal').write_text('// metal\n' * 10)
            (self.cache / f'{name}_b.json').write_text(json.dumps({'chain': self.seed.CHAIN, 'key': name + '_b', 'entry': 'main0'}))
            entries.append({'variant': name, 'deps': {}, 'request': '0' * 64, 'answers': {}})
        (self.cache / f'{self.shader}.variants').write_text(json.dumps(entries))

    def tearDown(self):
        self.tmp.cleanup()

    def test_without_a_list_every_cached_variant(self):
        files = self.seed.seed_files(self.cache)
        self.assertEqual(sum(n.endswith('_b.metal') for n in files), 3)

    def test_with_the_run_s_list_only_those(self):
        files, indexes = self.seed.seed_files(self.cache, {self.names[0], self.names[2]})
        self.assertEqual(sorted(n for n in files if n.endswith('_b.metal')), [f'{self.names[0]}_b.metal', f'{self.names[2]}_b.metal'])
        self.assertEqual([e['variant'] for e in indexes[f'{self.shader}.variants']], [self.names[0], self.names[2]])

    def test_all_of_an_index_checked_links_it_as_it_is(self):
        files, indexes = self.seed.seed_files(self.cache, set(self.names))
        self.assertIn(f'{self.shader}.variants', files)
        self.assertEqual(indexes, {})

    def test_none_of_a_shader_checked_leaves_it_out(self):
        files, indexes = self.seed.seed_files(self.cache, set())
        self.assertEqual(files, {})
        self.assertEqual(indexes, {})


class PublishDigests(unittest.TestCase):
    """极速更新 (user 2026-11-11): a seed file that is still the cache file the last seed had keeps its recorded digest."""

    def test_unchanged_files_keep_their_digests(self):
        seed_mod = load_seed()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cache, out = tmp / 'cache', tmp / 'seed'
            cache.mkdir(); out.mkdir()
            names = ['a_b.metal', 'b_b.metal', 'c.variants']
            for n in names:
                (cache / n).write_text(n * 50)
                (out / n).hardlink_to(cache / n)
            files = {n: seed_mod.sha256(out / n) for n in names}
            from prepare_shader_seed import record_verified
            record_verified(out, files)
            # the next publish: the same cache files linked again, one translation made again, one index written anew
            staging = tmp / 'seed.partial'; staging.mkdir()
            (cache / 'b_b.metal').unlink(); (cache / 'b_b.metal').write_text('made again')
            for n in names[:2]:
                (staging / n).hardlink_to(cache / n)
            (staging / 'c.variants').write_text('[]')
            read = []
            got, count = seed_mod.publish_digests(sorted(staging.iterdir()), out,
                                                   lambda paths: (read.extend(p.name for p in paths) or {p.name: seed_mod.sha256(p) for p in paths}))
            self.assertEqual(count, 2)
            self.assertEqual(sorted(read), ['b_b.metal', 'c.variants'])
            self.assertEqual(got['a_b.metal'], files['a_b.metal'])
            self.assertEqual(got['b_b.metal'], seed_mod.sha256(staging / 'b_b.metal'))


class SeedUnchanged(unittest.TestCase):
    """极速更新: a seed whose files are all the published ones, from the same inputs, is kept without publishing again."""

    def setUp(self):
        self.seed = load_seed()
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name)
        self.cache, self.out = tmp / 'cache', tmp / 'seed'
        self.cache.mkdir(); self.out.mkdir()
        self.files = {}
        for n in ('a_b.metal', 'a_b.json', 'a.variants'):
            (self.cache / n).write_text(n)
            (self.out / n).hardlink_to(self.cache / n)
            self.files[n] = self.cache / n
        (self.out / 'shader-keys.json').write_text('{}')
        self.inputs = {'chain': 'x', 'translator_sha256': '1'}
        (self.out / 'seed-manifest.json').write_text(json.dumps({'inputs': self.inputs, 'files': {n: '0' for n in [*self.files, 'shader-keys.json']}}))

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_same_files_and_inputs_are_kept(self):
        self.assertTrue(self.seed.seed_unchanged(self.out, self.files, self.inputs))

    def test_a_translation_made_again_is_published(self):
        (self.cache / 'a_b.metal').unlink(); (self.cache / 'a_b.metal').write_text('again')
        self.assertFalse(self.seed.seed_unchanged(self.out, self.files, self.inputs))

    def test_other_inputs_or_another_file_are_published(self):
        self.assertFalse(self.seed.seed_unchanged(self.out, self.files, {**self.inputs, 'translator_sha256': '2'}))
        (self.cache / 'b_b.metal').write_text('new')
        self.assertFalse(self.seed.seed_unchanged(self.out, {**self.files, 'b_b.metal': self.cache / 'b_b.metal'}, self.inputs))

    def test_a_seed_without_inputs_is_published(self):
        (self.out / 'seed-manifest.json').write_text(json.dumps({'files': {n: '0' for n in self.files}}))
        self.assertFalse(self.seed.seed_unchanged(self.out, self.files, self.inputs))


class RuntimeKeepsTheRequest(unittest.TestCase):
    """The runtime side, in its source: the request's digest goes into the index entry and never to the translator."""

    def test_the_parts(self):
        rt = (NVN / 'totk_shader_rt.m').read_text(errors='replace')
        self.assertIn('rq[@"_request"] = hex(rd, 32); rq[@"_answers"] = answers;', rt)
        self.assertIn('entry[@"request"] = rq[@"_request"]', rt)
        self.assertIn('if (seed_verify()) found = seed_verified(shader, stage, control, code, local, answers, found);', rt)
        # every place a request leaves the runtime sends it without the "_" keys
        self.assertNotIn('json_string(rq)', rt)
        self.assertIn('TOTK_SEED_VERIFIED_OUT', (NVN / 'nvn_mtl.m').read_text(errors='replace'))


if __name__ == '__main__':
    unittest.main()
