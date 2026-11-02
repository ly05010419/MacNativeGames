"""Compile and exercise Metroid Dread's RomFS path normalizer without game files or keys."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DreadRomfsPaths(unittest.TestCase):
    def test_native_path_parser(self):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('A C compiler is required for the native path regression')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'test-dread-romfs-paths'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/test_dread_romfs_paths.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('21 Dread RomFS path cases', result.stdout)


if __name__ == '__main__':
    unittest.main()
