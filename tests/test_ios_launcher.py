"""The iPhone / iPad MNG Launcher (tools/ios_launcher) and the games' iOS apps agree on the URL schemes.

The Launcher sees a game as installed only when canOpenURL answers for its scheme, which iOS allows only for schemes in
LSApplicationQueriesSchemes, and the game's app must declare that scheme (CFBundleURLTypes, from build_ipa.py url_scheme).
A scheme missing on either side shows an installed game as not installed, with nothing in any log.
"""
import importlib.util
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / 'tools/ios_launcher'
BUILDS = {'totk': ROOT / 'games/totk/ios', 'oot': ROOT / 'games/oot/ios'}


def load(path, name):
    if str(path.parent) not in sys.path:   # build_ipa.py imports its neighbours (compile_seed)
        sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def launcher_games():
    text = (LAUNCHER / 'Sources/Games.swift').read_text()
    return re.findall(r'"([a-z0-9]+)"', re.search(r'static let all = \[([^\]]*)\]', text)[1])


def queried_schemes():
    text = (LAUNCHER / 'project.yml').read_text()
    block = text.split('LSApplicationQueriesSchemes:', 1)[1]
    return re.findall(r'^\s+- (mng-[a-z0-9]+)\s*$', block.split('settings:', 1)[0], re.M)


class Schemes(unittest.TestCase):
    def test_every_launcher_game_is_queried(self):
        games = launcher_games()
        # every game with an iPhone / iPad build in the repository (user 2026-11-19: "支持游戏 都加上")
        self.assertEqual(set(games), {'oot', 'totk', 'botw', 'dkb', 'pkp', 'dread', 'tmnt'})
        self.assertEqual(sorted('mng-' + g for g in games), sorted(queried_schemes()))
        ipa = load(LAUNCHER / 'install.py', 'ios_launcher_install_games')
        self.assertEqual(set(ipa.GAMES), set(games))
        self.assertEqual(set(ipa.MAC_BUNDLES.values()), set(games))   # every case can get its picture

    def test_every_launcher_game_is_built_with_its_scheme(self):
        # Dread and TMNT write their Info.plist themselves: the scheme is literal there
        made = set(re.findall(r'^\s+- (mng-[a-z]+)\s*$', (ROOT / 'games/dread/ios/tools/build_ipa.py').read_text(), re.M))
        made |= set(re.findall(r'^\s+- (mng-[a-z]+)\s*$', (ROOT / 'games/tmnt/ios/project.yml').read_text(), re.M))
        self.assertEqual(made, {'mng-dread', 'mng-tmnt'})
        for name, folder in BUILDS.items():
            ipa = load(folder / 'tools/build_ipa.py', f'build_ipa_{name}')
            for version in ('v0', 'v121', 'v143', 'botw160', 'botw190', 'dkb', 'pkp', 'oot'):
                made.add(ipa.url_scheme(version))
            self.assertEqual(ipa.url_scheme('v143'), 'mng-totk')
            self.assertEqual(ipa.url_scheme('botw190'), 'mng-botw')
            self.assertEqual(ipa.url_scheme('oot'), 'mng-oot')
        for game in launcher_games():
            self.assertIn('mng-' + game, made)

    def test_the_games_apps_declare_the_scheme(self):
        for name, folder in BUILDS.items():
            spec = (folder / 'project.yml').read_text()
            self.assertRegex(spec, r'CFBundleURLTypes:.*\n(?:.*\n){0,3}\s+- \$\{TOTK_URL_SCHEME\}', name)
            script = (folder / 'tools/build_ipa.py').read_text()
            self.assertIn('TOTK_URL_SCHEME=url_scheme(a.version)', script, name)


class SharedCaseViews(unittest.TestCase):
    """The iPhone Launcher compiles the Mac Launcher's case views as they are (install.py SHARED): a Mac-only call
    outside `#if os(macOS)` breaks the iPhone build, found only when someone installs it."""

    MAC_ONLY = re.compile(r'\b(NSEvent|NSImage|NSApp|NSWorkspace|NSColor|NSView|NSScreen|NSCursor|import AppKit)\b')

    def test_shared_files_use_mac_calls_only_under_macos(self):
        ipa = load(LAUNCHER / 'install.py', 'ios_launcher_install')
        for name in ipa.SHARED:
            depth, mac = 0, []
            for line in (ipa.MAC_UI / name).read_text().splitlines():
                code = line.split('//', 1)[0]
                s = code.strip()
                if s.startswith('#if'):
                    mac.append('os(macOS)' in s)
                elif s.startswith('#else') and mac:
                    mac[-1] = not mac[-1]
                elif s.startswith('#endif') and mac:
                    mac.pop()
                elif self.MAC_ONLY.search(code) and not any(mac):
                    self.fail(f'{name}: Mac-only call outside #if os(macOS): {line.strip()}')

    def test_mac_home_page_draws_the_shared_front(self):
        home = (LAUNCHER.parents[0] / 'builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('CoverFront(game: game', home)
        ios = (LAUNCHER / 'Sources/LauncherApp.swift').read_text()
        # a tap turns the middle case over and opens it, as on the Mac ("手机 点击 打开 效果 没有了")
        for view in ('CoverFlow(', 'CoverFront(', 'CaseSpine(', 'CaseBack(', 'launches: launches'):
            self.assertIn(view, ios)
        self.assertNotIn('onOpen:', ios)
        self.assertIn('arrows: false', ios)          # "左右箭头不要"
        self.assertNotIn('.saturation(', ios)        # "不要变黑 正常颜色"
        self.assertNotIn('notInstalled', ios)        # "去掉 还没安装 提示"
        # "为什么 这么卡": no picture is read from its file while drawing; the blur is made once (CoverImages)
        self.assertNotIn('contentsOfFile', ios)
        self.assertIn('backdrop: cover?.backdrop', ios)
        # the Mac draws its cases the same way (user 2026-11-19: "mac 也要这样优化吗" "改")
        self.assertIn('backdrop: art?.backdrop', home)
        self.assertNotIn('NSImage(contentsOfFile:', home)
        self.assertIn('final class CoverImages', (LAUNCHER.parents[0] / 'builder_app/Sources/MNGBuilder/CoverArt.swift').read_text())

    def test_landscape_only(self):   # user 2026-11-19: "默认 横版 不要竖版"
        spec = (LAUNCHER / 'project.yml').read_text()
        for key in ('UISupportedInterfaceOrientations:', 'UISupportedInterfaceOrientations~ipad:'):
            block = spec.split(key, 1)[1].split('\n        U', 1)[0]
            self.assertIn('UIInterfaceOrientationLandscapeLeft', block)
            self.assertNotIn('Portrait', block)

    def test_mac_cover_flow_still_turns_over(self):   # onOpen is optional: the Mac passes none and keeps its back
        flow = (LAUNCHER.parents[0] / 'builder_app/Sources/MNGBuilder/CoverFlow.swift').read_text()
        self.assertIn('var onOpen: ((Int) -> Void)? = nil', flow)
        home = (LAUNCHER.parents[0] / 'builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertNotIn('onOpen:', home)


if __name__ == '__main__':
    unittest.main()
