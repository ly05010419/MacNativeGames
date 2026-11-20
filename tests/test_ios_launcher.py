"""The iPhone / iPad MNG Launcher (tools/ios_launcher) and the games' iOS apps agree on the URL schemes.

The Launcher sees a game as installed only when canOpenURL answers for its scheme, which iOS allows only for schemes in
LSApplicationQueriesSchemes, and the game's app must declare that scheme (CFBundleURLTypes, from build_ipa.py url_scheme).
A scheme missing on either side shows an installed game as not installed, with nothing in any log.
"""
import importlib.util
import json
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


def catalog(name):
    """GameCatalog.<name> in the shared GameStrings.swift."""
    text = (ROOT / 'tools/builder_app/Sources/MNGBuilder/GameStrings.swift').read_text()
    return re.findall(r'"([a-z0-9]+)"', re.search(rf'static let {name} = \[([^\]]*)\]', text)[1])


def launcher_games():
    """The games the iPhone Launcher looks for (their apps' URL schemes)."""
    return catalog('onIPhone')


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

    def test_the_mac_launcher_is_dark_with_nothing_under_the_stage(self):
        # user 2026-11-19: "mac 版本 去掉 下面这个说明 然后默认 改成 黑色背景 不用 白色背景"
        mac = ROOT / 'tools/builder_app/Sources/MNGBuilder'
        self.assertIn('NSApp.appearance = NSAppearance(named: .darkAqua)', (mac / 'App.swift').read_text())
        home = (mac / 'HomePage.swift').read_text()
        self.assertNotIn('Text(T.gameName(selected))', home)
        self.assertNotIn('Text(T.gamePlatforms(selected))', home)
        # "盒子里 黑色字体": black type on the opened case's light frosted glass, on both
        detail_text = (mac / 'CaseDetail.swift').read_text()
        self.assertIn('.environment(\\.colorScheme, frost == nil ? .dark : .light)', detail_text)
        self.assertIn('.overlay(Color.white.opacity(0.45))', detail_text)
        self.assertNotIn('.environment(\\.colorScheme, .dark)\n    }', (LAUNCHER / 'Sources/LauncherApp.swift').read_text())
        # "这个按钮 去掉 不是盒子里的": no add button under the stage, the opened case's own kept
        self.assertNotIn('Button { open(selected) } label: { Label(T.addGame, systemImage: "plus") }', home)
        self.assertIn('Button(action: add) { Label(T.addGame, systemImage: "plus")', home)
        # "没安装的 去掉 左边盒子里的 sd卡": a case opens with its cartridge only for a game built (the Mac) or installed
        # (the iPhone)
        detail = (mac / 'CaseDetail.swift').read_text()
        self.assertIn('if cartridge {\n                            Self.cartridge(inside,', detail)
        self.assertIn('cartridge: { i in model.installed.contains { $0.game == cases[i] } },', home)
        self.assertIn('cartridge: { library.installed.contains(cases[$0]) },', (LAUNCHER / 'Sources/LauncherApp.swift').read_text())
        self.assertEqual((mac / 'CoverFlow.swift').read_text().count('cartridge: cartridge(index)'), 2)
        # "龟速模式 按钮 只能我开发版 看到 用户 不能看到": the slow-motion menu in a build made here only
        menu = home.index('Picker(T.slowMotion, selection: $slowMotion)')
        self.assertIn('if !Paths.isRelease {', home[menu - 2000:menu])
        # "加一个“景深强度”选项（0.05 现在、0.10、0.15）": beside it, the development build only; the release keeps 0.05
        reach = home.index('Picker(T.depthReach, selection: $depthReach)')
        self.assertIn('if !Paths.isRelease {', home[reach - 900:reach])
        self.assertIn('static let depthReaches = [0.05, 0.10, 0.15]', home)
        self.assertIn('.environment(\\.depthReach, Paths.isRelease ? DepthReachKey.defaultValue : CGFloat(depthReach))', home)
        art_now = (mac / 'CoverArt.swift').read_text()
        self.assertIn('struct DepthReachKey: EnvironmentKey { static let defaultValue: CGFloat = 0.05 }', art_now)
        self.assertIn('let pixels = reach * w * scene.height * perPoint', art_now)
        # "mac 增加一个 开关 我可以对比 看效果": two layers or one, beside it, the development build only, off at first;
        # the release and the iPhone draw one, as 1.1.0 ("打开 更差"; "只能我测试显示 发布版本 不显示")
        switch = home.index('Toggle(T.depthLayers, isOn: $depthLayers)')
        self.assertIn('if !Paths.isRelease {', home[switch - 200:switch])
        self.assertIn('.environment(\\.depthLayers, !Paths.isRelease && depthLayers)', home)
        self.assertIn('@AppStorage("depthLayers") private var depthLayers = false', home)
        self.assertIn('struct DepthLayersKey: EnvironmentKey { static let defaultValue = false }', art_now)
        self.assertIn('focus: Self.depthFocus, layered: layered)', (mac / 'CoverArt.swift').read_text())
        self.assertIn('shown.layered == key.4', (mac / 'DepthSurface.swift').read_text())
        scene = (mac / 'DepthScene.swift').read_text()
        self.assertIn('if layered, let behind = scene.behind {', scene)
        self.assertIn('(scene.plain ?? scene.depth).clampedToExtent()', scene)   # one layer: the depth as it was

    def test_every_case_s_depth_is_made_ahead(self):
        # user 2026-11-19: "每次 看一个封面 才开始准备 视差效果 还要等1秒 不能都提前准备好吗？": both home pages ask for
        # every case's scene when they show, the middle case first, made one at a time and all kept
        scene = (ROOT / 'tools/builder_app/Sources/MNGBuilder/DepthScene.swift').read_text()
        self.assertIn('static let kept = 16', scene)
        self.assertIn('func prepare(_ paths: [String])', scene)
        self.assertIn('guard !working, !queue.isEmpty else { return }', scene)
        # the model compiled once and kept ("depth model ready in 7487 ms" on every start before)
        self.assertIn('let kept = folder.appendingPathComponent("\\(modelName)-\\(stamp).mlmodelc")', scene)
        self.assertIn('MLModel(contentsOf: kept, configuration: MLModelConfiguration())', scene)
        call = 'DepthScenes.shared.prepare(DepthScenes.outwards('
        self.assertIn(call, (ROOT / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text())
        self.assertIn(call, (LAUNCHER / 'Sources/LauncherApp.swift').read_text())

    def test_the_launcher_icons_come_from_the_project_icon(self):
        # user 2026-11-19: "这个 icon 带白边 去掉", then "ok": icon.png without its light ground, the Mac's icon on the
        # macOS grid (transparent round it), the iPhone's filling the square and opaque (iOS cuts its own corners)
        import struct
        def png(path):
            head = path.read_bytes()[:26]
            width, height, depth, kind = struct.unpack('>IIBB', head[16:26])
            return width, height, kind
        resources = ROOT / 'tools/builder_app/Resources'
        self.assertEqual(png(resources / 'AppIcon.png'), (1024, 1024, 6))      # RGBA
        self.assertEqual(png(resources / 'AppIcon-iOS.png'), (1024, 1024, 2))  # RGB: no transparency on iOS
        self.assertEqual(png(ROOT / 'icon.png')[2], 6)                          # the light ground made transparent
        self.assertIn("ICON = REPO / 'tools/builder_app/Resources/AppIcon-iOS.png'", (LAUNCHER / 'install.py').read_text())

    def test_both_launchers_show_the_same_games_in_the_same_order(self):
        # user 2026-11-19: "为什么 mac上的 游戏数量 和 iphone不一样？？？" → "统一 b 两边数量一样", "顺序也一样": one
        # list, shared, read by both; no reordering on either side
        every = catalog('all')
        self.assertEqual(every, ['oot', 'totk', 'botw', 'eow', 'odyssey', 'acnh', 'dkb', 'pkp', 'dread', 'tmnt'])
        self.assertTrue(set(catalog('onIPhone')) <= set(every))
        home = (ROOT / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('static let games = GameCatalog.all', home)
        self.assertIn('private var order: [String] { Self.games }', home)
        games = (LAUNCHER / 'Sources/Games.swift').read_text()
        self.assertIn('static let all = GameCatalog.all', games)
        self.assertIn('static let playable = GameCatalog.onIPhone', games)
        app = (LAUNCHER / 'Sources/LauncherApp.swift').read_text()
        self.assertIn('private var order: [String] { Games.all }', app)
        self.assertIn('Games.playable.filter { UIApplication.shared.canOpenURL', app)
        ipa = load(LAUNCHER / 'install.py', 'ios_launcher_install_cases')
        self.assertEqual(list(ipa.CASES), every)
        pins = json.loads((ROOT / 'tools/builder_app/Resources/covers.json').read_text())['covers']
        self.assertEqual(sorted(pins), sorted(every))   # every case its picture

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
        self.assertIn('CoverFront(game: game, picture: cover?.picture, picturePath: cover?.path, center: Games.center(game))', ios)
        # the Mac draws its cases the same way (user 2026-11-19: "mac 也要这样优化吗" "改")
        self.assertIn('CoverFront(game: game, picture: art?.picture, picturePath: art?.path, center: center)', home)
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
        self.assertIn('FrozenBlur.image($0, width: 2 * openW, height: openH, radius: 44, saturation: 1.2,', book)
        self.assertNotIn('ultraThinMaterial', detail)
        self.assertNotIn('sheetInset', detail)
        # "整个后面 黑色 背景 换成封面": the stage behind the opened case is the game's picture
        # "mac 试一试 3d 视差效果": the leaning case's front in layers moving apart (the backdrop away, the picture and
        # its name towards the pointer), each only moved, the middle case only
        art = (mac / 'CoverArt.swift').read_text()
        self.assertIn('@Environment(\\.caseParallax) private var parallax', art)
        self.assertIn('.offset(middle)', art)
        # "去掉 这个 只显示 图片，不用 正方形了 用 长方形": the insert is the picture alone, no name nor blurred ground
        for part in ('.offset(back)', '.offset(front)', 'T.gameName(game)', '.blur(radius: 10)'):
            self.assertNotIn(part, art)
        self.assertIn('.environment(\\.caseParallax, i == g.center ? CGPoint(x: tiltX, y: tiltY) : .zero)',
                      (mac / 'CoverFlow.swift').read_text())
        self.assertIn('.environment(\\.caseParallax, lean)', detail)
        # "能想iphone 原生的 一样好吗？" "好 做" (a spatial scene), then "开始做2": the picture's depth by Depth Anything
        # V2 Small (Apache-2.0, carried as it is, compiled by Core ML when the Launcher starts), and the picture drawn
        # anew pixel by pixel from it as the case leans (parallax occlusion mapping in a Core Image kernel written as
        # source: no Metal of our own, Command Line Tools builds), one image a frame, the last kept for the reflection
        scene = (mac / 'DepthScene.swift').read_text()
        self.assertIn('@MainActor final class DepthScenes: ObservableObject {', scene)
        for part in ('VNCoreMLRequest(model: model)', 'MLModel.compileModel(at: url)', 'static let side = 512',
                     'static let kept = 16', 'let ready = Self.model != nil', '@MainActor enum DepthWarp {',
                     'static let kernel: CIKernel? = CIKernel(source: source)', 'float hit = step(z, d) * (1.0 - found);', 'static let steps = 48',
                     'static func warped(_ scene: DepthScenes.Scene, shift: CGPoint, focus: CGFloat, layered: Bool = true) -> CIImage?'):
            self.assertIn(part, scene)
        self.assertNotIn('ShaderLibrary', scene)
        # "ab 都做": the warp drawn by the GPU straight into a Metal layer, never copied back into a CGImage (B), and
        # nothing drawn for a shift under half a pixel nor for the sensor's jitter (A)
        self.assertNotIn('createCGImage', scene)
        surface = (mac / 'DepthSurface.swift').read_text()
        for part in ('CIContext(mtlCommandQueue:', 'CIRenderDestination(mtlTexture: drawable.texture, commandBuffer: buffer)',
                     'shown.shift == key.1', 'buffer.present(drawable)',
                     # no more pixels than the scene's own (at the iPhone's 3x a frame in five was dropped)
                     'let fit = min(1, extent.height / max(1, bounds.height * scale))'):
            self.assertIn(part, surface)
        self.assertNotIn('makeLibrary', surface)   # no shader of our own: Command Line Tools builds
        self.assertIn("'DepthSurface.swift'", (LAUNCHER / 'install.py').read_text())
        # "直接做2 可以照 3D Photo Inpainting 的思路，自己写一个轻量的两层版本（前景加补好的背景） 做mac": the depth's
        # edges made sharp, the picture behind near things filled from their farther side, and where the walk meets a
        # side, what is behind shows; on the Mac, then the iPhone too ("安装 iPhone")
        layers = (mac / 'DepthLayers.swift').read_text()
        for part in ('enum DepthLayers {\n    #if os(macOS)\n    static let on = !Paths.isRelease\n    #else\n    static let on = false\n',
                     'static func sharpened(', 'static func nearestFarther(', 'vImageMin_PlanarF', 'vImageMax_PlanarF',
                     'jump = mix(jump, e - prev, hit);', 'abs(behindAt - before)', 'depth.samplingNearest()', 'static let reach = 64',
                     'static let kernel: CIKernel? = CIKernel(source: source)'):
            self.assertIn(part, layers)
        self.assertNotIn('import Metal', layers)   # Core Image source, as the one layer
        for part in ('guard DepthLayers.on else {', 'DepthLayers.make(depth: depth, rgba: rgba, width: w, height: h)',
                     'format: .RGh', 'if layered, let behind = scene.behind {'):
            self.assertIn(part, scene)
        self.assertIn("'DepthLayers.swift'", (LAUNCHER / 'install.py').read_text())
        self.assertIn('if parallax != .zero, DepthWarp.ready, let scene = depth.scene(of: picturePath) {', art)
        self.assertIn('let half = { (v: CGFloat) in (v * 2).rounded() / 2 }', art)
        self.assertIn('if next != tilt, next == .zero || abs(next.x - tilt.x) >= 0.002 || abs(next.y - tilt.y) >= 0.002 {',
                      (LAUNCHER / 'Sources/Motion.swift').read_text())
        self.assertIn('static var depthReach: CGFloat { 0.05 }', art)   # "厚度 小一半"
        # "裁剪成 正方形 然后 中间80% 显示在封面上", then "改成 90%": the square key art drawn 1 / 0.9 x the insert high, centred, so the
        # insert shows its middle 90%; never the picture enlarged by a scale ("现在 看不全了")
        self.assertIn('static var margin: CGFloat { 1 / 0.9 }', art)
        self.assertNotIn('overscan', art)
        self.assertIn('CroppedPicture(picture: picture, center: center, margin: margin).offset(middle)', art)
        self.assertIn('self.init(aspect: picture.size.width / max(1, picture.size.height), center: center, margin: margin)', art)
        # every picture decoded to the same height ("像素都一致 不要有些高清 有些模糊")
        self.assertIn('nonisolated static let height = 1024', art)
        self.assertIn('kCGImageSourceThumbnailMaxPixelSize: longest', art)
        self.assertIn('struct Scene { let picture: CIImage; let depth: CIImage; let height: CGFloat; var behind: CIImage? = nil; var plain: CIImage? = nil }',
                      scene)
        # every case's picture the game's official key art (Resources/covers.json: addresses and SHA-256 only), in
        # a local build's app, fetched by the released one on the player's Mac, else the app's icon
        import json as _json
        pins = _json.loads((mac.parents[1] / 'Resources/covers.json').read_text())['covers']
        self.assertEqual(sorted(pins), sorted(['oot', 'totk', 'botw', 'eow', 'odyssey', 'acnh', 'dkb', 'pkp', 'dread',
                                               'tmnt']))
        for game, pin in pins.items():
            # the originals as Nintendo uploaded them, never recompressed ("不要压缩", "像素都一致")
            self.assertTrue(pin['url'].startswith(('https://assets.nintendo.com/image/upload/store/software/', 'https://atum-img-lp1.cdn.nintendo.net/i/c/')), game)
            self.assertNotIn('q_90', pin['url'])
            self.assertRegex(pin['sha256'], r'^[0-9a-f]{64}$')
        self.assertFalse(list((mac.parents[1] / 'Resources').glob('*.jpg')))   # no picture in the repository
        official = (mac / 'OfficialCovers.swift').read_text()
        for part in ('SHA256.hash(data: data)', 'appendingPathComponent("Covers/\\(name).jpg")',
                     'FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)'):
            self.assertIn(part, official)
        self.assertIn('OfficialCovers.shared.path(for: game) ?? picturePath(of: app)', (mac / 'HomePage.swift').read_text())
        # the box photos as the whole front were tried and dropped ("大金刚和 pokopia 用以前的" "动森和旷野 也用 以前的"
        # "时之笛 用 2 号图吧" "好 3"): every front is our band over the picture, and the shared CoverArt.swift must
        # use nothing of the Mac's alone (OfficialCovers), or the iPhone app does not build
        self.assertFalse([g for g, p in pins.items() if 'box' in p])
        for gone in ('trimmedFile', 'boxMargin', 'var whole'):
            self.assertNotIn(gone, art)
        self.assertNotIn('OfficialCovers', art)
        # "用 1 把 裁剪 右边 不要中间" then "好 3": Tears of the Kingdom's front cut from the right of its wide key art
        self.assertEqual(pins['totk']['center'], 0.61)   # "让人物 在右边但是能看全"
        self.assertEqual(pins['botw']['center'], 0.365)   # "旷野 把人 漏出来", "用2图 人物 居中吧": the wide key art, Link in the middle
        self.assertEqual(pins['totk']['sha256'], 'f20258999e8d852a496dad2961a87faf2b119488f97fb81442f378043e58b000')
        self.assertIn('center: Games.center(game)', (LAUNCHER / 'Sources/LauncherApp.swift').read_text())
        self.assertIn("shutil.copy2(source, folder / 'covers.json')", (LAUNCHER / 'install.py').read_text())
        self.assertIn('picturePath: art?.path, center: center)', (mac / 'HomePage.swift').read_text())
        make_app = (mac.parents[1] / 'make_app.py').read_text()
        self.assertIn('covers = official_covers(stage / \'Contents/Resources\', fetch_pictures=not a.portable)', make_app)
        self.assertIn('official = official_covers(project / \'Covers\')', (LAUNCHER / 'install.py').read_text())
        self.assertIn('ofType: "jpg", inDirectory: "Covers")', (LAUNCHER / 'Sources/Games.swift').read_text())
        self.assertNotIn('SpatialLayers', art)
        self.assertIn("'DepthScene.swift'", (LAUNCHER / 'install.py').read_text())
        # the model pinned by SHA-256 in tools/deps, put into both apps with its licence, never into the repository
        pins = (LAUNCHER.parents[0] / 'deps/pins.py').read_text()
        self.assertIn("DEPTH_MODEL_PACKAGE = 'DepthAnythingV2SmallF16P8.mlpackage'", pins)
        self.assertIn("'cfef6f6f2a70783dedc0bfae40cecbc2052285d3'", pins)
        self.assertIn("say('depth model: ' + depth_model(stage / 'Contents/Resources'))",
                      (LAUNCHER.parents[0] / 'builder_app/make_app.py').read_text())
        self.assertIn("project / 'Models' / DEPTH_MODEL_PACKAGE", (LAUNCHER / 'install.py').read_text())
        self.assertIn('- path: Models', (LAUNCHER / 'project.yml').read_text())
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
        self.assertIn('slow: Paths.isRelease ? 1 : Double(slowMotion)', home)   # test builds only (test_launcher_test_controls)
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
        self.assertIn('corner: 0, turns:', phone)
        # "红色按钮 改成绿色和mac 一样 然后小一点放下面": the start green, smaller, at the bottom, the Mac's words
        self.assertIn('.buttonStyle(.borderedProminent).tint(.green).controlSize(wide ? .regular : .small)', phone)
        self.assertNotIn('tint(caseRed)', phone)
        # "mac 的 鼠标 在封面上的 效果（关闭的时候） 放在iphone上 用 陀螺仪 模拟": the phone's turn in place of the pointer,
        # only while the middle case rests closed; stopped in the background
        # sent by a call (TurnFeed), not watched: a reading redraws the middle case only, never the home page or the
        # stage ("越用越慢", "陀螺仪 效果 慢半拍": 65 readings a second redrew all ten cases)
        self.assertIn('corner: 0, turns: motion.feed) { i in', phone)
        self.assertIn('@State private var motion = MotionTilt()', phone)
        self.assertIn('if now == .active { library.refresh(); motion.start() } else { motion.stop() }', phone)
        motion = (LAUNCHER / 'Sources/Motion.swift').read_text()
        self.assertIn('final class MotionTilt {', motion)
        self.assertNotIn('@Published', motion)
        self.assertIn('feed.send(next)', motion)
        self.assertIn('if abs(next.x) < 0.01, abs(next.y) < 0.01 { next = .zero }', motion)   # caught up: it stops
        self.assertIn('manager.startDeviceMotionUpdates(to: .main)', motion)
        self.assertIn('rest = Self.toward(from, now, Self.follow)', motion)   # held still, the case comes back straight
        # "iphone 上 效果 明显一点": 9° to the edge, half again past the Mac's lean, 4 s to come back straight
        for part in ('private static let full = 0.15', 'private static let most = 1.5',
                     'private static let follow = 0.012'):
            self.assertIn(part, motion)
        cover_flow = (mac / 'CoverFlow.swift').read_text()
        self.assertIn('var turns: TurnFeed? = nil', cover_flow)
        self.assertIn('let resting = !flipped && !dragging && !moving && abs(position - Double(index)) < 0.01', cover_flow)
        self.assertIn('turns?.onTurn = { turn in follow(turn) }', cover_flow)
        # the lean and light out of the stage: only MiddleLean watches them, the stage animates no tilt
        self.assertIn('@State private var leaning = CaseLean()', cover_flow)
        self.assertIn('MiddleLean(leaning: leaning) { tilt, lit in', cover_flow)
        self.assertNotIn('var tiltX: Double', cover_flow)
        self.assertIn('var animatableData: AnimatablePair<AnimatablePair<Double, Double>, AnimatablePair<CaseAmounts, Double>> {',
                      cover_flow)
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
