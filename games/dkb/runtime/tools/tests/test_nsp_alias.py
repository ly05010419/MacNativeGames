"""NSP script: a title ID no profile has, whose main is a build a profile registers (a regional release of the same build, GitHub issue #114), packs from a profile derived
from that one (same_build, game.py by_build, derive_profile with the folder's title ID and main.npdm's program ID). No game files, keys or Xcode needed (temporary files only)."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools'))
import game, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

PROFILE, OTHER, PROGRAM = 0x0100AAAA00000000, 0x0100AAAA00FFF000, 0x0100AAAA00FFF000
BID, UPD_BID = 'ab' * 16, 'cd' * 16


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


class Alias(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        prof = self.tmp / 'games' / f'{PROFILE:016X}'; prof.mkdir(parents=True)
        (prof / 'game.tsv').write_text('\n'.join([f'title_id\t{PROFILE:016X}', f'program_id\t{PROFILE:016X}', 'name\tTest Game', 'modules\tmain',
                                                  f'build\tv0\t1.0.0\tmain={BID}', 'package\tv0', 'shader_versions\tv0', 'pad_layout\tfull']) + '\n')
        (prof / 'shader_map.tsv').write_text('# made again from the build\n')
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_by_build(self):
        self.assertEqual(game.by_build(BID).id, f'{PROFILE:016X}')
        self.assertIsNone(game.by_build(UPD_BID))
        self.assertIsNone(game.by_title(OTHER))

    def test_same_build_tries_the_update_then_the_base(self):
        seen = []
        def fake_run(cmd, log, env=None, cwd=None, on_line=None, ok=()):   # nx_extract: the update's main is another build, the base game's is the profile's
            seen.append(cmd[3]); nso(Path(cmd[4]) / 'ExeFS/main', UPD_BID if cmd[3] != '-' else BID)
        fam = {'base': self.tmp / 'base.nsp', 'updates': [(65536, self.tmp / 'upd.nsp')], 'dlc': []}
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', fake_run):
            g, ver, bid = nsp.same_build(self.tmp, OTHER, fam, self.tmp / 'prod.keys', REPO, {})
        self.assertEqual((g.id, ver, bid), (f'{PROFILE:016X}', 'v0', BID))
        self.assertEqual(seen, [fam['updates'][0][1], '-'])
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', lambda cmd, log, **k: nso(Path(cmd[4]) / 'ExeFS/main', UPD_BID)):
            self.assertIsNone(nsp.same_build(self.tmp, OTHER, fam, self.tmp / 'prod.keys', REPO, {}))

    def test_same_build_goes_on_past_a_sparse_base(self):
        seen = []
        def fake_run(cmd, log, env=None, cwd=None, on_line=None, ok=()):   # nx_extract: the update's main is another build; the sparse base alone exits 4 without a main
            seen.append((cmd[3], ok))
            if cmd[3] != '-': nso(Path(cmd[4]) / 'ExeFS/main', UPD_BID)
            elif 4 not in ok: raise nsp.BuildError('nx_extract exit code 4')
        fam = {'base': self.tmp / 'base.nsp', 'updates': [(65536, self.tmp / 'upd.nsp')], 'dlc': []}
        with mock.patch.object(nsp, 'build_nx_extract', lambda *a: 'nx'), mock.patch.object(nsp, 'run', fake_run):
            self.assertIsNone(nsp.same_build(self.tmp, OTHER, fam, self.tmp / 'prod.keys', REPO, {}))
        self.assertEqual([c for c, _ in seen], [fam['updates'][0][1], '-'])

    def test_derived_profile_takes_the_title_and_program_ids(self):
        d = self.tmp / 'extract/v0'; nso(d / 'ExeFS/main', BID)
        (d / 'ExeFS/main.npdm').write_bytes(b'META' + bytes(0x7c) + b'ACI0' + bytes(0xc) + struct.pack('<Q', PROGRAM) + bytes(0x20))
        g = game.load(f'{PROFILE:016X}'); out = self.tmp / 'derived' / g.id
        nsp.derive_profile(g, out, 'v0', d, ['main'], OTHER)
        h = game.Game(out)
        self.assertEqual((h.title_id, int(h.get('program_id'), 16)), (OTHER, PROGRAM))
        self.assertEqual((h.get('name'), h.get('pad_layout'), h.builds()['v0']['main']), ('Test Game', 'full', BID))
        self.assertFalse((out / 'shader_map.tsv').exists())   # made again from the build's own files
        nsp.derive_profile(g, out, 'v0', d, ['main'], PROFILE)   # the profile's own title: its IDs as they are
        self.assertEqual(game.Game(out).title_id, PROFILE)


if __name__ == '__main__':
    unittest.main()
