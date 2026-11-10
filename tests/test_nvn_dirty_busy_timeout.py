"""Forced BUSY-timeout sites in nvn_dirty.c: tests/nvn_dirty_busy_timeout.c compiled against the real
nvn_dirty.c, for Tears of the Kingdom and Breath of the Wild. With -DBUSY_WAIT_MS=1 and -DITERS=10 each
site times out 10 times (40 timeouts); the red log prints only the first 8 process-wide totals (then
counts silently), naming the site. Runs on macOS with clang (mach timing); skipped elsewhere."""
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
DIRTY = REPO / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_dirty.c'
HARNESS = Path(__file__).with_name('nvn_dirty_busy_timeout.c')
RED = '【红】'


def run_busy(dirty, *defines):
    """(exit code, output) of the harness built against dirty with defines."""
    out = Path(tempfile.mkdtemp())
    try:
        exe = out / 'busy'
        subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g', '-Wall', f'-DDIRTY_C="{dirty}"',
                        '-DBUSY_WAIT_MS=1', '-DITERS=10', *defines, str(HARNESS), '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        return r.returncode, r.stdout + r.stderr
    finally:
        shutil.rmtree(out, ignore_errors=True)


def assert_red_pattern(test, out, sites):
    reds = [ln for ln in out.splitlines() if RED in ln]
    test.assertEqual(len(reds), 8, out)
    for site in sites:
        test.assertTrue(any(site in ln for ln in reds), f'missing site {site} in:\n{out}')
    totals = [int(m.group(1)) for ln in reds for m in [re.search(r'total #(\d+)', ln)] if m]
    test.assertEqual(totals, [1, 2, 3, 4, 5, 6, 7, 8], out)


@unittest.skipUnless(platform.system() == 'Darwin' and shutil.which('xcrun'), 'macOS with clang')
class BusyTimeout(unittest.TestCase):
    def test_tears_of_the_kingdom_sites(self):
        code, out = run_busy(DIRTY)
        self.assertEqual(code, 0, out)
        assert_red_pattern(self, out, ('on_fault', 'prep_write', 'gpu_write_begin', 'nvn_dirty_protect'))

    def test_breath_of_the_wild_sites(self):
        code, out = run_busy(DIRTY, '-DGAME_BOTW=1')
        self.assertEqual(code, 0, out)
        assert_red_pattern(self, out, ('on_fault', 'prep_write', 'gpu_write_begin', 'protect_seg'))


if __name__ == '__main__':
    unittest.main()
