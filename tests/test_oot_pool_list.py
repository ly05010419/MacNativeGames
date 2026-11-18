"""Ocarina of Time's pool proof is made once at packaging (build/pool_sources.tsv), not at every shader load (2026-11-18,
user: "a"). nvn_pool_sources took 40-70 ms for a 200 KB shader, 99.6 s over the 40,659 translations, at every start's
warm-up and at every first load in play (80 ms frames on the second save).

The proof itself and its rows are checked by games/oot/tests/pool_sources.m (probe-pool-sources: every proof written as a
row and read back) and by tools/pool_sources_list.m --verify over the real seed at every packaging. These tests check the
wiring: the list is made by the runtime's own code and kept only for the same seed and proof, the App carries it beside
the shader map, and the runtime uses a row only for the MSL and reflection it was made from."""
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_pool_list', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PoolList(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.work = Path(self.temp.name)
        self.tree = self.work / 'oot'
        seed = self.tree / 'build/shader-seed/oot'
        seed.mkdir(parents=True)
        (seed / 'seed-manifest.json').write_text('{"seed": 1}\n')
        (self.work / 'build').mkdir()
        (self.work / 'build/pool-sources-list').write_text('#!/bin/sh\n')
        self.build = load_build()

    def tearDown(self):
        self.temp.cleanup()

    def made(self):
        calls = []
        def run(cmd, check):
            calls.append(cmd)
            Path(cmd[2]).write_text(f'# nvn pool sources v1 seed={cmd[3]} scan={cmd[4]}\n')
            return subprocess.CompletedProcess(cmd, 0)
        with mock.patch.object(self.build.subprocess, 'run', side_effect=run):
            out = self.build.pool_list(self.work, self.tree, 'full')
        return out, calls

    def test_made_by_the_runtime_s_tool_with_every_row_read_back(self):
        out, calls = self.made()
        self.assertEqual(out, self.tree / 'build/pool_sources.tsv')
        self.assertEqual(len(calls), 1)
        cmd = calls[0]
        self.assertEqual(Path(cmd[0]), self.work / 'build/pool-sources-list')   # built with the runtime (Makefile)
        self.assertEqual(cmd[3], self.build.sha(self.tree / 'build/shader-seed/oot/seed-manifest.json'))
        self.assertEqual(cmd[4], self.build.sha(NVN / 'nvn_pool_sources.m'))
        self.assertEqual(cmd[-1], '--verify')

    def test_kept_for_the_same_seed_and_proof_made_again_otherwise(self):
        self.made()
        self.assertEqual(self.made()[1], [])                                     # nothing changed: kept
        (self.tree / 'build/shader-seed/oot/seed-manifest.json').write_text('{"seed": 2}\n')
        self.assertEqual(len(self.made()[1]), 1)                                 # another seed: made again

    def test_no_tool_no_list(self):
        (self.work / 'build/pool-sources-list').unlink()
        with self.assertRaises(ValueError):
            self.build.pool_list(self.work, self.tree, 'full')


class Wiring(unittest.TestCase):
    def test_the_app_carries_it_beside_the_shader_map(self):
        build = (GAME / 'build.py').read_text()
        body = build[build.index('def package_app('):]
        self.assertIn("pools=pool_list(work,tree,mode) if shader_record else None", body)
        self.assertIn("clone_file(pools,data/'build/pool_sources.tsv')", body)

    def test_the_tool_is_built_with_every_runtime(self):
        make = (GAME / 'Makefile').read_text()
        self.assertIn('runtime: $(OUT)/oot-native $(OUT)/pool-sources-list', make)
        rule = make[make.index('$(OUT)/pool-sources-list:'):]
        for part in ('tools/pool_sources_list.m', 'third_party/d2r_gfx/gfx/nvn/nvn_pool_sources.m', 'third_party/d2r_gfx/gfx/nvn/nvn_refl.m'):
            self.assertIn(part, rule.splitlines()[0] + rule.splitlines()[2])

    def test_the_runtime_uses_a_row_only_for_its_own_msl_and_reflection(self):
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('g_pool_list = pool_list_load(', mtl)
        load = mtl[mtl.index('static NvShader *shader_load_mode('):]
        load = load[:load.index('\n}\n')]
        self.assertIn('[row[0] isEqualToString:pool_list_digest(full, jd)]', load)
        self.assertIn('nvn_pool_sources_decode(&o->rf, row[1])', load)
        self.assertIn('proved = nvn_pool_sources(&o->rf, j[@"bindings"], full)', load)   # anything else proven as before
        self.assertLess(load.index('refl_mark_stores(o'), load.index('nvn_pool_sources_decode'))   # rw marked first, as the tool does

    def test_the_tool_marks_stores_before_the_proof_as_the_runtime_does(self):
        tool = (GAME / 'tools/pool_sources_list.m').read_text()
        self.assertLess(tool.index('nvn_refl_mark_stores(&rf'), tool.index('nvn_pool_sources(&rf'))
        self.assertIn('static void refl_mark_stores(NvShader *o, NSArray *bs, NSString *src) { nvn_refl_mark_stores(',
                      (NVN / 'nvn_mtl.m').read_text())   # one copy of the rule, shared


if __name__ == '__main__':
    unittest.main()
