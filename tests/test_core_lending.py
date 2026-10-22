"""Breath of the Wild's core lending (BOTW_CORE_TIMESLICE_MS) against the real threads.c, without a game: a polling
priority-44 owner lends its core to a priority-46 waiter only with the setting on and only where the game allows it."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


class CoreLending(unittest.TestCase):
    def test_lends_only_when_on_and_where_allowed(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / 'core-lending'
            build = subprocess.run(
                [compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-DGAME_BOTW=1',
                 '-Ithird_party/d2r_gfx/gfx/nvn/include', 'tests/core_lending/probe.c', 'runtime/threads.c',
                 'runtime/thread_enter.S', '-o', str(probe)], cwd=TOTK, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for setting, lends in ((None, False), ('0', False), ('2', True)):
                env = {k: v for k, v in os.environ.items() if k != 'BOTW_CORE_TIMESLICE_MS'}
                if setting is not None:
                    env['BOTW_CORE_TIMESLICE_MS'] = setting
                run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=60, env=env)
                self.assertEqual(run.returncode, 0, run.stderr[-2000:])
                report = json.loads(run.stdout.strip().splitlines()[-1])
                self.assertEqual(report, {'lend': lends, 'hold': False}, setting)


if __name__ == '__main__':
    unittest.main()
