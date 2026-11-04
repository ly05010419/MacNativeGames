"""The pipeline archive's cleanup of Metal's build folders: only the app's own folders, never while a process of the
app has a batch open (games/totk/tests/pipeline_archive/folders.m). No GPU, game or player data."""
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'


@unittest.skipUnless(sys.platform == 'darwin', 'Mac source probe')
class PipelineArchiveFolders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.work.name) / 'folders'
        r = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(GAME / 'tests/pipeline_archive/folders.m'),
                            '-framework', 'Foundation', '-o', str(cls.exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            cls.work.cleanup()
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, case):
        with tempfile.TemporaryDirectory() as directory:
            r = subprocess.run([str(self.exe), directory, case], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout, sorted(p.relative_to(directory).as_posix() for p in Path(directory).rglob('*'))

    def test_shared_folder_made_during_a_batch_is_kept(self):
        out, left = self.run_case('shared')
        self.assertIn('PASS: shared folder made during a batch kept', out)
        self.assertIn('shared/com.apple.gpuarchiver/gpuarchiver-other', left)

    def test_two_processes(self):
        out, left = self.run_case('peers')
        self.assertIn('PASS: two processes keep each other', out)
        self.assertIn('own/pipeline-archive.lock', left)   # never removed: both processes must lock the same file

    def test_no_batch_without_the_lock(self):
        out, left = self.run_case('nolock')
        self.assertIn('PASS: without the lock no batch begins', out)
        self.assertNotIn('own/com.apple.gpuarchiver/gpuarchiver-old', left)

    def test_no_batch_without_an_own_folder(self):
        out, _ = self.run_case('noown')
        self.assertIn('PASS: without an own cache folder no batch begins', out)

    def test_game_binaries_carry_their_bundle_id(self):
        """Without it a bare game binary's build folders land in the shared cache folder, which is never cleaned."""
        make = (GAME / 'Makefile').read_text().split('\n')
        self.assertIn('GAME_INFO := -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist', make)
        rules = [i for i, line in enumerate(make) if re.match(r'build/totk-native(-\w+)?:', line)]
        self.assertEqual(len(rules), 7)
        for i in rules:
            self.assertIn('runtime/game_info.plist', make[i].split(':', 1)[1].split(), make[i][:40])
            self.assertIn(' $(GAME_INFO) ', make[i + 1], make[i][:40])
        info = plistlib.loads((GAME / 'runtime/game_info.plist').read_bytes())
        app = re.search(r"^BUNDLE_ID = '([^']+)'", (ROOT / 'build_app.py').read_text(), re.M).group(1)
        self.assertEqual(info['CFBundleIdentifier'], 'org.macnativegames.totk-native')
        self.assertNotEqual(info['CFBundleIdentifier'], app)


if __name__ == '__main__':
    unittest.main()
