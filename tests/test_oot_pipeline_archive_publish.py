"""Publishing pipeline archive batches when processes share one pipelines/ folder (games/oot/tests/pipeline_archive/
publish.m, totk_pipeline_archive_publish.h): real processes and threads, no GPU, game or player data."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'   # Ocarina of Time's copy of the archive headers (from main 22deb11f, fec7e3b7)
CASES = {
    'same': 'two writers under one name keep their own batches',
    'midload': 'a batch read half published survives and is complete',
    'fail': "a failed writer leaves only the other writer's batch",
    'stale': 'a start alone removes what writes cut short',
    'threads': "one thread finishing leaves the other's batch guarded",
    'wipe': 'the folder is wiped only once no one publishes, the lock file stays',
}


@unittest.skipUnless(sys.platform == 'darwin', 'Mac source probe')
class PipelineArchivePublish(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.work.name) / 'publish'
        r = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(GAME / 'tests/pipeline_archive/publish.m'),
                            '-framework', 'Foundation', '-o', str(cls.exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            cls.work.cleanup()
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_cases(self):
        for case, said in CASES.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                r = subprocess.run([str(self.exe), directory, case], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn('PASS: ' + said, r.stdout)
                self.assertTrue((Path(directory) / 'pipelines.lock').is_file())   # never removed


if __name__ == '__main__':
    unittest.main()
