"""NSP script --work-dir / WORK_DIR: the build folder is the folder given, as it is, and the IPA (or Mac app) defaults to the folder holding it; without it the
build folder is <title ID>_build in the NSP folder and the IPA is in the NSP folder. The provisional profile goes in <build folder>/profile (copied once from the
default place when --work-dir is new). The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

TID = 0x010015100B514000   # a game with a profile in dm/games


def profile_ipa():
    profile = game.by_title(TID)
    assert profile, 'no profile for the test game'
    return profile.get('ipa')


class Stop(Exception): pass


class WorkDir(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.tmp = Path(td.name).resolve()
        self.nsps = self.tmp / 'nsps'; self.nsps.mkdir()
        (self.tmp / 'prod.keys').write_text('test')

    def plan(self, *opts, setting=''):
        """What main decides before it extracts anything: DM_ROOT (the build folder) and the IPA path the console reports; self.nsz_work is the build folder
        the NSZ conversions were given (None: the default)"""
        fam = {'base': self.nsps / 'base.nsp', 'updates': [], 'dlc': []}
        args = nsp.parse_args([str(self.nsps), '-k', str(self.tmp / 'prod.keys'), *opts])
        said, seen = [], {}
        def stop(env, *a): seen.update(env); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'scan_nsps', return_value=(TID, fam)), \
             mock.patch.object(nsp, 'decompress_nsz', return_value=fam) as nsz, mock.patch.object(nsp, 'nsp_versions', return_value={'v0': fam['base']}), \
             mock.patch.object(nsp, 'configure_dotnet'), mock.patch.object(nsp, 'WORK_DIR', setting), mock.patch.object(nsp, 'check_prereqs', side_effect=stop), \
             mock.patch.object(nsp, 'say', side_effect=said.append):
            with self.assertRaises(Stop): nsp.main(args)
        self.nsz_work = nsz.call_args.args[5]
        ipa = next(m.split(': ', 1)[1] for m in said if m.startswith('output IPA: '))
        return Path(seen['DM_ROOT']), Path(ipa)

    def test_default(self):
        ipa = profile_ipa()
        self.assertEqual(self.plan(), (self.nsps / f'{TID:016X}_build', self.nsps / ipa))
        self.assertIsNone(self.nsz_work)

    def test_option_moves_the_build_folder_and_the_default_ipa(self):
        ipa = profile_ipa()
        out = self.tmp / 'builds' / 'mine'
        self.assertEqual(self.plan('--work-dir', str(out)), (out, out.parent / ipa))
        self.assertEqual(self.nsz_work, out)   # the NSZ conversions are build files too

    def test_setting_does_the_same(self):
        ipa = profile_ipa()
        out = self.tmp / 'elsewhere'
        self.assertEqual(self.plan(setting=str(out)), (out, out.parent / ipa))
        self.assertEqual(self.nsz_work, out)

    def test_out_names_the_ipa_whatever_the_build_folder(self):
        out, ipa = self.plan('--work-dir', str(self.tmp / 'w'), '-o', str(self.tmp / 'x.ipa'))
        self.assertEqual((out, ipa), (self.tmp / 'w', self.tmp / 'x.ipa'))

    def test_profile_folder(self):
        args = lambda *o: nsp.parse_args([str(self.nsps), *o])
        default = self.nsps / f'{TID:016X}_build' / 'profile'
        with mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROFILE_OUT', ''), mock.patch.object(nsp, 'say'):
            self.assertEqual(nsp.profile_folder(args(), self.nsps, TID), default)
            work = self.tmp / 'w'
            self.assertEqual(nsp.profile_folder(args('--work-dir', str(work)), self.nsps, TID), work / 'profile')
            self.assertFalse(work.exists())   # no profile to migrate: nothing is made
            elsewhere = self.tmp / 'p'
            self.assertEqual(nsp.profile_folder(args('--work-dir', str(work), '--profile-out', str(elsewhere)), self.nsps, TID), elsewhere)
            # the setting does the same as the option
            with mock.patch.object(nsp, 'WORK_DIR', str(work)):
                self.assertEqual(nsp.profile_folder(args(), self.nsps, TID), work / 'profile')

    def test_profile_migrates_once_and_keeps_the_original(self):
        old = self.nsps / f'{TID:016X}_build' / 'profile' / f'{TID:016X}'; old.mkdir(parents=True)
        (old / 'game.tsv').write_text('old')
        work = self.tmp / 'w'
        with mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROFILE_OUT', ''), mock.patch.object(nsp, 'say'):
            got = nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(work)]), self.nsps, TID)
            self.assertEqual(got, work / 'profile')
            self.assertEqual((got / f'{TID:016X}' / 'game.tsv').read_text(), 'old')
            self.assertEqual((old / 'game.tsv').read_text(), 'old')   # the original stays
            (got / f'{TID:016X}' / 'game.tsv').write_text('edited'); (old / 'game.tsv').write_text('newer old')
            nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(work)]), self.nsps, TID)
            self.assertEqual((got / f'{TID:016X}' / 'game.tsv').read_text(), 'edited')   # a profile there is not overwritten
            other = self.tmp / 'x'; (other / 'profile').mkdir(parents=True)
            nsp.profile_folder(nsp.parse_args([str(self.nsps), '--work-dir', str(other)]), self.nsps, TID)
            self.assertEqual(list((other / 'profile').iterdir()), [])   # a folder that already has a profile is left alone


if __name__ == '__main__':
    unittest.main()
