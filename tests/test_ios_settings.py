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

    def test_two_layers_are_one_choice(self):
        # One switch selects the layer (main.m's TOTK_CUSTOM_PICTURE): the presets (Full HD, frame rate) are handed over
        # when it is off, the custom picture and frame rows when it is on. Neither layer may write the other's keys, so
        # both rows sets must stay in the pane and the switch must default to the presets.
        rows = self._rows()
        self.assertIn('TOTK_CUSTOM_PICTURE', rows)
        self.assertEqual(rows['TOTK_CUSTOM_PICTURE']['Type'], 'PSToggleSwitchSpecifier')
        self.assertIs(rows['TOTK_CUSTOM_PICTURE']['DefaultValue'], False)
        self.assertIn('TOTK_DOCKED', rows)
        self.assertIn('TOTK_PHONE_FPS', rows)
        for key in ('TOTK_IOS_PICTURE_MODE', 'TOTK_RENDER_RES', 'TOTK_FPS_CAP'):
            self.assertIn(key, rows)
        executable = self._helper()
        for value in rows['TOTK_PHONE_FPS']['Values']:
            with self.subTest(key='TOTK_PHONE_FPS', value=value):
                run = subprocess.run([str(executable), '--preset-fps', value], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, f'TOTK_PHONE_FPS={value}: {run.stdout}{run.stderr}')
        # The key -> layer table is the one main.m filters by; a row that moves layers without the table following it
        # would be handed over (or dropped) by the wrong layer.
        for key, layer in {'TOTK_DOCKED': 'preset', 'TOTK_PHONE_FPS': 'preset',
                           'TOTK_IOS_PICTURE_MODE': 'custom', 'TOTK_RENDER_RES': 'custom',
                           'TOTK_FPS_CAP': 'custom', 'TOTK_FPS_OVERLAY': 'always',
                           'TOTK_ASPECT': 'always'}.items():
            with self.subTest(key=key, layer=layer):
                run = subprocess.run([str(executable), '--layer', key], capture_output=True, text=True)
                self.assertEqual(run.stdout.strip(), layer, key)

    def test_aspect_ratio_row_offers_only_valid_values(self):
        # aspect_parse takes "" (the game's own framing), "0" (16:9) and explicit ratios; "display" was dropped with
        # fitting the window (runtime/aspect_math.h) and would make the game runtime_fail, so the pane must not offer it.
        rows = self._rows()
        row = rows['TOTK_ASPECT']
        self.assertEqual(row['DefaultValue'], '')
        self.assertIn('', row['Values'])
        self.assertIn('0', row['Values'])
        self.assertNotIn('display', row['Values'])
        self.assertEqual(len(row['Values']), len(row['Titles']))

    def test_scene_values_are_sizes(self):
        # build_ipa.py embeds one of these sizes in the exported patch, and main.m rejects a stored value the row does
        # not list; both parse <width>x<height>.
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
