"""The GPU probes (make probe-gpu-*) run the version whose game code is on this Mac (2026-11-15).

They named 1.0.0 only (build/totk-native rewritten/v0), so a Mac holding 1.4.3 could not run them, and
probe-gpu-drv-c0 (the indirect dispatch check behind #2190) went unrun for ten days. PROBE_ROOT names a work folder's
copy of games/totk; PROBE_VERSION picks the version, else the first of v0, v121, v143 with game code there. These tests
read the commands make would run (make -n) and the version check; nothing is built or run on the GPU.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'


def place(root, *versions, data=True):
    for v in versions:
        (root / 'rewritten' / v).mkdir(parents=True, exist_ok=True)
        (root / 'rewritten' / v / 'main.patched.bin').write_bytes(b'')
        if data:
            (root / 'build/game-data' / v).mkdir(parents=True, exist_ok=True)
            (root / 'build/game-data' / v / 'runtime-tables.bin').write_bytes(b'')


def make(*args, dry=True):
    # OPUS_PREFIX / VPX_PREFIX given, so make does not ask tools/deps to build the codecs
    command = ['make', '-s'] + (['-n'] if dry else []) + list(args) + ['OPUS_PREFIX=/opus', 'VPX_PREFIX=/vpx']
    return subprocess.run(command, cwd=TOTK, capture_output=True, text=True, timeout=60)


def probe_line(output):
    return next(line for line in output.splitlines() if line.rstrip().endswith('probe-gpu-drv-c0 > analysis/native-probe-gpu-drv-c0.json'))


class GpuProbeVersion(unittest.TestCase):
    def test_a_work_folder_holding_143_runs_the_143_runtime_on_its_game_code_and_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            place(root, 'v143')
            run = make('probe-gpu-drv-c0', f'PROBE_ROOT={root}/')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            line = probe_line(run.stdout)
            self.assertIn(f'TOTK_GAME_DATA="{root}/build/game-data/v143"', line)
            self.assertIn(f'build/totk-native-v143 "{root}/rewritten/v143" probe-gpu-drv-c0', line)
            self.assertNotIn('rewritten/v0', line)

    def test_the_first_version_present_is_taken_and_a_named_one_wins(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            place(root, 'v0', 'v143')
            line = probe_line(make('probe-gpu-drv-c0', f'PROBE_ROOT={root}/').stdout)
            self.assertIn(f'build/totk-native "{root}/rewritten/v0"', line)
            line = probe_line(make('probe-gpu-drv-c0', f'PROBE_ROOT={root}/', 'PROBE_VERSION=v143').stdout)
            self.assertIn(f'build/totk-native-v143 "{root}/rewritten/v143"', line)

    def test_a_version_without_game_data_in_a_work_folder_is_passed_over(self):
        """This Mac's work folder: 1.0.0's code without its game data, 1.4.3 with both (make end to end picked 1.0.0)."""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            place(root, 'v0', data=False)
            place(root, 'v143')
            line = probe_line(make('probe-gpu-drv-c0', f'PROBE_ROOT={root}/').stdout)
            self.assertIn(f'build/totk-native-v143 "{root}/rewritten/v143"', line)

    def test_no_game_code_or_no_game_data_stops_before_anything_is_built(self):
        """Checked while the Makefile is read: with make -j the build ran beside a checking recipe."""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            run = make('probe-gpu-drv-c0', '-j8', f'PROBE_ROOT={root}/')
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('GPU probes need a version', run.stdout + run.stderr)
            self.assertNotIn('clang', run.stdout)
            place(root, 'v143', data=False)
            run = make('probe-gpu-drv-c0', '-j8', f'PROBE_ROOT={root}/')
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('GPU probes need a version', run.stdout + run.stderr)
            self.assertNotIn('clang', run.stdout)
            run = make('probe-gpu-drv-c0', '-j8', f'PROBE_ROOT={root}/', 'PROBE_VERSION=v143')
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('no game data', run.stdout + run.stderr)
            self.assertNotIn('clang', run.stdout)
            place(root, 'v143')
            run = make('probe-gpu-version', f'PROBE_ROOT={root}/', dry=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('GPU probe: v143', run.stdout)


if __name__ == '__main__':
    unittest.main()
