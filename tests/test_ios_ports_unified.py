"""Every port with an iPhone / iPad app is built with the Zelda app and tools (user 2026-11-20: "统一 统一 统一！！！ 这是规范
要求！！！ 用测试保证 不靠记忆！"): one build (games/totk/ios/tools/build_port_ipa.py), one app (games/totk/ios/Sources with
its GAME_<NAME> branches), one iOS front end (shared/runtime/frontend_ios.m), one installer. A port's games/<game>/ios holds
its profile (tools/build_ipa.py) and its answers to the shell (Sources/ios_front.m), nothing else."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORTS = {'acnh': 'GAME_ACNH', 'odyssey': 'GAME_ODYSSEY'}
SHARED_BUILD = ROOT / 'games/totk/ios/tools/build_port_ipa.py'


def files(folder):
    return {p.name for p in folder.iterdir() if p.is_file() and p.name != '.DS_Store'} if folder.is_dir() else set()


class Ports(unittest.TestCase):
    def test_only_a_profile_and_its_answers(self):
        for game in PORTS:
            with self.subTest(game=game):
                ios = ROOT / 'games' / game / 'ios'
                self.assertEqual(files(ios / 'tools'), {'build_ipa.py'})
                self.assertEqual(files(ios / 'Sources'), {'ios_front.m'})
                self.assertEqual({p.name for p in ios.iterdir() if p.name not in ('.DS_Store', '__pycache__')}, {'tools', 'Sources'})

    def test_profiles_call_the_shared_build(self):
        for game, define in PORTS.items():
            with self.subTest(game=game):
                text = (ROOT / 'games' / game / 'ios/tools/build_ipa.py').read_text()
                self.assertIn('import build_port_ipa as port_ipa', text)
                self.assertIn(f"define='{define}'", text)
                self.assertIn(f"game='{game}'", text)
                self.assertIn('port_ipa.cli(PORT)', text)
                self.assertNotIn('xcodebuild', text, 'the build is the shared one')

    def test_one_front_end(self):
        shared = ROOT / 'shared/runtime/frontend_ios.m'
        for game in PORTS:
            with self.subTest(game=game):
                link = ROOT / 'games' / game / 'runtime/frontend_ios.m'
                self.assertTrue(link.is_symlink() and link.resolve() == shared.resolve())
        self.assertIn('CFBundleDisplayName', shared.read_text(), 'the boot title is the app\'s own name')

    def test_the_shell_knows_every_port(self):
        features = (ROOT / 'games/totk/ios/Sources/port_features.h').read_text()
        settings = (ROOT / 'games/totk/ios/Sources/picture_settings.h').read_text()
        for game, define in PORTS.items():
            with self.subTest(game=game):
                self.assertIn(f'defined({define})', features)
                self.assertRegex(settings, rf'defined\({define}\)\s*\n#define PP_THIS_GAME PP_GAME_{game.upper()}')
        self.assertIn("('totk','botw','acnh','odyssey')", (ROOT / 'games/totk/ios/tools/build_ipa.py').read_text().replace(' ', ''),
                      'pane_for gives every port the picture rows')

    def test_shaders_outside_the_app_as_zeldas(self):
        # user 2026-11-21: "为什么 安装 这么慢 … 这个不是标准吗 为什么又发明一遍 ？？？ 写测试保证": with --image-outside
        # the port build leaves the shaders beside the app (<out>-shader-seed with fingerprint.txt), which Zelda's
        # installer pushes once as image parts; the app carries no shader-seed or shader-cache of its own
        build = SHARED_BUILD.read_text()
        self.assertIn("seed_out = out.with_name(out.stem + '-shader-seed')", build)
        self.assertIn("stamp = out / 'fingerprint.txt'", build)
        installer = (ROOT / 'games/totk/ios/tools/install_device.py').read_text()
        self.assertIn("seed = ipa.with_name(ipa.stem + '-shader-seed')", installer, 'the installer looks where the build puts it')
        i = build.index('if a.image_outside:\n        # The shaders beside the app')
        outside = build[i:build.index('    else:', i)]
        self.assertNotIn("res / 'shader-seed'", outside)
        self.assertNotIn("res / 'shader-cache'", outside)

    def test_device_libraries_as_zeldas(self):
        # user 2026-11-21: "为什么 ipad在 预热着色器， 看看塞尔达 如何做的 统一 用测试保证": the port seed gets the device
        # .metallib files from Zelda's compile_seed.py (the game loads them instead of compiling on the phone), in both
        # packagings, and never the Mac's (macOS) libraries
        build = SHARED_BUILD.read_text()
        self.assertIn("from compile_seed import seed_flags", build)
        self.assertIn("TOOLS / 'compile_seed.py', out, 'iphoneos'", build)
        self.assertEqual(build.count("seed_with_mac_cache(L.seed, cache, seed_out, run,"), 2, 'outside and inside the app')
        self.assertNotRegex('a' * 64 + '-' + 'b' * 12 + '_b.metallib', __import__('re').compile(
            __import__('re').search(r"SEED_NAME = re.compile\(r'(.*?)'\)", build).group(1)))
        for game in PORTS:
            gfx = (ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text()
            self.assertIn('totk_rt_dir(), pc]', gfx, f'{game}: its shader loader takes the device library from the cache')
            self.assertIn('.metallib", totk_rt_dir()', gfx, game)

    def test_damaged_device_index_does_not_refuse_the_seed(self):
        # an empty .variants a crash left in the iPad's cache made the import refuse the whole seed (ACNH, 2026-11-21)
        ports = (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h').read_text()
        self.assertIn('if (existing && ![user isKindOfClass:NSArray.class]) { user = nil; existing = nil; }', ports)
        self.assertNotIn('(existing && ![user isKindOfClass:NSArray.class])) {', ports)

    def test_merged_seed_names_pass_the_import(self):
        # the Mac cache keeps seed-installed.json beside its shaders; merged into the seed, the import refused it and the
        # game started with no shaders (iPad, 2026-11-21)
        import importlib.util, json, re, shutil, tempfile
        spec = importlib.util.spec_from_file_location('build_port_ipa', SHARED_BUILD)
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        safe = re.search(r'regularExpressionWithPattern:\s*@"(.*?)"', (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h').read_text()).group(1).replace('\\\\', '\\')
        with tempfile.TemporaryDirectory() as d:
            seed, cache, out = Path(d, 'seed'), Path(d, 'cache'), Path(d, 'out')
            seed.mkdir(); cache.mkdir()
            (seed / 'seed-manifest.json').write_text(json.dumps({'files': {}}))
            shader = 'a' * 64 + '-' + 'b' * 12 + '_b.metal'
            (cache / shader).write_text('x'); (cache / 'seed-installed.json').write_text('{}')
            mod.seed_with_mac_cache(seed, cache, out)
            names = json.loads((out / 'seed-manifest.json').read_text())['files']
            self.assertEqual(set(names), {shader})
            for n in names: self.assertRegex(n, safe)

    def test_one_seed_reader(self):
        # the image parts are read by the shared totk_seed_image.h, the ports' import is one shared file
        image = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h'
        ports = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/totk_shader_seed_ports.h'
        self.assertIn('#include "totk_seed_image.h"', ports.read_text())
        self.assertIn('seed_image_data(', ports.read_text())
        for game in ('totk', *PORTS):
            link = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/totk_seed_image.h'
            self.assertTrue(link.is_symlink() and link.resolve() == image.resolve(), game)
        for game in PORTS:
            link = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/totk_shader_seed.h'
            self.assertTrue(link.is_symlink() and link.resolve() == ports.resolve(), game)

    def test_every_port_has_its_icon(self):
        # user 2026-11-21: "Odyssey 没有 icon？": its Mac build keeps the icon in app-assets, not build/AppIcon.icns
        self.assertIn("icon=work / 'app-assets/OdysseyIcon.jpg'", (ROOT / 'games/odyssey/ios/tools/build_ipa.py').read_text())
        self.assertIn("L.icon or L.build / 'AppIcon.icns'", SHARED_BUILD.read_text())

    def test_runtimes_answer_the_shell(self):
        for game in PORTS:
            with self.subTest(game=game):
                rt = ROOT / 'games' / game / 'runtime'
                self.assertIn('void runtime_audio_output_restart(int recreate)', (rt / 'audio_mixer.c').read_text())
                self.assertIn('TOTK_ROMFS_DATA', (rt / 'romfs.c').read_text(), 'the image in Documents/GameData (install_device.py --game-data)')
                front = (ROOT / 'games' / game / 'ios/Sources/ios_front.m').read_text()
                for name in ('runtime_phone_fps', 'runtime_phone_fps_set', 'runtime_display_max_hz'):
                    self.assertRegex(front, rf'\b{name}\(', name)


if __name__ == '__main__':
    unittest.main()
