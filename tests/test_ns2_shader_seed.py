"""The Switch 2 seed's translation cache (games/totk/scripts/prepare_ns2_seed.py, Bananza and Pokopia; user 2026-11-11: "如何让
快速更新 不要这么容易失效"): its folder belongs to what decides every translation at once (chain, depth mode, translator, the
spirv-cross it runs), never to the runtime binary; each translation keeps its request and a seed run checks it against its
runtime's (totk_shader_rt.m seed_verified), and only what that run checked or made goes into the seed."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('prepare_ns2_seed', SCRIPTS / 'prepare_ns2_seed.py')
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


class TranslationContext(unittest.TestCase):
    def test_translator_spirv_cross_and_depth_mode_decide(self):
        with tempfile.TemporaryDirectory() as tmp:
            translator, spirv = Path(tmp) / 'translator', Path(tmp) / 'spirv-cross'
            translator.write_bytes(b'translator-v1')
            spirv.write_bytes(b'spirv-cross-v1')
            base = seed.translation_context(translator, 'minus-w', spirv)
            self.assertEqual(base, seed.translation_context(translator, 'minus-w', spirv))
            self.assertNotEqual(base, seed.translation_context(translator, 'zero', spirv))
            spirv.write_bytes(b'spirv-cross-v2')
            changed = seed.translation_context(translator, 'minus-w', spirv)
            self.assertNotEqual(base, changed)
            translator.write_bytes(b'translator-v2')
            self.assertNotEqual(changed, seed.translation_context(translator, 'minus-w', spirv))

    def test_the_runtime_binary_does_not(self):
        # any runtime change used to translate every shader again; the requests decide now (seed_verified)
        self.assertEqual(seed.translation_context.__code__.co_varnames[:3], ('translator', 'depth_mode', 'spirv_cross'))


class SeedFiles(unittest.TestCase):
    def test_only_what_the_run_checked_or_made(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)
            shader = 'cd' * 32
            names = [f'{shader}-{c * 12}' for c in 'ab']
            for name in names:
                (cache / f'{name}_b.metal').write_text('// metal\n')
                (cache / f'{name}_b.json').write_text('{}')
            (cache / f'{shader}.variants').write_text(json.dumps([{'variant': n, 'deps': {}} for n in names]))
            self.assertEqual(sum(n.endswith('_b.metal') for n in seed.seed_files(cache)), 2)
            files, indexes = seed.seed_files(cache, {names[1]})
            self.assertEqual([n for n in files if n.endswith('_b.metal')], [f'{names[1]}_b.metal'])
            self.assertEqual([e['variant'] for e in indexes[f'{shader}.variants']], [names[1]])



class GameShaderKeys(unittest.TestCase):
    """Every shader the game ships (game-shader-keys.json, from the corpus): beside the seed, never in its manifest, and
    written only when it changed (user 2026-11-12: every game's shaders apart; the shader merge reads it)."""
    ROOT = Path(__file__).resolve().parents[1]

    def test_written_once_and_kept_while_it_holds_the_same_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            identity = seed.shader_key_scope(['b' * 64, 'a' * 64], 'pkp', 'c' * 64)
            seed.write_identity(folder, identity)
            path = folder / seed.IDENTITY
            self.assertEqual(json.loads(path.read_text())['keys'], ['a' * 64, 'b' * 64])
            before = path.stat().st_mtime_ns
            seed.write_identity(folder, identity)
            self.assertEqual(path.stat().st_mtime_ns, before, 'the same keys: not written again (the bundle stays signed)')
            seed.write_identity(folder, seed.shader_key_scope(['a' * 64], 'pkp', 'c' * 64))
            self.assertEqual(json.loads(path.read_text())['keys'], ['a' * 64])

    def test_never_one_of_the_manifests_files(self):
        # the runtime takes a manifest's files only by its own names and refuses the seed for any other one
        guard = (self.ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_seed.h').read_text()
        self.assertNotIn('game-shader-keys', guard)
        text = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertLess(text.index("report['files'], read = publish_digests("), text.index('write_identity(staging, identity)'),
                        'written after the manifest takes its digests')
        self.assertIn("'--all-keys', all_keys", text)

    def test_a_translated_shader_outside_the_game_stops_the_seed(self):
        text = (SCRIPTS / 'prepare_ns2_seed.py').read_text()
        self.assertIn("if not set(keys) <= set(identity['keys']):", text)


if __name__ == '__main__':
    unittest.main()
