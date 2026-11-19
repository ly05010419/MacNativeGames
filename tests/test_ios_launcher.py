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

    def test_the_mac_case_opens_like_a_book_and_can_turn_instead(self):
        # user 2026-11-19: "launcher 盒子 翻转 改成 盒子 打开" (the Mac only), "旧方案 保留 可以切换"
        mac = LAUNCHER.parents[0] / 'builder_app/Sources/MNGBuilder'
        detail = (mac / 'CaseDetail.swift').read_text()
        self.assertIn('enum CaseOpening: String, CaseIterable {\n    case book, turn', detail)
        self.assertIn('anchor: .leading', detail)   # the cover swings about the spine on its left edge
        # "从中间 打开 就像真实 打开盒子": the case keeps its shape and its spine ends at the stage's middle, the two
        # halves the same size
        book = detail[detail.index('private func book(p: CGFloat)'):detail.index('private func opened(')]
        self.assertIn('let hinge = hinge0 + (stage.width / 2 - hinge0) * p', book)
        self.assertIn('let openW = openH * stage.w / stage.h', book)
        self.assertNotIn('coverShare', book)
        # the photo of an opened case ("这样效果"): one picture across both halves, clear plastic, the cartridge
        # the cartridge in the middle of the left half ("放在 左边 中间")
        self.assertIn('Self.cartridge(inside, width: openW * 0.405, height: openW * 0.405 * 1.36)', book)   # "大50%"
        # "右边的 翻页 一直在 抖动": both halves are laid out once at the opened size and only scaled while the case
        # grows, never laid out anew at each frame's size (type stepping a whole pixel against the moving picture)
        self.assertIn('tray(width: openW, height: openH, corner: openCorner, shown: shown, frost: frost)', book)
        self.assertIn('.scaleEffect(grow, anchor: .topLeading)', book)
        self.assertIn('.scaleEffect(x: -grow, y: grow)', book)
        self.assertNotIn('tray(width: caseW', book)
        # "透明的边框 和红色封面 不一致 开始的时候": closed, the tray is pressed in and leans with the cover over it
        self.assertIn('.modifier(PressedLean(stage: stage, press: press, lean: angle > -90 ? lean : .zero))', book)
        self.assertIn('struct PressedLean: ViewModifier, Animatable {', detail)
        # the whole inside picture frosted, across both halves, not two glass sheets ("整个后面 背景 改成 毛玻璃 效果，而不是
        # 两块 毛玻璃"): blurred as one picture before it is cut in two
        self.assertIn('Self.frosted(frost, left: true, width: openW, height: openH)', book)
        self.assertIn('Self.frosted(frost, left: false, width: width, height: height)', detail)
        self.assertIn('FrozenBlur.image($0, width: 2 * openW, height: openH, radius: 16, saturation: 1.35,', book)
        self.assertNotIn('ultraThinMaterial', detail)
        self.assertNotIn('sheetInset', detail)
        # "整个后面 黑色 背景 换成封面": the stage behind the opened case is the game's picture
        # "算了 mac 背景也是毛玻璃": the ground frosted on the Mac and the iPhone alike
        self.assertIn('FrozenBlur.image($0, width: stage.width, height: stage.height, radius: 22,', book)
        self.assertNotIn('groundBlur', detail)
        # "封面移动的时候里面图片 也在一直 调整": nothing in the opened case is blurred frame by frame; the blurred
        # pictures are made once (FrozenBlur, the picture drawn by ImageRenderer with the same modifiers)
        self.assertNotIn('.blur(', detail)
        art = (mac / 'CoverArt.swift').read_text()
        self.assertIn('enum FrozenBlur {', art)
        self.assertIn('.blur(radius: radius, opaque: true)', art)
        # "慢速模式 … 慢1 2 3 5 10 20 50倍": every animation of the stage goes through paced, slowed by the menu's choice
        flow = (mac / 'CoverFlow.swift').read_text()
        self.assertIn('private func paced(_ animation: Animation) -> Animation { animation.speed(1 / max(1, slow)) }', flow)
        self.assertEqual(re.findall(r'withAnimation\(\.', flow), [])   # no animation of its own pace
        self.assertNotIn('= Animation.spring(', flow)
        home = (mac / 'HomePage.swift').read_text()
        self.assertIn('static let slowMotions = [1, 2, 3, 5, 10, 20, 50]', home)
        self.assertIn('slow: Double(slowMotion)', home)
        self.assertEqual((mac / 'CoverFlow.swift').read_text().count('inside: inside(index)'), 2)   # case and reflection
        flow = (mac / 'CoverFlow.swift').read_text()
        self.assertIn('var opening = CaseOpening.turn', flow)   # the default when a Launcher does not choose
        self.assertEqual(flow.count('opening: opening'), 2)      # the opened case and its reflection
        home = (mac / 'HomePage.swift').read_text()
        self.assertIn('@AppStorage(CaseOpening.key) private var caseOpening = CaseOpening.book.rawValue', home)
        self.assertIn('opening: CaseOpening(rawValue: caseOpening) ?? .book', home)
        settings = (mac / 'UsageStats.swift').read_text()
        self.assertIn('Picker(T.caseOpening, selection: $caseOpening)', settings)
        # the iPhone / iPad open the case the same way (user 2026-11-19: "安装 iphone ，效果一样"), over its picture
        phone = (LAUNCHER / 'Sources/LauncherApp.swift').read_text()
        self.assertIn('arrows: false, opening: .book,', phone)
        self.assertIn('inside: { CoverImages.shared.cover(at: Games.picturePath(cases[$0]))?.picture }', phone)
        # "整个背景 都不是全屏， 做成全屏": the stage is the whole screen, square-cornered; the Mac keeps its rounded panel
        self.assertIn('corner: 0, tiltInput:', phone)
        # "红色按钮 改成绿色和mac 一样 然后小一点放下面": the start green, smaller, at the bottom, the Mac's words
        self.assertIn('.buttonStyle(.borderedProminent).tint(.green).controlSize(wide ? .regular : .small)', phone)
        self.assertNotIn('tint(caseRed)', phone)
        # "mac 的 鼠标 在封面上的 效果（关闭的时候） 放在iphone上 用 陀螺仪 模拟": the phone's turn in place of the pointer,
        # only while the middle case rests closed; stopped in the background
        self.assertIn('corner: 0, tiltInput: motion.tilt) { i in', phone)
        self.assertIn('if now == .active { library.refresh(); motion.start() } else { motion.stop() }', phone)
        motion = (LAUNCHER / 'Sources/Motion.swift').read_text()
        self.assertIn('final class MotionTilt: ObservableObject {', motion)
        self.assertIn('manager.startDeviceMotionUpdates(to: .main)', motion)
        self.assertIn('rest = Self.toward(from, now, Self.follow)', motion)   # held still, the case comes back straight
        # "iphone 上 效果 明显一点": 9° to the edge, half again past the Mac's lean, 4 s to come back straight
        for part in ('private static let full = 0.15', 'private static let most = 1.5',
                     'private static let follow = 0.012'):
            self.assertIn(part, motion)
        cover_flow = (mac / 'CoverFlow.swift').read_text()
        self.assertIn('var tiltInput: CGPoint? = nil', cover_flow)
        self.assertIn('let resting = !flipped && !dragging && !moving && abs(position - Double(index)) < 0.01', cover_flow)
        self.assertIn('.onChange(of: tiltInput) { _, turn in if let turn { follow(turn) } }', cover_flow)
        # "右边 文字 和按钮 都往中间一点 离上下 一定距离": the phone's own margins, the Mac's left as they are
        self.assertIn('.padding(.horizontal, wide ? 10 : 0).padding(.vertical, wide ? 14 : 0)', phone)
        self.assertLess(phone.index('Spacer(minLength: 0)'), phone.index('Label(T.play, systemImage: "play.fill")'))
        self.assertIn('static let play = s("Start game", "启动游戏")', (LAUNCHER / 'Sources/Games.swift').read_text())
        self.assertIn('.ignoresSafeArea()\n        .background(Color.black.ignoresSafeArea())', phone)
        self.assertIn('let size = screen.size.height / 1.47', phone)
        flow_now = (mac / 'CoverFlow.swift').read_text()
        self.assertIn('var corner: CGFloat = 14', flow_now)
        self.assertIn('.clipShape(RoundedRectangle(cornerRadius: corner, style: .continuous))', flow_now)
        # "去掉 关闭按钮 点击背景 关闭": no close button; a tap on the ground round the case closes it, either opening
        self.assertNotIn('xmark', detail)
        self.assertEqual(detail.count('.onTapGesture(perform: close)'), 2)
        self.assertIn('.onKeyPress(.escape)', flow_now)   # Escape still closes it on the Mac


if __name__ == '__main__':
    unittest.main()
