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
        self.assertEqual(PAGES['oot'], ('controls', 'storage', 'saves'))

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


if __name__ == '__main__':
    unittest.main()
