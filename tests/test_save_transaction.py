"""Compile games/totk/runtime/fs_host.c's save transactions into a test and run it without game files, keys or saves."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SaveTransaction(unittest.TestCase):
    def test_commit_crash_recovery_and_full_disk(self):
        if sys.platform != 'darwin' or not shutil.which('xcrun'):
            self.skipTest('the save host uses clonefile and renamex_np (macOS)')
        with tempfile.TemporaryDirectory(prefix='totk-save-transaction-') as directory:
            executable = Path(directory) / 'test-save-transaction'
            scratch = Path(directory) / 'scratch'
            scratch.mkdir()
            build = subprocess.run(
                ['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                 str(ROOT / 'tests/test_save_transaction.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable), str(scratch)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('22 save transaction cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
