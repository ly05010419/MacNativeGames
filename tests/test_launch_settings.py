"""The Mac launcher's settings window (games/totk/app/launch_settings.h): checks, precedence and when it opens by itself,
for each kind of launcher build_app.py makes; and every setting it offers is still read by the runtime."""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
BUILDS = {'1.0.0': [], '1.1.2': ['-DTOTK_GAME_V112=1'], '1.2.1': ['-DTOTK_GAME_V121=1'], '1.4.3': ['-DTOTK_GAME_V143=1'],
          'botw': ['-DGAME_BOTW=1', '-DGAME_BOTW160=1']}


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class LaunchSettingsTests(unittest.TestCase):
    def test_probe(self):
        for build, defines in BUILDS.items():
            with self.subTest(build=build), tempfile.TemporaryDirectory() as tmp:
                probe = Path(tmp) / 'probe'
                compile_ = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', *defines, f'-I{TOTK}/app',
                                           f'-I{TOTK}/runtime', str(TOTK / 'tests/launch_settings/probe.c'),
                                           str(TOTK / 'runtime/motion_fusion.c'), '-o', str(probe)], capture_output=True, text=True)
                if compile_.returncode:
                    self.skipTest(f'clang could not build the probe: {compile_.stderr[-500:]}')
                run = subprocess.run([str(probe)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(json.loads(run.stdout)['status'], 'pass')

    def test_every_setting_is_read_by_the_runtime(self):
        names = re.findall(r'^\s*\{"(TOTK_[A-Z0-9_]+)", ', (TOTK / 'app/launch_settings.h').read_text(), re.M)
        self.assertGreater(len(names), 15)
        sources = [p.read_text(errors='replace') for d in ('runtime', 'third_party/d2r_gfx/gfx/nvn')
                   for p in (TOTK / d).rglob('*') if p.suffix in ('.c', '.h', '.m')]
        for name in names:
            with self.subTest(name=name):
                self.assertTrue(any(f'getenv("{name}")' in s or f'env_on("{name}")' in s for s in sources), name)


if __name__ == '__main__':
    unittest.main()
