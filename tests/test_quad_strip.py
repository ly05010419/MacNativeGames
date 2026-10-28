"""Check the quad-strip geometry used for Hero's Path without game data or Metal."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QuadStrip(unittest.TestCase):
    def test_geometry(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        # AddressSanitizer can spin forever in its own start-up on some macOS installs (2026-10-28: 27 min in
        # __asan::AsanInitInternal, before main); a probe that has not finished in 60 s is built again with the
        # undefined-behaviour checks only, so the geometry is still checked and the suite cannot hang (after PR #1216).
        with tempfile.TemporaryDirectory() as directory:
            for sanitizers in ('address,undefined', 'undefined'):
                executable = Path(directory) / f'quad-strip-{sanitizers.replace(",", "-")}'
                build = subprocess.run([
                    compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    f'-fsanitize={sanitizers}', str(ROOT / 'tests/test_quad_strip.c'),
                    '-o', str(executable)], capture_output=True, text=True, timeout=120)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                try:
                    run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
                except subprocess.TimeoutExpired:
                    if sanitizers == 'undefined':
                        self.fail('the quad-strip probe did not finish in 60 s')
                    continue
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass', run.stdout)
                return
