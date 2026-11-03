"""Shader regressions between two builds of a game's shader libraries (dm/gfx/tools/shader_regressions.py) and the NSP script's stop on them (build_ipa_from_nsp.py
shader_regressions; GitHub issue #272): a shader that translated in the newest other b-*/ of the same game and fails now is listed, new keys and shaders that failed before are not,
another game's stamped library is never the previous build, and the NSP script stops unless ALLOW_SHADER_REGRESSIONS. Build reports only: no game files, translator or Metal needed."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
TOOL = REPO / 'dm/gfx/tools/shader_regressions.py'
spec = importlib.util.spec_from_file_location('shader_regressions', TOOL)
sr = importlib.util.module_from_spec(spec); spec.loader.exec_module(sr)
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

FAIL = 'NotSupportedException: MSL 2.3 image atomics require IGN with a discarded result'


class Base(unittest.TestCase):
    def setUp(self):
        self.lib = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.lib)
        self.mtime = 1_000_000

    def build(self, chain, rows, game=None):   # a library folder with dmsh_build.py's report; each later call is newer
        d = self.lib / ('b-' + chain); d.mkdir()
        (d / sr.REPORT).write_text(json.dumps([{'key': k, 'st': st, 'err': err} for k, st, err in rows]))
        if game: (d / 'inputs.tsv').write_text(f'game\t{game}\ninputs\t0123456789ab\n')
        self.mtime += 10; os.utime(d / sr.REPORT, (self.mtime, self.mtime))

    def tool(self, chain):
        return subprocess.run([sys.executable, str(TOOL), str(self.lib), chain], capture_output=True, text=True)


class Compare(Base):
    def test_lists_only_shaders_that_were_ok(self):
        self.build('old', [('a', 'ok', ''), ('b', 'ok', ''), ('c', 'translation failed', 'x')])
        self.build('new', [('a', 'ok', ''), ('b', 'translation failed', FAIL), ('c', 'translation failed', 'x'), ('d', 'metal failed', 'y')])
        self.assertEqual(sr.compare(str(self.lib), 'new'), ('b-old', [('b', 'translation failed', FAIL)]))
        r = self.tool('new')
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout.splitlines(), ['shader regressions against b-old: 1', f'[RED] b: translated in b-old, translation failed in b-new: {FAIL}'])

    def test_newest_other_build(self):
        self.build('older', [('a', 'ok', '')])
        self.build('old', [('a', 'translation failed', 'x')])
        self.build('new', [('a', 'translation failed', 'x')])
        self.assertEqual(sr.compare(str(self.lib), 'new'), ('b-old', []))
        self.assertEqual(self.tool('new').returncode, 0)

    def test_other_game_skipped(self):   # one chain version serves every game: only the stamp tells the games apart
        self.build('mine', [('a', 'ok', '')], game='010069C01AB82000')
        self.build('theirs', [('a', 'ok', '')], game='01006000040C2000')
        self.build('new', [('a', 'translation failed', FAIL)], game='010069C01AB82000')
        self.assertEqual(sr.compare(str(self.lib), 'new')[0], 'b-mine')

    def test_nothing_to_compare(self):
        self.build('new', [('a', 'translation failed', FAIL)])
        (self.lib / 'cas').mkdir(); (self.lib / 'b-empty').mkdir()   # dmsh_build.py's compile cache, a folder without a report
        r = self.tool('new')
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('no earlier build', r.stdout)

    def test_no_report(self):
        self.assertEqual(self.tool('new').returncode, 2)


class NspScript(Base):
    def setUp(self):
        super().setUp()
        self.logs = self.lib / 'logs'; self.logs.mkdir()
        self.said = []
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        self.build('old', [('a', 'ok', ''), ('b', 'ok', '')])

    def check(self):
        nsp.shader_regressions(sys.executable, REPO, self.lib, 'new', self.logs)

    def test_stops(self):
        self.build('new', [('a', 'ok', ''), ('b', 'translation failed', FAIL)])
        with mock.patch.object(nsp, 'ALLOW_SHADER_REGRESSIONS', False), self.assertRaises(nsp.BuildError) as e:
            self.check()
        self.assertIn('1 shaders that translated in b-old fail in b-new', str(e.exception))
        self.assertIn('[RED] b:', (self.logs / 'shader_regressions.log').read_text())

    def test_allowed(self):
        self.build('new', [('a', 'ok', ''), ('b', 'translation failed', FAIL)])
        with mock.patch.object(nsp, 'ALLOW_SHADER_REGRESSIONS', True):
            self.check()
        self.assertTrue(any(s.startswith('[RED] 1 shaders') for s in self.said), self.said)

    def test_none(self):
        self.build('new', [('a', 'ok', ''), ('b', 'ok', '')])
        self.check()
        self.assertEqual(self.said, [])


if __name__ == '__main__': unittest.main()
