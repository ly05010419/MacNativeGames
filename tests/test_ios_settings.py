"""The iPad Settings pane's mode -> environment mapping, without UIKit or a device."""
import plistlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PLIST = ROOT / 'games/totk/ios/Settings.bundle/Root.plist'


class IOSSettings(unittest.TestCase):
    def _helper(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory)
        executable = directory / 'ios-settings'
        build = subprocess.run(
            [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I', str(ROOT / 'games/totk/ios/Sources'),
             str(ROOT / 'tests/test_ios_settings.c'),
             str(ROOT / 'games/totk/ios/Sources/settings_map.c'),
             '-o', str(executable)],
            capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        return executable

    def _rows(self):
        with open(PLIST, 'rb') as file:
            root = plistlib.load(file)
        return {item['Key']: item for item in root['PreferenceSpecifiers'] if 'Key' in item}

    def test_picture_and_frame_modes(self):
        run = subprocess.run([str(self._helper())], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('ios settings ok', run.stdout)

    def test_pane_values_are_accepted(self):
        executable = self._helper()
        rows = self._rows()
        for key, flag in (('TOTK_IOS_PICTURE_MODE', '--picture'), ('TOTK_FPS_CAP', '--frame-cap')):
            self.assertIn(key, rows)
            row = rows[key]
            self.assertIn('Values', row)
            self.assertIn('Titles', row)
            self.assertIn('DefaultValue', row)
            values = row['Values']
            self.assertTrue(values, key)
            self.assertEqual(len(values), len(row['Titles']), key)
            self.assertEqual(len(values), len(set(values)), key)
            for value in values:
                with self.subTest(key=key, value=value):
                    run = subprocess.run([str(executable), flag, value], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, f'{key}={value}: {run.stdout}{run.stderr}')
        run = subprocess.run([str(executable), '--defaults'], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        picture_default, frame_default = run.stdout.splitlines()
        self.assertEqual(picture_default, rows['TOTK_IOS_PICTURE_MODE']['DefaultValue'])
        self.assertEqual(frame_default, rows['TOTK_FPS_CAP']['DefaultValue'])

    def test_scene_values_are_sizes(self):
        # build_ipa.py reads this row to generate the HeapDef overlay of every scene the pane can select, and main.m
        # rejects a stored value the row does not list; both parse <width>x<height>.
        row = self._rows()['TOTK_RENDER_RES']
        values = row['Values']
        self.assertIn(row['DefaultValue'], values)
        self.assertEqual(len(values), len(row['Titles']))
        for value in values:
            with self.subTest(value=value):
                width, height = (int(part) for part in value.lower().split('x'))
                self.assertGreaterEqual(width, 64)
                self.assertGreaterEqual(height, 64)

    def test_no_interpolation_switch(self):
        # MetalFX interpolation costs more GPU than the frames it replaces on this device and a downscaled output is
        # not accepted by the interpolator, so the pane must not offer a switch that can only hurt.
        self.assertNotIn('TOTK_MFX_INTERP', self._rows())

    def test_overlays_are_separate_switches(self):
        # The runtime reads the two overlays independently; a single combined mode would be a third state the player
        # cannot see.
        rows = self._rows()
        self.assertNotIn('TOTK_HUD_MODE', rows)
        for key in ('TOTK_FPS_OVERLAY', 'TOTK_SHADER_OVERLAY'):
            self.assertIn(key, rows)
            self.assertEqual(rows[key]['Type'], 'PSToggleSwitchSpecifier')
            self.assertIs(rows[key]['DefaultValue'], True)


    def test_every_row_is_applied_at_launch(self):
        # main.m registers every TOTK_* row that has a DefaultValue and hands it to the runtime as an environment
        # variable; a row without one, or a key without the prefix, would show in Settings and change nothing.
        rows = self._rows()
        self.assertTrue(rows)
        for key, row in rows.items():
            with self.subTest(key=key):
                self.assertTrue(key.startswith('TOTK_'), key)
                self.assertIn('Type', row, key)
                self.assertIn('DefaultValue', row, key)


if __name__ == '__main__':
    unittest.main()
