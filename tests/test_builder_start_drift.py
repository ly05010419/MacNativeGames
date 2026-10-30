"""Drift (S5.1, P1 and gap 6): every value tools/builder/start_options.py can pass or write is one the game's own
parser takes, read from the runtime's sources in this repository, and the app names and data folders the Settings tab
looks for are build_app.py's. A change in the runtime that drops a value fails here, not at a player's start."""
import ast
from pathlib import Path
import re
import unittest

from tools.builder.games import GAMES
from tools.builder.start_options import ENV, FILE, HOLD_ENV, OPTIONS, PANEL, PICTURE_WORDS

REPO = Path(__file__).resolve().parents[1]
NVN = REPO / 'games/totk/third_party/d2r_gfx/gfx/nvn'
RUNTIME = REPO / 'games/totk/runtime'


def source(path):
    return path.read_text()


class Drift(unittest.TestCase):
    def test_the_panels_files_and_words(self):
        """The window writes the settings panel's own files (after PR #1184), and only words its restore takes:
        settings_panel.h settings_restore_begin reads graphics-frame.txt interp-<MFX_*>, graphics-output.txt an
        OUTPUT_MODE_* number, graphics-taa.txt 0 / 1 and graphics-aspect.txt a ratio in 0.5..8."""
        panel = source(RUNTIME / 'settings_panel.h')
        restore = panel[panel.index('static void settings_restore_begin'):]
        restore = restore[:restore.index('\n}\n')]
        for name, _ in PANEL.values():
            self.assertIn(f'"{name}"', restore)
        self.assertIn('"interp-%d %c"', restore)
        modes = re.search(r'enum \{ MFX_OFF, (.*?) \}', source(NVN / 'totk_mfx_mode.h'))[1].split(', ')
        number = {name: i + 1 for i, name in enumerate(modes)}
        self.assertEqual({c: w for c, w in PANEL['interpolation'][1].items()},
                         {'interp30': f'interp-{number["MFX_30"]}', 'interp': f'interp-{number["MFX_50"]}',
                          'interpmax': f'interp-{number["MFX_MAX"]}'})
        outputs = re.search(r'enum \{ (OUTPUT_MODE_OFF.*?) \}', source(RUNTIME / 'runtime.h'))[1].split(', ')
        self.assertEqual(PANEL['picture'][1], {'metalfx': str(outputs.index('OUTPUT_MODE_METALFX')),
                                               'native-2k': str(outputs.index('OUTPUT_MODE_NATIVE'))})
        self.assertIn('(v==0||v==1))taa_set(v)', restore.replace(' ', ''))
        self.assertIn('ratio>=0.5f&&ratio<=8', restore.replace(' ', ''))
        self.assertAlmostEqual(float(PANEL['aspect'][1]['21:9']), 21 / 9, places=6)

    def test_play_passes_only_what_the_panel_lacks(self):
        self.assertEqual(ENV, {('ground', '16'): {'TOTK_ANISO': '16'}})
        self.assertEqual({o.id for v in OPTIONS.values() for o in v if o.how != FILE}, {'ground'})
        self.assertIn('if (!e || !*e || !strcmp(e, "0")) v = MFX_OFF;', source(NVN / 'totk_mfx.m'))   # the held start
        self.assertEqual(HOLD_ENV, {'interpolation': {'TOTK_MFX': '0'}})

    def test_anisotropy_is_clamped_to_16(self):
        self.assertIn('aniso > 16 ? 16', source(NVN / 'nvn_mtl.m'))
        self.assertEqual(ENV[('ground', '16')], {'TOTK_ANISO': '16'})

    def test_1_4_3_picture_words_are_the_games(self):
        code = source(RUNTIME / 'game_patches_v143.c')
        restore = code[code.index('static void picture_mode_restore'):]
        restore = restore[:restore.index('\n}\n')]
        for word in set(PICTURE_WORDS) - {'auto'}:   # picture-mode.txt is only read now (the panel's file is written)
            self.assertIn(f'"{word}"', restore)
        self.assertIn('getenv("TOTK_PICTURE_MODE")', restore)

    def test_botw_1_9_0_has_no_native_2k(self):
        code = source(RUNTIME / 'game_patches_botw.c')
        self.assertIn('int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }', code)
        picture = next(o for o in OPTIONS['botw190'] if o.id == 'picture')
        self.assertEqual(picture.choices, ('auto', 'metalfx'))

    def test_version_folders_are_the_runtimes(self):
        header = source(RUNTIME / 'game_version.h')
        for version in OPTIONS:
            self.assertIn(f'#define TOTK_GAME_DIR "{version}"', header)

    def test_app_names_and_data_folders_are_build_apps(self):
        tree = ast.parse(source(REPO / 'build_app.py'))
        apps = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == 'APPS' for t in n.targets))
        for game_id, spec in GAMES.items():
            name, bundle, folder = apps[game_id]
            self.assertEqual((spec.app_name, spec.bundle_id, spec.data_folder), (name, bundle, folder))
        self.assertTrue(re.search(r'"Library/Application Support/MacNativeGames/" GAME_FOLDER',
                                  source(REPO / 'games/totk/app/launcher.m')))
        self.assertIn('stringByAppendingPathComponent:@"picture-mode.txt"', source(REPO / 'games/totk/app/launcher.m'))


if __name__ == '__main__':
    unittest.main()
