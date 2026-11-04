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
