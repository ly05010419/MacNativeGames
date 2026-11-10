"""Real folder-sync transactions: separate user identities, backups and invalid remote metadata."""
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
A, B = 'a' * 32, 'b' * 32


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation requires macOS')
class CloudSave(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temp.name) / 'probe'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', str(ROOT / 'tests/cloud_save_probe.m'),
                        '-framework', 'Foundation', '-o', str(cls.exe)], check=True, timeout=90)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_save(self, cloud, profile, uid, mode):
        run = subprocess.run([str(self.exe), str(cloud), str(profile), uid, mode],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_users_keep_separate_remote_state_and_backups(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            legacy = cloud / 'Saves/current/progress.sav'
            legacy.parent.mkdir(parents=True); legacy.write_bytes(b'legacy')
            for uid in (A, B):
                self.run_save(cloud, profile, uid, 'push')
                current = cloud / 'Users' / uid / 'Saves/current'
                self.assertEqual((current / 'progress.sav').read_text(), uid)
                meta = plistlib.loads((current / 'sync.plist').read_bytes())
                self.assertEqual(meta['uid'], uid)
                self.assertEqual(meta['gen'], 1)
                self.assertTrue((profile / f'cloud-sync/{uid}.plist').exists())
            self.assertEqual(legacy.read_bytes(), b'legacy')
            other_device = root / 'other'
            self.run_save(cloud, other_device, A, 'pull')
            self.assertEqual((other_device / f'save/{A}/progress.sav').read_text(), A)
            backups = list((other_device / f'cloud-backups/{A}').glob('*/progress.sav'))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_text(), 'local copy')
            self.assertFalse((other_device / f'cloud-backups/{B}').exists())

    def test_wrong_remote_user_is_neither_downloaded_nor_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            self.run_save(cloud, profile, A, 'push')
            current = cloud / f'Users/{A}/Saves/current'
            path = current / 'sync.plist'; meta = plistlib.loads(path.read_bytes())
            meta['uid'] = B; path.write_bytes(plistlib.dumps(meta))
            before = {p.name: p.read_bytes() for p in current.iterdir()}
            other = root / 'other'
            self.run_save(cloud, other, A, 'mismatch')
            self.assertEqual((other / f'save/{A}/progress.sav').read_text(), 'local copy')
            self.assertEqual({p.name: p.read_bytes() for p in current.iterdir()}, before)
            self.assertFalse((other / f'cloud-sync/{A}.plist').exists())

    def test_unchanged_save_never_goes_over_a_newer_one_that_did_not_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            self.run_save(cloud, profile, A, 'push')
            current = cloud / f'Users/{A}/Saves/current'
            good = (current / 'progress.sav').read_bytes()
            (current / 'progress.sav').write_bytes(b'half')   # still syncing: the files do not match sync.plist's hash
            other = root / 'other'
            run = subprocess.run([str(self.exe), str(cloud), str(other), A, 'mismatch'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('stay local', run.stderr)
            self.assertEqual((current / 'progress.sav').read_bytes(), b'half')
            self.assertEqual(plistlib.loads((current / 'sync.plist').read_bytes())['gen'], 1)
            (current / 'progress.sav').write_bytes(good)   # the download finished
            self.run_save(cloud, other, A, 'pull')
            self.assertEqual((other / f'save/{A}/progress.sav').read_bytes(), good)
            backups = list((other / f'cloud-backups/{A}').glob('*/progress.sav'))
            self.assertEqual([b.read_text() for b in backups], ['local copy'])

    def test_new_save_after_a_failed_load_stays_local_until_the_next_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            self.run_save(cloud, profile, A, 'push')
            current = cloud / f'Users/{A}/Saves/current'
            good = (current / 'progress.sav').read_bytes()
            (current / 'progress.sav').write_bytes(b'half')
            other = root / 'other'
            run = subprocess.run([str(self.exe), str(cloud), str(other), A, 'played'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertNotIn('saved to the sync folder', run.stderr)
            self.assertEqual((current / 'progress.sav').read_bytes(), b'half')
            self.assertEqual(plistlib.loads((current / 'sync.plist').read_bytes())['gen'], 1)
            (current / 'progress.sav').write_bytes(good)   # the download finished
            self.run_save(cloud, other, A, 'pull')
            self.assertEqual((other / f'save/{A}/progress.sav').read_bytes(), good)
            backups = list((other / f'cloud-backups/{A}').glob('*/progress.sav'))
            self.assertEqual(sorted(b.read_text() for b in backups), ['local copy'])

    def test_conflict_copies_stay_while_the_folder_does_not_match_its_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            self.run_save(cloud, profile, A, 'push')
            current = cloud / f'Users/{A}/Saves/current'
            (current / 'progress.sav').write_bytes(b'half')
            (current / 'progress 2.sav').write_bytes(b'may hold the real data')
            run = subprocess.run([str(self.exe), str(cloud), str(profile), A, 'pull'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('conflict copies kept', run.stderr)
            self.assertEqual((current / 'progress 2.sav').read_bytes(), b'may hold the real data')

    def test_icloud_conflict_copies_do_not_block_a_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cloud, profile = root / 'cloud', root / 'profile'
            self.run_save(cloud, profile, A, 'push')
            current = cloud / f'Users/{A}/Saves/current'
            (current / 'progress 2.sav').write_bytes(b'the other device')   # iCloud keeps a crossed upload's file so
            other = root / 'other'
            self.run_save(cloud, other, A, 'pull')
            self.assertEqual((other / f'save/{A}/progress.sav').read_text(), A)
            self.assertFalse((other / f'save/{A}/progress 2.sav').exists())
            self.assertFalse((current / 'progress 2.sav').exists())   # cleaned from the folder too
            (current / 'progress 2.sav').write_bytes(b'again')
            self.run_save(cloud, other, A, 'pull')   # in sync now: still cleaned
            self.assertFalse((current / 'progress 2.sav').exists())
            self.assertEqual((current / 'progress.sav').read_text(), A)
