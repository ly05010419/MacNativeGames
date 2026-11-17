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
        self.assertIn('CoverFlow(count: cases.count, index: center)', home)
        # the built games first, then the ones not built yet (user 2026-11-17); the chosen one is kept by game, not place
        self.assertIn('return built + Self.games.filter { !built.contains($0) }', home)
        self.assertIn('let cases = order', home)
        self.assertIn('set: { chosen = order[$0] }', home)
        flow =(REPO / 'tools/builder_app/Sources/MNGBuilder/CoverFlow.swift').read_text()
        for way in ('chevron.left', 'chevron.right', '.onKeyPress(.leftArrow)', '.onKeyPress(.rightArrow)', 'DragGesture('):
            self.assertIn(way, flow)

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
        home = (REPO / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('CroppedPicture(picture: picture, center: crops.center(of: picturePath))', home)
        art = (REPO / 'tools/builder_app/Sources/MNGBuilder/CoverArt.swift').read_text()
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
