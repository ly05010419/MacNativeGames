"""Animal Crossing's iPhone / iPad app is the Zelda ports' app, not a copy (user 2026-11-20: "塞尔达的 设置 都移植过来
要统一", "塞尔达不就这样吗 你为什么又发明一遍", "统一 统一 统一！！！ 这是规范 要求！！！ 用测试保证 不靠记忆！"):
games/acnh/ios holds only its build script and its answers to what the shared app asks of a runtime; the app's sources,
the start screen, picture settings, touch controls, Settings pane, embed, dependency, Xcode and install tools are those
of games/totk/ios, with GAME_ACNH where the game differs. A new file there that copies one of them fails here."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ACNH = ROOT / 'games/acnh/ios'
ZELDA = ROOT / 'games/totk/ios'
BUILD = (ACNH / 'tools/build_ipa.py').read_text()


def files(folder):
    return {p.name for p in folder.iterdir() if p.is_file() and p.name != '.DS_Store'} if folder.is_dir() else set()


class OnlyItsOwnFiles(unittest.TestCase):
    def test_tools_are_zeldas(self):
        self.assertEqual(files(ACNH / 'tools'), {'build_ipa.py'},
                         'gen_embed / build_deps / xcode_dir / install_device: use games/totk/ios/tools')

    def test_sources_are_zeldas(self):
        self.assertEqual(files(ACNH / 'Sources'), {'ios_front.m'},
                         'the app (main.m, start screen, touch controls, ...) is games/totk/ios/Sources')
        self.assertFalse((ACNH / 'Settings.bundle').exists(), 'the Settings pane is made from games/totk/ios/Settings.bundle')

    def test_nothing_named_like_a_zelda_or_dread_file(self):
        theirs = set()
        for game in ('totk', 'dread', 'oot', 'tmnt'):
            for sub in ('Sources', 'tools'):
                theirs |= files(ROOT / 'games' / game / 'ios' / sub)
        mine = files(ACNH / 'Sources') | files(ACNH / 'tools')
        self.assertEqual(mine & theirs - {'Info.plist', 'build_ipa.py'}, set(), 'a copy of another port\'s iOS file')


class BuiltFromTheSharedApp(unittest.TestCase):
    def test_compiles_the_shell_and_uses_the_tools(self):
        # the shared port build (games/totk/ios/tools/build_port_ipa.py) with this game's profile
        self.assertIn('import build_port_ipa as port_ipa', BUILD)
        self.assertIn("define='GAME_ACNH'", BUILD)
        self.assertIn('port_ipa.cli(PORT)', BUILD)
        shared = (ROOT / 'games/totk/ios/tools/build_port_ipa.py').read_text()
        self.assertIn("SHELL = TOTK / 'ios/Sources'", shared)
        self.assertIn("APP = 'TOTK'", shared, 'TOTK.app, which games/totk/ios/tools/install_device.py signs and installs')
        for tool in ('gen_embed.py', 'build_deps.py'):
            self.assertIn(f"TOOLS / '{tool}'", shared, tool)
        self.assertIn('pane_for(', shared)
        self.assertIn('--image-outside', shared)

    def test_the_shell_knows_the_game(self):
        main = (ZELDA / 'Sources/main.m').read_text()
        self.assertGreaterEqual(main.count('GAME_ACNH'), 5)
        self.assertIn('PP_GAME_ACNH', (ZELDA / 'Sources/picture_settings.h').read_text())
        self.assertIn("'acnh'", (ZELDA / 'tools/build_ipa.py').read_text(), 'pane_for knows the game')
        self.assertIn('--alt-text', (ZELDA / 'tools/gen_embed.py').read_text(), 'the 60 FPS text')
        self.assertIn('--vorbis', (ZELDA / 'tools/build_deps.py').read_text(), 'libogg and libvorbis')

    def test_its_runtime_reads_what_the_shell_hands_over(self):
        romfs = (ROOT / 'games/acnh/runtime/romfs.c').read_text()
        self.assertIn('TOTK_ROMFS_DATA', romfs, 'the image in Documents/GameData (install_device.py --game-data)')
        front = (ROOT / 'games/acnh/runtime/frontend_ios.m').read_text()
        for name in ('runtime_ios_attach_layer', 'runtime_ios_sync_drawable', 'runtime_ios_sync_safe_area'):
            self.assertRegex(front, rf'\bvoid {name}\(', name)
        loader = (ROOT / 'games/acnh/runtime/loader.c').read_text()
        self.assertIn('x0alt', loader, 'the 60 FPS text gen_embed.py --alt-text embeds')



class TallScreenUI(unittest.TestCase):
    def test_ui_keeps_its_shape_on_a_screen_taller_than_16_9(self):
        # user 2026-11-21: "动森 在ipad上的全屏 是拉伸 不是 像iphone一样是扩展？": on the iPad (4:3) the camera already takes
        # the screen's shape; the UI must be flattened about the centre (below 1), as it is narrowed on the iPhone (above 1)
        gfx = (Path(__file__).resolve().parents[1] / 'games/acnh/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
        # 0 is the game's own 16:9 (no aspect override), not a squeeze: taken as one it set the UI viewport's height to 0
        self.assertIn('return f > 1.001 || (f > 0.0 && f < 0.999) ? f : 1;', gfx)
        self.assertIn('if (ux < 1) { uy = ux; ux = 1; }', gfx)
        self.assertIn('S.vp[0][3] * vs * uy', gfx)

if __name__ == '__main__':
    unittest.main()
