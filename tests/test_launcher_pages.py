"""MNG Launcher's game pages per game (user 2026-11-13: Ocarina of Time gets Controls, Storage and Saves): the backend's
table (tools/builder/games.py PAGES) and the window's (Model.swift gamePages) are the same, a page a game does not have is
refused, OOT's pages read its own folder but its launcher's game slot (TOTK/game-slot), and its keyboard page names the
buttons only (their uses in OOT are not confirmed)."""
import fcntl
import json
from pathlib import Path
import plistlib
import re
import shutil
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.builder import game_state  # noqa: E402
from tools.builder.games import GAMES, PAGE_GAMES, PAGES  # noqa: E402
from tools.builder_jobs import tabs  # noqa: E402


def oot_app(folder):
    app = Path(folder) / 'Ocarina of Time.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.macnativegames.oot'}))
    return app


class Pages(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.home = self.tmp / 'home'
        self.app = oot_app(self.tmp)

    def test_the_window_shows_the_pages_the_backend_serves(self):
        swift = (REPO / 'tools/builder_app/Sources/MNGBuilder/Model.swift').read_text()
        table = re.search(r'static let gamePages: \[String: \[String\]\] = \[(.*?)\n    \]', swift, re.S).group(1)
        window = {g: tuple(re.findall(r'"(\w+)"', pages)) for g, pages in re.findall(r'"(\w+)": \[([^\]]*)\]', table)}
        self.assertEqual(window, PAGES)
        self.assertEqual(set(PAGES), set(PAGE_GAMES))
        self.assertTrue(all(set(p) <= set(tabs.TABS) for p in PAGES.values()))

    def test_ocarina_is_a_page_game_but_not_in_the_build_catalog(self):
        self.assertIn('oot', PAGE_GAMES)
        self.assertNotIn('oot', GAMES)   # built by games/oot/build.py; GAMES is compared with build_app.py
        self.assertEqual(PAGES['oot'], ('controls', 'storage', 'saves'))   # 诊断模式 removed 2026-11-15: a quick App carries it

    def test_a_page_the_game_does_not_have_is_refused(self):
        for tab in ('settings', 'mods'):
            answer = tabs.call(tab, 'state', self.app, home=self.home)
            self.assertFalse(answer['ok'])
            self.assertIn(tab, answer['error'])
        dkb = self.tmp / 'x' / 'Donkey Kong Bananza.app'
        (dkb / 'Contents').mkdir(parents=True)
        (dkb / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.macnativegames.dkb'}))
        self.assertFalse(tabs.call('saves', 'state', dkb, home=self.home)['ok'])

    def test_ocarina_s_pages_read_its_own_folder(self):
        ctx = tabs.context(self.app, self.home)
        self.assertEqual(ctx.data_dir, self.home / 'Library/Application Support/MacNativeGames/OcarinaOfTime')
        saves = tabs.call('saves', 'state', self.app, home=self.home)
        self.assertTrue(saves['ok'], saves)
        self.assertEqual(saves['result']['sources'][0]['path'], str(ctx.data_dir / 'profile'))
        self.assertEqual(saves['result']['sources'][0]['title_id'], '0400486024DEC000')
        controls = tabs.call('controls', 'state', self.app, home=self.home)
        self.assertTrue(controls['ok'], controls)
        self.assertEqual(controls['result']['game'], 'oot')
        self.assertTrue(tabs.call('storage', 'state', self.app, home=self.home)['ok'])

    def test_ocarina_s_game_slot_is_the_one_its_launcher_takes(self):
        support = self.home / 'Library/Application Support/MacNativeGames'
        self.assertEqual(game_state.slot_dir(support / 'OcarinaOfTime'), support / 'TOTK/game-slot')
        self.assertEqual(game_state.slot_dir(support / 'TOTK'), support / 'TOTK/game-slot')
        self.assertEqual(game_state.slot_dir(support / 'BotW'), support / 'BotW/game-slot')
        slot = support / 'TOTK/game-slot'; slot.mkdir(parents=True)
        (slot / 'lock').write_text('')
        fd = open(slot / 'lock'); fcntl.flock(fd, fcntl.LOCK_EX)   # a game holds TOTK/game-slot
        try:
            saves = tabs.call('saves', 'state', self.app, home=self.home)
            self.assertTrue(saves['result']['running'], 'OOT runs while its slot (TOTK/game-slot) is held')
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN); fd.close()

    def test_ocarina_s_keyboard_page_names_the_buttons_only(self):
        answer = tabs.call('controls', 'keyboard', self.app, home=self.home)
        self.assertTrue(answer['ok'], answer)
        titles = {a['id']: a['title'] for a in answer['result']['actions']}
        self.assertEqual([titles[b] for b in 'abxy'], ['A', 'B', 'X', 'Y'])

    def test_every_game_s_cover_can_be_reached(self):
        # the games as the iPod classic's Cover Flow (user 2026-11-17): a mouse wheel scrolls the page, so every cover is
        # reached by the arrows, the arrow keys and a drag, whatever the window's width
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('CoverFlow(count: cases.count, index: center, launches: launches, name: { T.gameName(cases[$0]) })', home)
        # the built games first, then the ones not built yet (user 2026-11-17); the chosen one is kept by game, not place
        self.assertIn('return built + Self.games.filter { !built.contains($0) }', home)
        self.assertIn('let cases = order', home)
        self.assertIn('set: { chosen = order[$0] }', home)
        flow =(REPO / 'tools/builder_app/Sources/MNGBuilder/CoverFlow.swift').read_text()
        for way in ('chevron.left', 'chevron.right', '.onKeyPress(.leftArrow)', '.onKeyPress(.rightArrow)', 'DragGesture('):
            self.assertIn(way, flow)

    def test_the_middle_case_turns_over_to_its_actions(self):
        # scheme A of the sketch (user 2026-11-17: "先做 a"): a click on the middle case or Return turns it over, its back
        # holds the game's actions (GameActions) or, for a game not built yet, the way to build it; Esc turns it back;
        # its pages open under the Cover Flow; the drawn back takes no clicks, so a live one is laid on top once settled
        flow = (REPO / 'tools/builder_app/Sources/MNGBuilder/CoverFlow.swift').read_text()
        # a click presses the middle case in (user 2026-11-18: "点击的时候 再来一下 感觉点击上了的效果"); a drag lets
        # the press go
        for part in ('withAnimation(pressDown) { press = 1 }', 'dampingFraction: 0.42)) { press = 0 }',
                     'lean(at: nil, nil); release()',
                     'let pressed = i == center ? CGFloat(press) * w * 0.1 : 0'):
            self.assertIn(part, flow)
        # it turns on the button coming up, however long it was held ("长按 也翻页"), not when dragged away
        self.assertNotIn('clickLongest', flow)
        for part in ('guard pressedMiddle, !flipped, moved < 6,',
                     'if i != index { go(to: i) }'):
            self.assertIn(part, flow)
        self.assertNotIn('openSoon', flow)
        # and turns from the case as pressed and leaning, springing out as it goes, not from the case at rest
        # ("从默认状态开始的 所以不连贯")
        for part in ('coverLean = tilt; coverLit = CGFloat(lit); detailPress = CGFloat(press)',
                     'withAnimation(.spring(response: 0.3, dampingFraction: 0.5)) { detailPress = 0 }',
                     'press: detailPress', 'func pressedScale(_ press: CGFloat) -> CGFloat'):
            self.assertIn(part, flow)
        detail = (REPO / 'tools/builder_app/Sources/MNGBuilder/CaseDetail.swift').read_text()
        for part in ('var press: CGFloat = 0', 'stage.pressedScale(press)', 'let deeper = 1 + 0.8 * Double(press)',
                     # the press springs on its own: in CaseDetail's animatableData its spring took the turn over and the
                     # turn ran fast ("翻页 速度快了很多")
                     'struct PressedFront<Cover: View>: View, Animatable',
                     'var animatableData: AnimatablePair<CGFloat, AnimatablePair<CGFloat, AnimatablePair<CGFloat, CGFloat>>>'):
            self.assertIn(part, detail)
        self.assertNotIn('AnimatablePair(open, press)', detail)
        # the tap ending the click that opened it does not close it again ("翻页超快 然后黑屏"), and an open begun after
        # a close does not run
        for part in ('if !pressedMiddle { turn(false) }', 'guard flipped else { return }   // closed again'):
            self.assertIn(part, flow)
        self.assertNotIn('if flipped { turn(false); return }', flow)
        for part in ('.onKeyPress(.escape)', 'if flipped { return .ignored }',
                     'withAnimation(turnSpring, completionCriteria: .removed)',
                     'let a = -side * tilt * (1 - 0.15 * Double(pull)) + Double.pi * over * Self.turnWay',
                     'static let turnWay: Double = -1'):   # "这个方向好": the opening edge shows half way
            self.assertIn(part, flow)
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('GameActions(app: app, section: $gamePage, live: live) { launches += 1 }', home)
        # it opens over the stage ("翻转后 放大 全屏 然后 左边是封面 右边现在的 按钮"), turning and growing at once ("翻转的
        # 同时 放大 不是翻转完"): from its front at rest to the stage, half a circle the case's way, its front until edge-on,
        # then the opened case (cover left, live actions right, laid out wide); closing plays it backwards
        detail = (REPO / 'tools/builder_app/Sources/MNGBuilder/CaseDetail.swift').read_text()
        for part in ('let fromW = stage.w * stage.frontScale', 'stage.backCenter.x + (stage.width / 2 - stage.backCenter.x) * p',
                     '.rotation3DEffect(.degrees(settled ? 0 : 180 * Double(p) * CoverFlowGeometry.turnWay)', 'if p < 0.5 {',
                     '.scaleEffect(x: settled ? 1 : -1, y: 1)', 'static var coverShare: CGFloat { 0.44 }'):
            self.assertIn(part, detail)
        for part in ('CaseDetail(stage: stage, open: open, lean: coverLean, lit: coverLit, press: detailPress, close: { turn(false) })',
                     'withAnimation(openingTurn) { open = 1; coverLean = .zero; coverLit = 0 }',
                     'private let openingTurn = Animation.spring(response: 0.46, dampingFraction: 0.69)',
                     'withAnimation(turnSpring, completionCriteria: .removed) { open = 0; coverLean = .zero; coverLit = 0 } completion: {',
                     'flip: Double(open), hideCenter: expanded',
                     "including: flipped ? .subviews : .all", 'guard !expanded else { return }'):
            self.assertIn(part, flow)
        self.assertNotIn('openSpring', flow)
        # its reflection grows and shrinks with it ("返回的时候 底下阴影 没有跟上"), upside down about the stage's floor
        # line, never live; one ground behind cover and actions ("不要 这样 隔开"); big buttons 200 points, centred
        self.assertIn('.scaleEffect(x: 1, y: -1, anchor: UnitPoint(x: 0.5, y: (stage.floorY + stage.gap / 2) / height))', flow)
        self.assertIn('detail(index, false)', flow)
        self.assertIn('LinearGradient(colors: [Color(white: 0.17), Color(white: 0.03)], startPoint: .top, endPoint: .bottom)', detail)
        self.assertNotIn('Color(red: 0.12, green: 0.12, blue: 0.13)', detail)
        self.assertIn('private var buttonWidth: CGFloat { wide ? 200 : .infinity }', home)
        self.assertNotIn('.frame(maxWidth: wide ? .infinity : nil)', home)   # "极速更新 不要居中了"
        # "封面 要依然支持 鼠标效果": the opened cover leans and lights, laid flat once settled; "去掉 滚动条"
        for part in ('.rotation3DEffect(.degrees(Double(lean.x) * 9)', 'private var settled: Bool { open >= 0.999 }',
                     'CaseFront(light: Double(lit), at: UnitPoint(x: (1 + lean.x) / 2'):
            self.assertIn(part, detail)
        self.assertIn('case .active(let p): leanCover(at: p, stage)', flow)
        self.assertIn('ScrollView(.vertical, showsIndicators: false) {   // no scroll bar beside the stage', home)
        # "文字 高度对齐 启动游戏按钮 放在 黑色背景 右下角": the column level with the cover, the start bottom right
        self.assertIn('let margin = max(12, (height - stage.h * scale) / 2)', detail)
        self.assertIn('VStack(alignment: .trailing, spacing: 8) {', home)
        self.assertNotIn('backLive', flow)
        self.assertIn('GameActions(app: app, section: $gamePage, live: live, wide: true) { launches += 1 }', home)
        # how it feels, as the sketch's A page (user 2026-11-17: "有鼠标 光影 和 盒子动的特效"): lean and light under the
        # pointer, rubber band and throw on a drag, two-finger swipes
        for part in ('.onContinuousHover(coordinateSpace: .local)', 'i == center ? CATransform3DConcat(lean, r) : r',
                     'RadialGradient(colors: [.white.opacity(0.42 * light)', 'func pull(_ x: Double)',
                     'v.predictedEndTranslation.width', 'NSEvent.addLocalMonitorForEvents(matching: .scrollWheel)'):
            self.assertIn(part, flow)
        # a start turns the opened case back to its front, the page of the games, the way Esc closes it; its cover no
        # longer grows over the whole stage first (user 2026-11-18: "点击 启动游戏 就翻页返回主页 不要放大封面")
        self.assertIn('.onChange(of: launches) { turn(false) }', flow)
        self.assertIn('.onKeyPress(.escape) { guard flipped else { return .ignored }; turn(false); return .handled }', flow)
        for gone in ('func grow()', 'zooming', 'zoomFade', 'zoomFromCover', 'stage.fill'):
            self.assertNotIn(gone, flow)
        self.assertIn('started()', home)
        # the folded cases answer the pointer too (user 2026-11-17: "其他收起来的 应该也有相应的互动效果"): the one under
        # it, found on the stage at rest, comes out a little, those beyond move aside, its name floats over it; it stays
        # behind the nearer cases ("效果 太夸张 都出来了": it had come all the way out over them)
        for part in ('let i = stage.cover(at: p)', 'pulls = CaseAmounts.one(side, of: count)',
                     'pull * w * 0.1', 'CGFloat(pushed(i)) * w * 0.2', 'Text(name(pulled))'):
            self.assertIn(part, flow)
        self.assertIn('var drawOrder: [Int] { (0..<count).sorted { abs(place($0).d) > abs(place($1).d) } }', flow)
        # "总是有 卡顿 和 重新调整 的 硬跳": the corner layers are one Canvas per box (not 12 projected views), and the
        # pointer is judged on the stage as shown, with the current pull
        self.assertIn('let layers = (0..<CoverFlowGeometry.slices).map { g.outline(', flow)
        self.assertNotIn('ForEach(0..<CoverFlowGeometry.slices', flow)
        self.assertIn('position: position,\n                                          pulls: pulls)', flow)
        # "动画结束的时候 会有一次 硬跳 调整" (the frame record: a clicked case kept its pull to the middle and dropped it
        # after): a new middle takes the pull back with the move's spring, and the pointer pulls nothing while it moves
        self.assertIn('.onChange(of: index) { turn(false); settleMove() }', flow)
        self.assertIn('withAnimation(spring, completionCriteria: .removed) { index = to; drag = 0 } completion: { moving = false }', flow)
        self.assertNotIn('withAnimation(spring) { index =', flow)   # every move goes through go(to:)
        self.assertIn('!dragging, !moving,', flow)
        self.assertIn('MNG_COVERFLOW_TRACE', (REPO / 'tools/builder_app/Sources/MNGBuilder/CoverFlowTrace.swift').read_text())
        self.assertIn('struct CaseAmounts: VectorArithmetic', (REPO / 'tools/builder_app/Sources/MNGBuilder/CaseAmounts.swift').read_text())
        self.assertIn('EmptyBack(ready: model.installedLoaded, live: live) { open(game) }', home)
        self.assertIn('GamePage(id: id, app: app.app)', home)
        self.assertNotIn('GameActions(app: app).id(app.app)', home)   # no actions box under the stage any more
        # one state for the three views of a back, read only by the live one
        self.assertIn('GameCardState.of(app.app)', home)
        # the switches are read before the case opens, so its buttons never move and no Python process runs during
        # the opening (user 2026-11-19: "mac 第一次打开 这些按钮在动 而且卡，第二次 就好了"); the opened case reads them
        # again only after its opening animation
        self.assertIn('await GameCardState.preload(middle + model.installed.map(\\.app))', home)
        self.assertIn('if !state.loaded { await state.loadSwitches(app.app); return }', home)
        self.assertIn('try? await Task.sleep(for: .milliseconds(800))', home)
        self.assertNotIn('state.asked', home)
        self.assertIn('.keyboardShortcut(live ? KeyboardShortcut.defaultAction : nil)', home)

    def test_another_game_s_build_page_starts_without_the_files_chosen_before(self):
        # user 2026-11-17: "点击空盒子 安装的时候 应该过去的信息 清空": a case or a full update opens the Build page through
        # Model.buildGame, which drops files of another game (the keys stay) unless a build is running
        swift = REPO / 'tools/builder_app/Sources/MNGBuilder'
        model = (swift / 'Model.swift').read_text()
        self.assertIn('if !jobRunning, plan?.game != game { clear() }', model)
        clear = model[model.index('func clear()'):]
        clear = clear[:clear.index('\n    }')]
        # a prod.keys picked among the game files is kept as the keys, not dropped with the files
        self.assertIn('if keysPath == nil, let found = plan?.keys { keysPath = found }', clear)
        self.assertNotIn('keysPath = nil', clear)
        # and step 2 shows those keys before any game file is chosen (no plan yet)
        self.assertIn('let keys = model.plan?.keys ?? model.keysPath', (swift / 'ContentView.swift').read_text())
        home = (swift / 'HomePage.swift').read_text()
        self.assertEqual(home.count('model.buildGame('), 2)
        self.assertNotIn('model.buildFor = ', home)

    def test_a_case_s_picture_is_cut_tall_round_its_title(self):
        # user 2026-11-17: "图片 重新裁剪 要合适的 长方形": the square picture is cut to a tall rectangle whose middle is
        # the middle of the title's words and what draws the eye (Vision), not shown whole with a blur under it
        # (the front moved to CoverArt.swift's CoverFront 2026-11-19, shared with the iPhone / iPad Launcher)
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('CoverFront(game: game, picture: art?.picture, picturePath: art?.path, backdrop: art?.backdrop)', home)
        art = (REPO / 'tools/builder_app/Sources/MNGBuilder/CoverArt.swift').read_text()
        self.assertIn('CroppedPicture(picture: picture, center: crops.center(of: picturePath))', art)
        for part in ('VNRecognizeTextRequest()', 'VNGenerateAttentionBasedSaliencyImageRequest()', 'Task.detached'):
            self.assertIn(part, art)
        self.assertIn('Text(T.gamePlatforms(game))', (REPO / 'tools/builder_app/Sources/MNGBuilder/CoverFlow.swift').read_text())


class HomeShowsABuild(unittest.TestCase):
    """A build started on the Build page (Model.build sets updating to nil) and left with 返回: the home page greys its
    buttons while phase is building, so it must also show that build's progress and Cancel, not grey buttons alone."""

    def test_the_home_page_shows_a_build_started_on_the_build_page(self):
        swift = REPO / 'tools/builder_app/Sources/MNGBuilder'
        self.assertIn('updating = nil', (swift / 'Model.swift').read_text())   # a build is not an update
        self.assertIn('private var mine: Bool { updateOnly == (model.updating != nil) }',
                      (swift / 'ActionSection.swift').read_text())   # so only ActionSection() shows its progress
        home = (swift / 'HomePage.swift').read_text()
        self.assertIn('if model.updating == nil, model.jobRunning {', home)   # Model.jobRunning: building or cancelling
        shown = home[home.index('if model.updating == nil, model.jobRunning'):]
        self.assertIn('ActionSection()', shown[:shown.index('}\n')])

    def test_a_job_shown_under_another_game_names_its_game(self):
        # switched to another card, a job's progress shows at the bottom: its box says whose job it is, as its first
        # line inside the box (a GroupBox label is drawn small, outside the box's top-left corner)
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertEqual(home.count('Text(jobGame).font(.title3.weight(.semibold))'), 2)
        self.assertNotIn('label: { Text(jobGame)', home)

    def test_a_finished_update_leaves_another_game_s_page(self):
        # the game opened (or the player cancelled): its box no longer sits under another game until the next job
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('if let running = model.updating, running != shown?.app, model.phase != .done, model.phase != .cancelled {', home)

    def test_a_finished_job_says_the_game_was_opened(self):
        # the game is opened at once (play(), no wait): 'Starting the game…' would stay on the page for good
        swift = REPO / 'tools/builder_app/Sources/MNGBuilder'
        strings = (swift / 'Strings.swift').read_text()
        for line in ('static let doneOK', 'static let updatedOK'):
            said = strings[strings.index(line):].split('\n')[0]
            self.assertNotIn('Starting the game', said)
            self.assertNotIn('正在启动游戏', said)
        # a job that ends without its 'done' line (build_job.py always prints one) went wrong: no game started on a
        # guess (after PR #2246, which started it there)
        ended = (swift / 'Model.swift').read_text()
        ended = ended[ended.index('private func ended('):ended.index('func play()')]
        self.assertNotIn('play()', ended)


if __name__ == '__main__':
    unittest.main()
