"""The dirty-page tracking's retirement race (nvn_dirty.c; nxnative af616f54): tests/nvn_dirty_retire_race.c compiled
against the real nvn_dirty.c, for Tears of the Kingdom's variant (the one fixed) and Breath of the Wild's (already
guarded). A pool retired between nvn_dirty_protect's lookup and its first page-state change must end untracked and
writable, and the retirement must have been held off by the slot mutex (the harness says which case it saw, with no
timed sleep deciding it). Runs on macOS with clang (mach, Darwin's mprotect behaviour); skipped elsewhere."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
DIRTY = REPO / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_dirty.c'
HARNESS = Path(__file__).with_name('nvn_dirty_retire_race.c')


def run_race(dirty, *defines):
    """(exit code, output) of the harness built against dirty with defines."""
    out = Path(tempfile.mkdtemp())
    try:
        exe = out / 'race'
        subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g', '-Wall', f'-DDIRTY_C="{dirty}"', *defines,
                        str(HARNESS), '-o', str(exe)], check=True, capture_output=True, text=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        return r.returncode, r.stdout + r.stderr
    finally:
        shutil.rmtree(out, ignore_errors=True)


@unittest.skipUnless(platform.system() == 'Darwin' and shutil.which('xcrun'), 'macOS with clang')
class RetireRace(unittest.TestCase):
    def test_tears_of_the_kingdom_holds_the_slot_mutex(self):
        code, out = run_race(DIRTY)
        self.assertEqual((code, out.strip().splitlines()), (0, ['the retirement was held off by the slot mutex', 'ok']),
                         out)

    def test_breath_of_the_wild_is_already_guarded(self):
        code, out = run_race(DIRTY, '-DGAME_BOTW=1')
        self.assertEqual((code, out.strip().splitlines()[-1]), (0, 'ok'), out)   # its own guard, either case


if __name__ == '__main__':
    unittest.main()
