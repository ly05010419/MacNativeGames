"""The Finder button resolves the same account folder as the host save filesystem."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'The host filesystem uses macOS APIs')
class SaveFolder(unittest.TestCase):
    def test_custom_profile_and_account_directory(self):
        with tempfile.TemporaryDirectory(prefix='save-profile test ') as temp:
            binary = pathlib.Path(temp) / 'save-folder-probe'
            subprocess.run(['clang', '-std=c11', '-D_DARWIN_C_SOURCE', '-ffunction-sections',
                            '-fdata-sections', '-Wl,-dead_strip',
                            '-I', str(ROOT / 'games/totk/runtime'),
                            str(ROOT / 'tests/save_folder_probe.c'),
                            str(ROOT / 'games/totk/runtime/fs_host.c'), '-o', str(binary)], check=True)
            run = subprocess.run([str(binary), str(pathlib.Path(temp) / 'profile')],
                                 capture_output=True, text=True, check=True)
            self.assertIn('save root override: ok', run.stdout)
