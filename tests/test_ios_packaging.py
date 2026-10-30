"""games/totk/ios/tools/build_ipa.py: what an iOS build leaves out of the way and copies (2026-10-23)."""
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]/'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import build_ipa  # noqa: E402
import install_device  # noqa: E402


class Discard(unittest.TestCase):
    def test_a_folder_is_moved_aside_at_once_and_deleted_later(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); old = root/'TOTK.app'
            (old/'GameData/shader-seed').mkdir(parents=True)
            for i in range(50):
                (old/f'GameData/shader-seed/{i}.metallib').write_bytes(b'x')
            build_ipa.discard(old)
            self.assertFalse(old.exists())   # the path is free for the new build right away
            for _ in range(100):
                if not any(root.iterdir()):
                    break
                time.sleep(0.05)
            self.assertEqual(list(root.iterdir()), [])   # and the moved folder is gone once the background delete ran

    def test_a_missing_path_is_fine(self):
        with tempfile.TemporaryDirectory() as d:
            build_ipa.discard(Path(d)/'none')


class SeedBeside(unittest.TestCase):
    """--ios-game-data documents leaves the startup shaders beside the output; the README installs the .ipa, and the
    seed was pushed only for an .app folder, so the game said 'No shaders' after a successful install."""
    def layout(self, root, output):
        (root/'Tears of the Kingdom-shader-seed').mkdir()
        (root/'Tears of the Kingdom-shader-seed/fingerprint.txt').write_text('f' * 64)
        app = root/'signed/Payload/TOTK.app'; (app/'GameData').mkdir(parents=True)
        return root/output, app

    def test_an_ipa_gets_its_seed(self):
        with tempfile.TemporaryDirectory() as d:
            ipa, app = self.layout(Path(d), 'Tears of the Kingdom.ipa'); ipa.write_bytes(b'PK')
            self.assertEqual(install_device.seed_beside(ipa, app), Path(d)/'Tears of the Kingdom-shader-seed')

    def test_an_app_folder_gets_its_seed(self):
        with tempfile.TemporaryDirectory() as d:
            out, app = self.layout(Path(d), 'Tears of the Kingdom.app'); out.mkdir()
            self.assertEqual(install_device.seed_beside(out, out), Path(d)/'Tears of the Kingdom-shader-seed')

    def test_an_app_that_carries_its_seed_gets_none(self):
        with tempfile.TemporaryDirectory() as d:
            ipa, app = self.layout(Path(d), 'Tears of the Kingdom.ipa'); (app/'GameData/shader-seed').mkdir()
            self.assertIsNone(install_device.seed_beside(ipa, app))


def read_pack(parts):
    """ios/Sources/main.m seed_unpack, in Python: the files of a pack stream cut into parts."""
    data = b''.join(p.read_bytes() for p in parts); assert data[:9] == b'TOTKPACK1'; at, out = 9, {}
    while True:
        nl = int.from_bytes(data[at:at + 2], 'little'); at += 2
        if not nl: return out, at == len(data)
        name = data[at:at + nl].decode(); at += nl
        size = int.from_bytes(data[at:at + 8], 'little'); at += 8
        out[name] = data[at:at + size]; at += size


class ShaderPack(unittest.TestCase):
    def test_files_come_back_whole_across_part_boundaries(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); seed = root/'seed'; seed.mkdir(); parts = []
            files = {'a.metal': b'x' * 1000, 'b.json': b'{}', 'empty.variants': b'', 'c.metallib': bytes(range(256)) * 3}
            for n, b in files.items():
                (seed/n).write_bytes(b)
            install_device.write_pack(seed, sorted(files), root, 'shader-seed-x', 97, parts.append)
            self.assertGreater(len(parts), 10)   # cut in the middle of names, sizes and contents
            self.assertTrue(all(p.stat().st_size <= 97 for p in parts))
            got, exact = read_pack(parts)
            self.assertEqual(got, files)
            self.assertTrue(exact)   # nothing after the end mark



