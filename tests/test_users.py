"""The local users store (runtime/account.c): migration, add / choose, restart and corrupt files."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Users(unittest.TestCase):
    def test_users_store(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'users'
            build = subprocess.run(
                [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/test_users.c'), str(ROOT / 'games/totk/runtime/account.c'),
                 '-I' + str(ROOT / 'games/totk/runtime'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: users', run.stdout)

    def test_odyssey_links_the_shared_store(self):
        """Super Mario Odyssey builds the same file (shared/runtime/account.c, 2026-11-17) with GAME_ODYSSEY from its
        game_version.h: the whole store, its profile of before kept as the user "Local", and only its own account entries."""
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        link = ROOT / 'games/odyssey/runtime/account.c'
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.resolve(), (ROOT / 'shared/runtime/account.c').resolve())
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'users'
            build = subprocess.run(
                [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-DTEST_ODYSSEY=1',
                 str(ROOT / 'tests/test_users.c'), str(link), '-I' + str(ROOT / 'games/odyssey/runtime'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: users', run.stdout)
            self.assertIn("pass: odyssey's profile of before", run.stdout)


if __name__ == '__main__':
    unittest.main()
