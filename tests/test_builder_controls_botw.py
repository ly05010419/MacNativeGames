"""E9 S9.1: the Controls page for Breath of the Wild. Each game reads and writes its own folder and keeps its own
Xbox/Nintendo answer (C1, C3, C4); a reading of another game than the one given is refused by Save and Restore (C6);
BotW's names (C2) and the icons only for the listed games (C5). Real files in a temporary folder; the page through
MNG Launcher's bridge (tools/builder_jobs/tabs.call) with a fake BotW app."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from test_launcher_tabs import fake_app
from tools.builder import controller_controls as cc, controls_file as cf
from tools.builder.controller_icons import ICONS, IconsInfo, icons_info
from tools.builder.games import BOTW, GAMES, PAGE_GAMES
from tools.builder.gui_controls_table import NAMES, ORDER
from tools.builder.strings import text
from tools.builder_jobs import tabs


def snapshot(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in sorted(folder.rglob('*')) if p.is_file()}


class Files(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp, self.store = Path(tmp), Path(tmp) / 'Builder/controls-tab.json'
        self.dirs = {g: Path(tmp) / GAMES[g].data_folder for g in ('totk', 'botw')}
        for game, folder in self.dirs.items():
            folder.mkdir()
            (folder / 'controller-type.txt').write_text('xbox\n')
            (folder / 'controls.txt').write_bytes(f'# {game}\n'.encode())

    def save(self, game, chosen=None):
        state = cc.read_state(self.dirs[game], self.store, game)
        chosen = chosen or game
        return cc.save(self.dirs[chosen], self.store, chosen, state, cf.swap(state.inputs, 'x', 'l1'))

    def test_each_game_reads_saves_and_restores_its_own_folder(self):   # C1, C4, both ways
        for game, other in (('botw', 'totk'), ('totk', 'botw')):
            with self.subTest(game=game):
                before = snapshot(self.dirs[other])
                result = self.save(game)
                self.assertTrue(result.ok, result)
                self.assertEqual(cf.layout(cf.parse((self.dirs[game] / 'controls.txt').read_bytes()), False)[0]
                                 ['l1'], 'x')
                self.assertTrue((self.dirs[game] / result.backup).is_file())
                restored = cc.restore(self.dirs[game], self.store, game, cc.read_state(self.dirs[game], self.store,
                                                                                      game))
                self.assertTrue(restored.ok and not (self.dirs[game] / 'controls.txt').exists())
                self.assertEqual(snapshot(self.dirs[other]), before)

    def test_a_state_of_the_game_chosen_before_writes_nothing(self):    # C6
        before = {g: snapshot(d) for g, d in self.dirs.items()}
        totk = cc.read_state(self.dirs['totk'], self.store, 'totk')
        self.assertEqual(self.save('totk', chosen='botw').reason, 'other_game')
        self.assertEqual(cc.restore(self.dirs['botw'], self.store, 'botw', totk).reason, 'other_game')
        self.assertEqual({g: snapshot(d) for g, d in self.dirs.items()}, before)

    def test_the_answer_per_game_and_the_old_file(self):                # C3
        for folder in self.dirs.values():
            (folder / 'controller-type.txt').write_text('auto\n')
        self.store.parent.mkdir()
        self.store.write_text(json.dumps({'format': 1, 'controller': 'nintendo'}))
        self.assertEqual((cf.read_answer(self.store, 'totk'), cf.read_answer(self.store, 'botw')), ('nintendo', None))
        self.assertIsNone(cc.read_state(self.dirs['botw'], self.store, 'botw').by_label)     # BotW asks
        self.assertIs(cc.set_answer(self.dirs['botw'], self.store, 'botw', 'xbox').by_label, False)
        self.assertEqual(json.loads(self.store.read_text()),
                         {'format': 2, 'controllers': {'totk': 'nintendo', 'botw': 'xbox'}})
        self.assertTrue(cc.read_state(self.dirs['totk'], self.store, 'totk').by_label)

    def test_only_a_running_game_blocks_its_own_folder(self):          # C4, the real lock
        slot = self.dirs['botw'] / 'game-slot'
        slot.mkdir()
        fd = os.open(slot / 'lock', os.O_CREAT | os.O_RDWR)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX)
        (slot / 'holder').write_text(f'owner=user\ngame_pid={os.getpid()}\n')      # game_state's running game
        self.assertEqual(self.save('botw').reason, 'running')
        self.assertTrue(self.save('totk').ok)

    def test_icons_only_for_the_listed_games_and_names_for_every_game(self):     # C2, C5
        self.assertEqual(ICONS, ('totk', 'botw'))                         # make_botw_button_icons.py
        self.assertEqual(icons_info(self.dirs['botw'], self.store, None, BOTW, None, self.tmp, self.tmp),
                         IconsInfo(False, 'no_app'))
        old = self.tmp / 'Breath of the Wild.app'
        (old / 'Contents/Resources/totk/extracted/botw160').mkdir(parents=True)
        self.assertEqual(icons_info(self.dirs['botw'], self.store, None, BOTW, old, self.tmp, self.tmp),
                         IconsInfo(False, 'not_latest'))                # 1.6.0's code points are not checked
        self.assertEqual(sorted(NAMES), sorted(PAGE_GAMES))   # every game the Controls page serves (2026-11-13: OOT too)
        for game, names in NAMES.items():
            self.assertEqual(tuple(names), ORDER, game)                  # the D-pad's four stay separate rows
        self.assertEqual(cc.read_state(self.dirs['botw'], self.store, 'other').reason, 'not_offered')


class Page(unittest.TestCase):
    """The Controls page of a BotW app reads BotW's folder under the home given, with BotW's names."""

    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp, self.home = Path(tmp), Path(tmp) / 'home'
        self.app = fake_app(self.tmp, 'org.macnativegames.botw', 'botw190')
        support = self.home / 'Library/Application Support/MacNativeGames'
        for folder in ('TOTK', 'BotW'):
            (support / folder).mkdir(parents=True)
            (support / folder / 'controller-type.txt').write_text('xbox\n')
        (support / 'BotW/controls.txt').write_bytes(b'# botw\n')
        env = patch.dict(os.environ, MNG_LANG='en')
        env.start()
        self.addCleanup(env.stop)

    def test_botw_is_read_from_its_folder_with_its_names(self):
        answer = tabs.call('controls', 'state', self.app, {}, home=self.home)
        self.assertTrue(answer['ok'], answer)
        view = answer['result']
        rows = {r['input']: r['name'] for r in view['rows']}
        self.assertEqual((view['game'], view['title']),
                         ('botw', text('gui.controls.title', name='Breath of the Wild')))
        self.assertEqual((rows['zr'], rows['up']), ('Bow', text('gui.controls.unconfirmed', button='D-pad up')))


if __name__ == '__main__':
    unittest.main()
