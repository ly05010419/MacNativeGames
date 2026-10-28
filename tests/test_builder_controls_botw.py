"""E9 S9.1: the Controls tab for Breath of the Wild. Each game reads and writes its own folder and keeps its own
Xbox/Nintendo answer (C1, C3, C4); a reading or a preview of the game chosen before is dropped, and Save and Restore
refuse it (C6); BotW's names (C2) and no icons button (C5). Real files in a temporary folder; the controller part on a
real BuildController with InlineJobs; the view on the FakeController."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from builder_tk_base import Base as ViewBase, tk
from test_builder_controller import Base as ControllerBase
from tools.builder import controller_controls as cc, controls_file as cf
from tools.builder.controller_icons import ICONS, IconsInfo, icons_info
from tools.builder.games import BOTW, GAMES
from tools.builder.gui_controls_table import NAMES, ORDER
from tools.builder.jobs import InlineJobs
from tools.builder.session import ControlsReady, ControlsSaved, IconsChecked
from tools.builder.start_options import StartPaths
from tools.builder.strings import text


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
        self.assertEqual(ICONS, ('totk',))
        self.assertEqual(icons_info(self.dirs['botw'], self.store, None, BOTW, None, self.tmp, self.tmp),
                         IconsInfo(False, 'not_offered'))
        self.assertEqual(sorted(NAMES), sorted(GAMES))
        for game, names in NAMES.items():
            self.assertEqual(tuple(names), ORDER, game)                  # the D-pad's four stay separate rows
        self.assertEqual(cc.read_state(self.dirs['botw'], self.store, 'other').reason, 'not_offered')


class Part(ControllerBase):                                              # the real controller: C1, C6
    def setUp(self):
        super().setUp()
        self.ctl.load_jobs = self.jobs = InlineJobs()
        self.ctl.start.paths = StartPaths(self.tmp / 'Builder/start-options.json', self.tmp / 'data')
        for folder in ('TOTK', 'BotW'):
            (self.tmp / 'data' / folder).mkdir(parents=True)
            (self.tmp / 'data' / folder / 'controller-type.txt').write_text('xbox\n')

    def test_the_chosen_game_is_read_from_its_folder(self):
        (self.tmp / 'data/BotW/controls.txt').write_bytes(b'# botw\n')
        self.ctl.select_game('botw')
        self.ctl.controls.read()
        self.jobs.run_next()
        self.ctl.tick()
        [ready] = [e for e in self.events() if isinstance(e, ControlsReady)]
        self.assertEqual((ready.state.game, ready.state.raw), ('botw', b'# botw\n'))

    def test_a_totk_reading_that_ends_after_botw_is_chosen_cannot_be_saved(self):
        jobs = self.jobs
        self.ctl.select_game('totk')
        self.ctl.controls.read()
        self.ctl.select_game('botw')                                    # before the reading ends
        jobs.run_next()
        self.ctl.tick()
        [ready] = [e for e in self.events() if isinstance(e, ControlsReady)]
        self.assertEqual(ready.state.game, 'totk')
        self.ctl.controls.save(ready.state, cf.swap(ready.state.inputs, 'x', 'l1'))
        jobs.run_next()
        self.ctl.tick()
        [saved] = [e for e in self.events() if isinstance(e, ControlsSaved)]
        self.assertEqual(saved.result.reason, 'other_game')
        self.assertEqual([p.name for p in (self.tmp / 'data').rglob('controls.txt*')], [])


@unittest.skipIf(tk is None, 'no Tk')
class Tab(ViewBase):
    def opened(self, game):
        self.start()
        self.loaded(game)
        self.view.pump()
        self.view.tabs.select('controls')
        return self.view.controls

    def deliver(self, *events):
        self.ctl.events.extend(events)
        self.view.pump()

    def state(self, game):
        inputs, custom = cf.layout(cf.parse(b''), False)
        return cc.ControlsState(game, type_word='xbox', by_label=False, inputs=inputs, custom=custom)

    def test_botw_is_read_with_its_names_and_no_icons_button(self):
        panel = self.opened('botw')
        self.assertEqual((panel.title.cget('text'), self.ctl.controls.calls),
                         (text('gui.controls.title', name='Breath of the Wild'), [('read',)]))
        self.deliver(ControlsReady(self.state('botw')))
        self.assertEqual(panel.rows['zr'][1].cget('text'), 'Bow')
        self.assertEqual(panel.rows['up'][1].cget('text'), text('gui.controls.unconfirmed', button='D-pad up'))
        self.deliver(IconsChecked(IconsInfo(False, 'not_offered')))
        self.assertEqual((panel.icons_button.winfo_manager(), panel.icons_line.cget('text')),
                         ('', text('gui.controls.icons.not.not_offered')))

    def test_a_reading_of_the_game_chosen_before_is_dropped_and_read_again(self):     # C6
        panel = self.opened('botw')
        self.deliver(ControlsReady(self.state('totk')))
        self.assertEqual((panel.state, panel.body.winfo_manager(), self.ctl.controls.calls),
                         (None, '', [('read',), ('read',)]))
        self.deliver(ControlsReady(self.state('botw')))
        self.assertEqual((panel.state.game, panel.body.winfo_manager()), ('botw', 'pack'))
        self.view.tabs.select('build')
        self.radio('totk').invoke()                                     # TOTK chosen: BotW's preview goes
        self.view.tabs.select('controls')
        self.view.pump()
        self.assertEqual((panel.state, panel.body.winfo_manager()), (None, ''))
        self.deliver(IconsChecked(IconsInfo(True)))                     # an icons check ends: not shown, TOTK read
        self.assertEqual((self.ctl.controls.calls[-1], panel.icons_line.cget('text')), (('read',), ''))


if __name__ == '__main__':
    unittest.main()