class FullHD(unittest.TestCase):
    """Settings › Full HD (docked): off by default, for both games. Tears of the Kingdom renders its scene at 1600x900
    (1.4.3 renders 960x540 on the phone otherwise); Breath of the Wild's docked branch renders 1920x1080 and its
    Settings pane says so (build_ipa.py)."""
    def test_switch_off_by_default_and_docked(self):
        import plistlib, re
        ios = TOOLS.parent
        specs = plistlib.loads((ios/'Settings.bundle/Root.plist').read_bytes())['PreferenceSpecifiers']
        switch = [s for s in specs if s.get('Key') == 'TOTK_DOCKED']
        self.assertEqual(len(switch), 1)
        self.assertIs(switch[0]['DefaultValue'], False)
        main = (ios/'Sources/main.m').read_text()
        # The switch is the pane's preset layer (main.m's apply_settings): it hands its own drawable and scene over
        # through settings_map.c's table, which is where the docked values live.
        self.assertIn('totk_preset_env_build([d boolForKey:@"TOTK_DOCKED"], &prenv)', main)
        for line in ('setenv("TOTK_IOS_DRAWABLE_H", prenv.drawable, 0)', 'setenv("TOTK_RENDER_RES", prenv.scene, 0)'):
            self.assertIn(line, main)
        preset = re.search(r'int totk_preset_env_build\(int docked.*?\n}', (ios/'Sources/settings_map.c').read_text(), re.S)
        self.assertIsNotNone(preset)
        table = preset.group(0) if preset else ''
        for value in ('"docked"', '"1080"', '"1600x900"'):
            self.assertIn(value, table)
        # The presets' picture is handed over once, when the game starts (apply_display_settings, after the start
        # screen's pick), and not while the custom layer is on (after PR #433).
        body = re.search(r'static void apply_display_settings\(void\) \{.*?\n}', main, re.S).group(0)
        self.assertIn('if ([d boolForKey:@"TOTK_CUSTOM_PICTURE"]) return;', body)
        self.assertIn('if (prenv.drawable) setenv("TOTK_IOS_DRAWABLE_H", prenv.drawable, 0);', body)
        self.assertEqual(main.count('totk_preset_env_build('), 1, 'the presets\' picture goes in one place')
        # Both games have the switch: Breath of the Wild's docked branch renders 1920x1080 too (ad35dfa), so no
        # game-specific exception is left in the helper. The only other operation-mode site is visionOS's default.
        self.assertNotIn('#ifndef GAME_BOTW', main)
        vision = 'setenv("TOTK_OPERATION_MODE","docked",1);'
        self.assertIn(vision, main)
        self.assertEqual(main.replace(vision, '').count('setenv("TOTK_OPERATION_MODE",'), 1,
                         'the other operation-mode site is the helper')
        build = (TOOLS/'build_ipa.py').read_text()
        self.assertIn("'      - path: Gen/Settings.bundle'", build)
        self.assertIn("s.get('Key')=='TOTK_DOCKED'", build)
        self.assertIn("specs[at-1]['FooterText']", build)
        self.assertNotIn('del specs[at-1:at+1]', build)

    def test_botw_pane_drops_totk_only_rows(self):
        import plistlib
        root = plistlib.loads((TOOLS.parent/'Settings.bundle/Root.plist').read_bytes())
        # The TOTKApp mark on each row is the only list of what Breath of the Wild must not show; if the marks were
        # dropped, the filtered pane would keep every row and the assertions below would pass vacuously.
        marked = {s['Key'] for s in root['PreferenceSpecifiers'] if s.get('TOTKApp') == 'totk'}
        self.assertTrue(marked, 'no row is marked TOTKApp=totk')
        build_ipa.settings_without_totk(root)
        specs = root['PreferenceSpecifiers']
        keys = {s['Key'] for s in specs if 'Key' in s}
        for key in marked:
            self.assertNotIn(key, keys)
        for key in ('TOTK_LANG', 'TOTK_PHONE_FPS', 'TOTK_FPS_OVERLAY', 'TOTK_SHADER_OVERLAY', 'TOTK_DEBUG_LOG'):
            self.assertIn(key, keys)
        # A group whose rows were all removed must not stay behind as an empty header.
        for i, spec in enumerate(specs):
            if spec.get('Type') == 'PSGroupSpecifier':
                self.assertLess(i + 1, len(specs), spec.get('Title'))
                self.assertNotEqual(specs[i+1].get('Type'), 'PSGroupSpecifier', spec.get('Title'))

    def test_export_scene_is_a_pane_value(self):
        # build_ipa.py embeds one scene size so the patch exists in the exported code; the pane's row must offer it, or
        # the embedded patch and the panel disagree about what the device may select.
        import plistlib, re
        build = (TOOLS/'build_ipa.py').read_text()
        match = re.search(r"TOTK_RENDER_RES='(\d+x\d+)'", build)
        self.assertIsNotNone(match, 'the export scene size is missing')
        values = plistlib.loads((TOOLS.parent/'Settings.bundle/Root.plist').read_bytes())
        scene = [s for s in values['PreferenceSpecifiers'] if s.get('Key') == 'TOTK_RENDER_RES'][0]
        self.assertIn(match.group(1) if match else '', scene['Values'])



class StartScreen(unittest.TestCase):
    """The start screen (ios/Sources/start_screen.m): the game starts from its button, the picture and frame rate are
    applied then (apply_display_settings in main.m), the picture row only where the Settings pane has Full HD."""
    def test_boots_from_the_start_screen_with_the_settings_applied_there(self):
        import re
        ios = TOOLS.parent
        main = (ios/'Sources/main.m').read_text()
        self.assertIn('#import "start_screen.h"', main)
        start = re.search(r'void \(\^start\)\(void\) = \^\{(.*?)\n    \};', main, re.S)
        self.assertTrue(start, 'the start screen before the boot')
        self.assertIn('TOTKStartView alloc] initWithFrame:self.view.bounds onStart:start]', main)
        # a script's run (TOTK_START_SCREEN=0) starts the same way without the screen (after PR #1174)
        self.assertRegex(main, r'getenv\("TOTK_START_SCREEN"\);\n    if \(screen && !strcmp\(screen, "0"\)\) start\(\);')
        body = start.group(1)
        self.assertLess(body.index('apply_display_settings()'), body.index('runtime_ios_sync_drawable'))
        self.assertLess(body.index('runtime_ios_sync_drawable'), body.index('boot]'))
        finish = main.index('- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:')
        self.assertNotIn('boolForKey:@"TOTK_DOCKED"', main[finish:], 'Full HD is applied when the game starts, not at launch')
        self.assertNotIn('stringForKey:@"TOTK_PHONE_FPS"', main[finish:])

    def test_shows_only_what_the_settings_pane_offers_and_writes_the_same_defaults(self):
        screen = (TOOLS.parent/'Sources/start_screen.m').read_text()
        self.assertIn('settingsPaneHas:@"TOTK_DOCKED"', screen)
        for key in ('forKey:@"TOTK_DOCKED"', 'forKey:@"TOTK_PHONE_FPS"'):
            self.assertIn(key, screen)
        self.assertNotIn('PLAYSHELF', screen.upper(), 'no launcher name on the game\'s screen')

if __name__ == '__main__':
    unittest.main()
