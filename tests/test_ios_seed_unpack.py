"""Exercise the production shader-pack unpacker with synthetic input and injected I/O failures."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'the unpacker uses macOS/iOS Foundation')
class IosSeedUnpack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('clang is required')
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = (ROOT / 'games/totk/ios/Sources/main.m').read_text()
        start = source.index('static NSString *seed_unpack(')
        end = source.index('\nstatic void *run_host', start)
        production = directory / 'seed_unpack.inc'
        production.write_text(source[start:end])
        cls.executable = directory / 'seed-unpack'
        build = subprocess.run(
            [compiler, '-fobjc-arc', '-fblocks', '-Wall', '-Wextra', '-Werror',
             '-Wno-incompatible-pointer-types', '-framework', 'Foundation',
             '-DPRODUCTION_UNPACKER="' + str(production) + '"',
             str(ROOT / 'tests/ios_seed_unpack_probe.m'), '-o', str(cls.executable)],
            capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    def run_case(self, case):
        with tempfile.TemporaryDirectory() as directory:
            run = subprocess.run([str(self.executable), case, directory],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('PASS ' + case, run.stdout)

    def test_valid_files_cross_pack_boundaries(self):
        self.run_case('valid_cross_part')

    def test_truncated_pack_keeps_previous_seed_and_retry_parts(self):
        self.run_case('truncated_stream')

    def test_bad_magic_keeps_previous_seed(self):
        self.run_case('bad_magic')

    def test_unsafe_entry_keeps_previous_seed(self):
        self.run_case('unsafe_entry')

    def test_allocation_failure_keeps_previous_seed_and_retry_parts(self):
        self.run_case('allocation_failure')

    def test_final_flush_failure_keeps_previous_seed_and_retry_parts(self):
        self.run_case('final_flush_failure')


if __name__ == '__main__':
    unittest.main()
