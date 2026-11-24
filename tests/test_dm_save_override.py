"""Verify optional DM_SAVE override across all five tuned-runtime variants (TOTK, BotW, DKB, Pokopia, OoT)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExternalSaveOverrideTests(unittest.TestCase):
    def test_all_five_tuned_variants_support_dm_save_override(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        variants = [
            ('totk', ROOT / 'games/totk/runtime', []),
            ('botw160', ROOT / 'games/totk/runtime', ['-DGAME_BOTW=1', '-DGAME_BOTW160=1']),
            ('botw190', ROOT / 'games/totk/runtime', ['-DGAME_BOTW=1', '-DGAME_BOTW190=1']),
            ('dkb', ROOT / 'games/totk/runtime', ['-DGAME_DKB=1', '-DGAME_NS2=1']),
            ('pkp', ROOT / 'games/totk/runtime', ['-DGAME_POKOPIA=1', '-DGAME_NS2=1']),
            ('oot', ROOT / 'games/oot/runtime', ['-DGAME_OOT=1', '-DGAME_NS2=1']),
        ]
        with tempfile.TemporaryDirectory() as directory:
            for name, runtime_dir, defines in variants:
                with self.subTest(variant=name):
                    exe = Path(directory) / f'test-{name}'
                    cmd = [
                        compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        *defines,
                        '-I' + str(runtime_dir),
                        str(ROOT / 'tests/test_dm_save_override.c'),
                        str(runtime_dir / 'fs_host.c'),
                        str(runtime_dir / 'account.c'),
                        '-o', str(exe),
                    ]
                    build = subprocess.run(cmd, capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: dm_save_override', run.stdout)


if __name__ == '__main__':
    unittest.main()
