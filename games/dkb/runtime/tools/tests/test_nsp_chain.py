"""NSP script chain files (scripts/build_ipa_from_nsp.py chain_header, chain_tables): the one chain version header (dm/gfx/nvn/dmsh_chain.h) is written again when stale and left
alone when current, and the committed one is current; only the c1 / uniform extent tables that are missing (they are not committed) or do not name the library's stamp (its chain version
and profile inputs) are made (GitHub issue #78), none without a stamped library, and no step's reuse key counts them; the tools that make them read only the shader build's outputs
(an exFAT drive's AppleDouble `._<name>` beside each, PR #555); no game files, keys or Xcode needed (temporary files only in a
temporary folder)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)


class Header(Base):
    def setUp(self):   # a repository whose chain tools are the real ones (linked) and whose header is its own
        super().setUp()
        self.repo = self.tmp / 'repo'
        (self.repo / 'dm/gfx/nvn').mkdir(parents=True)
        (self.repo / 'dm/gfx/tools').symlink_to(REPO / 'dm/gfx/tools'); (self.repo / 'dm/ios').symlink_to(REPO / 'dm/ios')
        (self.repo / 'dm/third_party').symlink_to(REPO / 'dm/third_party')
        (self.repo / 'dm/gfx/nvn/dmsh_abi.h').symlink_to(REPO / 'dm/gfx/nvn/dmsh_abi.h')   # the shader library ABI, a chain input beside the header
        self.h = self.repo / nsp.CHAIN_H
        shutil.copy(REPO / nsp.CHAIN_H, self.h)
        self.env = {k: v for k, v in os.environ.items() if k != 'DM_GAME'}   # the version needs no game

    def header(self):
        nsp.chain_header(sys.executable, self.repo, self.env, self.tmp / 'logs')

    def test_committed_current(self):   # a chain change committed without dmsh_chain.py --header
        r = subprocess.run([sys.executable, str(REPO / 'dm/gfx/tools/dmsh_chain.py')], env=self.env, capture_output=True, text=True, check=True)
        self.assertEqual(nsp.chain_of(REPO), r.stdout.split()[-1], 'dm/gfx/nvn/dmsh_chain.h is stale: run dm/gfx/tools/dmsh_chain.py --header and commit it')

    def test_stale_rewritten(self):
        want = nsp.chain_of(REPO)
        self.h.write_text('#define DMSH_CHAIN "000000000000"\n')
        self.header()
        self.assertEqual(nsp.chain_of(self.repo), want)
        self.assertEqual(len(self.said), 1); self.assertIn('held 000000000000', self.said[0])

    def test_missing_written(self):
        want = nsp.chain_of(REPO)
        self.h.unlink()
        self.header()
        self.assertEqual(nsp.chain_of(self.repo), want)
        self.assertIn('held no version', self.said[0])

    def test_current_untouched(self):
        os.utime(self.h, (1, 1))
        self.header()
        self.assertEqual(self.h.stat().st_mtime, 1); self.assertEqual(self.said, [])


class Tables(Base):
    CHAIN, INPUTS = '0123456789ab', 'aaaaaaaaaaaa'

    def setUp(self):
        super().setUp()
        self.sh = self.tmp / 'shader'; self.lib = self.sh / 'cache' / f'b-{self.CHAIN}'; self.lib.mkdir(parents=True)
        (self.lib / 'inputs.tsv').write_text(f'# stamp\ngame\t010014E00DB56000\ninputs\t{self.INPUTS}\n')
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / n)
        self.ran = []
        cm = mock.patch.object(nsp, 'run', lambda cmd, log, env=None: self.ran.append(Path(cmd[1]).name)); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def tables(self, c1, cb):   # each a header line's "b-<chain> inputs <digest>" part (or any text), None for a missing table
        for n, head in (('c1_full.tsv', c1), ('cb_extent.tsv', cb)):
            if head: (self.tmp / n).write_text(f'# chain {head} 2026-10-23T00:00:00+00:00\n')
        return nsp.chain_tables(sys.executable, REPO, self.sh, {}, self.tmp / 'logs', self.g, self.CHAIN)

    def current(self): return f'b-{self.CHAIN} inputs {self.INPUTS}'

    def test_current_kept(self):
        self.assertTrue(self.tables(self.current(), self.current()))
        self.assertEqual(self.ran, []); self.assertEqual(self.said, [])

    def test_other_inputs_made_again(self):   # the profile's tables changed and a full build stamped the library again
        self.assertTrue(self.tables(self.current(), f'b-{self.CHAIN} inputs bbbbbbbbbbbb'))
        self.assertEqual(self.ran, ['cb_extent.py']); self.assertIn(f'named b-{self.CHAIN} inputs bbbbbbbbbbbb', self.said[0])

    def test_other_chain_made_again(self):
        self.assertTrue(self.tables(f'b-fc6c0368c7b9 inputs {self.INPUTS}', self.current()))
        self.assertEqual(self.ran, ['c1_guard.py'])

    def test_old_header_made_again(self):   # a table from before the stamps names only the chain
        self.assertTrue(self.tables(f'b-{self.CHAIN}', self.current()))
        self.assertEqual(self.ran, ['c1_guard.py'])

    def test_missing_made(self):   # a checkout's first pack: the tables are not in the repository
        self.assertTrue(self.tables(None, None))
        self.assertEqual(self.ran, ['c1_guard.py', 'cb_extent.py']); self.assertIn('c1_full.tsv missing', self.said[0])

    def test_no_stamp(self):   # no full build made these Mac outputs: build_shaders then builds again (reuse) or stops (after a build)
        (self.lib / 'inputs.tsv').unlink()
        self.assertFalse(self.tables(self.current(), self.current()))
        self.assertEqual(self.ran, [])

    def test_no_mac_outputs(self):
        shutil.rmtree(self.sh / 'cache')
        self.assertFalse(self.tables(None, None))
        self.assertEqual(self.ran, [])

    def test_not_profile_inputs(self):   # the steps' reuse keys leave out what the iOS shader step makes, and the rewriter's patches (the rewrite step's own key)
        for n in ('game.tsv', 'shader_map.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'patches.py'): (self.tmp / n).write_text('')
        self.assertEqual([p.name for p in nsp.profile_files(types.SimpleNamespace(dir=self.tmp))], ['game.tsv', 'shader_map.tsv'])


class TableTools(unittest.TestCase):
    def test_appledouble_files_are_not_shaders(self):   # macOS writes a binary ._<name> beside each file on an exFAT / FAT drive; c1_guard.py read ._<key>_b.metal as UTF-8 and stopped
        lib = Path(tempfile.mkdtemp()) / 'b-0123456789ab'; lib.mkdir(); self.addCleanup(shutil.rmtree, lib.parent)
        key = '00112233aabbccdd'
        (lib / f'{key}_b.metal').write_text('fragment float4 main0(constant cb& fp_c1) { int _5 = in.v; return (fp_c1)._m0[_5]; }\n')
        (lib / f'{key}_b.json').write_text(json.dumps({'bindings': [{'kind': 'cbuf', 'reg': 0, 'msl_name': 'fp_c1'}]}))
        for n in (f'{key}_b.metal', f'{key}_b.json', 'inputs.tsv'): (lib / ('._' + n)).write_bytes(b'\0\x05\x16\x07\0\x02\0\0' + b'\xa0' * 4088)   # not UTF-8, not JSON
        for tool, table in (('c1_guard.py', 'c1_full.tsv'), ('cb_extent.py', 'cb_extent.tsv')):
            with self.subTest(tool=tool):
                r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools' / tool, lib, lib.parent / table], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((lib.parent / 'c1_full.tsv').read_text().splitlines()[1:], [key])   # the shader itself is still checked


if __name__ == '__main__':
    unittest.main()
