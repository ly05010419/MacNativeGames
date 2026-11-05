"""NRO collection/rewrite cache lifecycle, using synthetic manifests and a fake rewriter."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class NroPack(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.repo = self.tmp / 'repo'; self.exe = self.tmp / 'game/ExeFS'; self.rom = self.exe.parent / 'RomFS'
        self.exe.mkdir(parents=True); self.rom.mkdir(); (self.exe / 'main').write_bytes(b'NSO0' + bytes(0x50))
        (self.rom / 'game.nrr').write_bytes(b'registered')
        for f in ('dm/tools/nro_collect.py', 'dm/tools/nso_syms.py', 'dm/tools/patch_nso.py', 'dm/tools/nso_code.py', 'dm/tools/nso_asm.py', 'dm/tools/nso_match.py', 'dm/tools/sdk_patches.py', 'dm/gfx/tools/smash_arc.py'):
            p = self.repo / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('v1')
        self.out = self.tmp / 'patched_test'; self.logs = self.tmp / 'logs'; self.logs.mkdir()
        self.g = types.SimpleNamespace(words=lambda k: ['main'])
        self.row = 'refused\t' + 'ab' * 32 + '\t128\tgame.nrr\tloose sdk.nro\n'
        self.collections = self.rewrites = 0
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'run', self.collect),
                   mock.patch.object(nsp.subprocess, 'Popen', self.rewrite_process), mock.patch.object(nsp.time, 'sleep')):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def collect(self, cmd, log, **kwargs):
        self.collections += 1; dst = Path(cmd[3]); dst.mkdir(parents=True)
        (dst / 'nro.tsv').write_text('# name\tsha256\tsize\tnrr\twhere\n' + self.row)

    def rewrite_process(self, cmd, **kwargs):
        self.rewrites += 1; (Path(cmd[3]) / f'{cmd[4]}.patched.bin').write_bytes(b'rewritten')
        return types.SimpleNamespace(poll=lambda: 0, returncode=0)

    def build(self): nsp.rewrite(sys.executable, self.repo, self.exe, self.out, {}, self.logs, self.g)

    def test_refused_only_manifest_survives_and_invalidates_rewrite(self):
        self.build()
        self.assertIn(self.row, (self.out / 'nro.tsv').read_text())
        self.build(); self.assertEqual((self.collections, self.rewrites), (1, 1))
        self.row = self.row.replace('ab' * 32, 'cd' * 32)
        (self.rom / 'game.nrr').write_bytes(b'new registration')
        self.build()
        self.assertIn(self.row, (self.out / 'nro.tsv').read_text())
        self.assertEqual((self.collections, self.rewrites), (2, 2))

    def test_parser_changes_recollect(self):
        self.build()
        for i, f in enumerate(('dm/gfx/tools/smash_arc.py', 'dm/tools/nso_syms.py'), 2):
            (self.repo / f).write_text('v2'); self.build()
            self.assertEqual(self.collections, i)
        self.assertEqual(self.rewrites, 1)   # identical collected bytes do not need another rewrite

    def test_removed_registration_drops_stale_manifest(self):
        self.build(); (self.rom / 'game.nrr').unlink(); self.build()
        self.assertFalse((self.out / 'nro.tsv').exists())
        self.assertFalse((self.tmp / 'nro_test').exists())
        self.assertEqual(self.rewrites, 2)

    def test_unknown_build_reuse_counts_selected_rules_and_matcher(self):
        selected = self.repo / 'dm/games/0100000000000001/patches.py'
        foreign = self.repo / 'dm/games/0100000000000002/patches.py'
        for p in (selected, foreign): p.parent.mkdir(parents=True); p.write_text('MATCH_CUTS = {}')
        env = {'DM_GAME': selected.parent.name, 'DM_GAMES_DIR': '/derived/profile', 'DM_GAMES_FIRST': '1'}
        def build(): nsp.rewrite(sys.executable, self.repo, self.exe, self.out, env, self.logs, self.g)
        build(); build(); self.assertEqual(self.rewrites, 1)
        foreign.write_text('MATCH_CUTS = {} # unrelated change')
        build(); self.assertEqual(self.rewrites, 1)
        selected.write_text('MATCH_CUTS = {} # selected change')
        build(); self.assertEqual(self.rewrites, 2)
        (self.repo / 'dm/tools/nso_match.py').write_text('matcher changed')
        build(); self.assertEqual(self.rewrites, 3)
        env['DM_GAME'] = foreign.parent.name
        build(); self.assertEqual(self.rewrites, 4)


if __name__ == '__main__': unittest.main()
