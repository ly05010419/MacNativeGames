"""The iPhone / iPad picture profiles (games/totk/ios/Sources/picture_profile.c), built and run on the Mac without UIKit:
the fixed values of each profile, the scene held at the mode's canvas and the environment the runtime reads."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'games/totk/ios/Sources'


def build(test, *defines):
    """The test binary (tests/test_picture_profile.c with picture_profile.c), or skip without a C compiler."""
    compiler = shutil.which('cc')
    if not compiler:
        test.skipTest('A C compiler is required')
    directory = Path(tempfile.mkdtemp())
    test.addCleanup(shutil.rmtree, directory)
    executable = directory / 'picture-profile'
    run = subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', *defines, '-I', str(SOURCES),
                          str(ROOT / 'tests/test_picture_profile.c'), str(SOURCES / 'picture_profile.c'), '-o', str(executable)],
                         capture_output=True, text=True)
    test.assertEqual(run.returncode, 0, run.stdout + run.stderr)
    return executable


def keys(test):
    """The profile keys and the MetalFX keys, as the model names them."""
    run = subprocess.run([str(build(test)), '--keys'], capture_output=True, text=True)
    test.assertEqual(run.returncode, 0, run.stdout + run.stderr)
    profiles, metalfx = run.stdout.splitlines()
    return profiles.split(), metalfx.split()


class PictureProfile(unittest.TestCase):
    def test_profiles_scene_and_environment(self):
        run = subprocess.run([str(build(self))], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('picture profiles ok', run.stdout)

    def test_keys_are_the_stored_values(self):
        # The Settings pane and the app store these words; renaming one would orphan every saved choice.
        self.assertEqual(keys(self), (['quality', 'performance', 'economy', 'custom'], ['off', 'scene', 'screen']))


if __name__ == '__main__':
    unittest.main()
