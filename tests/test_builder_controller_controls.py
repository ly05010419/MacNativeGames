"""E6 S6.2b: controls_file.render's writing rules (C4, revision 3) and tools/builder/controller_controls.py's reading,
saving and restoring (C5-C8) on real files in a temporary folder; the controller part on a real BuildController with
InlineJobs. The game's own parser is compared in test_builder_controls_file.py."""
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from test_builder_controller import Base as ControllerBase
from tools.builder import controller_controls as cc, controls_file as cf
from tools.builder.jobs import InlineJobs
from tools.builder.session import ControlsReady, ControlsSaved, IconsDone
from tools.builder.start_options import StartPaths


def effective(data, by_label=False):
    return cf.layout(cf.parse(data), by_label)[0]


class Render(unittest.TestCase):
    def edit(self, original, picks, by_label=False, converted=()):
        inputs, custom = cf.layout(cf.parse(original), by_label)
        for switch_input, drawn in picks:
            inputs = cf.swap(inputs, switch_input, drawn)
        owned = {d for d in cf.DRAWN if d not in custom} | set(converted)
        return cf.render(original, inputs, by_label, owned), inputs

    def test_comments_and_other_lines_stay_and_one_block_is_added(self):
        original = b'# my layout\ntouchpad = minus\nview = minus  # kept\n'
        new, want = self.edit(original, [('x', 'l1')])
        self.assertTrue(new.startswith(original))
        self.assertEqual(new.count(cf.HEADER), 1)
        self.assertEqual(effective(new), want)
        again, want = self.edit(new, [('y', 'r1')])
        self.assertEqual((again.count(cf.HEADER), again.count(cf.END), effective(again)), (1, 1, want))
        self.assertTrue(again.startswith(original))

    def test_unconverted_custom_buttons_keep_target_and_mode(self):           # revision 3
        original = b'l1 = r tap\nr2 = zr aim l2\n'
        new, _ = self.edit(original, [('x', 'south')])
        before, after = cf.parse(original), cf.parse(new)
        for name in ('l1', 'l2', 'r2'):
            i = cf.PHYSICAL.index(name)
            self.assertEqual((after.map[i], after.modes[i]), (before.map[i], before.modes[i]), name)

    def test_a_dead_custom_line_goes_with_a_changed_button(self):
        original = b'l1 = r tap\nl1 = r\nr1 = l\n'                        # LB and RB swapped; a dead tap line
        self.assertEqual(cf.layout(cf.parse(original), False)[1], {})
        new, want = self.edit(original, [('l', 'l1')])
        self.assertEqual((effective(new), new), (want, b''))                # all of LB's lines gone, RB's too

    def test_a_hand_line_after_the_block_still_wins_and_is_kept(self):
        first, _ = self.edit(b'', [('x', 'l1')])
        hand = first + b'south = b  # mine\n'
        new, want = self.edit(hand, [('zl', 'menu')])
        block = new[new.index(cf.HEADER):]
        self.assertTrue(b'south = b  # mine\n' in new and b'south' not in block)   # kept, and still the one that counts
        self.assertEqual(effective(new), want)

    def test_a_header_left_alone_by_hand_drops_only_itself(self):
        original = cf.HEADER + b'\n# my notes after it\nview = minus\n'
        new, want = self.edit(original, [('x', 'l1')])
        self.assertTrue(b'# my notes after it\nview = minus\n' in new and new.count(cf.HEADER) == 1)
        self.assertEqual(effective(new), want)

    def test_the_xbox_letters_preset(self):
        for start in (effective(b''), cf.swap(effective(b''), 'x', 'l1')):
            letters = cf.xbox_letters(start)
            self.assertEqual([letters[d] for d in ('south', 'east', 'west', 'north')], ['a', 'b', 'x', 'y'])
            self.assertEqual(sorted(letters.values()), sorted(start.values()))     # every input, once each
            self.assertEqual(cf.xbox_letters(letters), letters)
        new, want = self.edit(b'', [('a', 'south'), ('b', 'east'), ('x', 'west'), ('y', 'north')])
        self.assertEqual(want, cf.xbox_letters(effective(b'')))
        self.assertEqual(effective(new), want)
        self.assertEqual(cf.xbox_letters(effective(b'', True)), effective(b'', True))   # by label: already so

    def test_label_mode_writes_the_names_that_mode_reads(self):
        new, want = self.edit(b'', [('x', 'south')], by_label=True)
        self.assertIn(b'east = x', new)                                   # the bottom button is 'east' by label
        self.assertEqual(effective(new, True), want)

    def test_a_converted_none_and_the_size_limit(self):
        new, want = self.edit(b'south = none\n', [('x', 'south')], converted=('south',))
        self.assertEqual((effective(new), b'north = none' in new), (want, True))
        with self.assertRaises(ValueError):
            cf.render(b'#' * 16380 + b'\n', cf.swap(effective(b''), 'x', 'l1'), False, set(cf.DRAWN))


class Files(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.data, self.store = Path(tmp) / 'TOTK', Path(tmp) / 'Builder/controls-tab.json'
        self.data.mkdir()
        self.controls = self.data / 'controls.txt'
        (self.data / 'controller-type.txt').write_text('xbox\n')
        running = patch.object(cc.game_state, 'game_running', return_value=False)
        self.running = running.start()
        self.addCleanup(running.stop)

    def save(self, state, picks, converted=()):
        desired = state.inputs
        for switch_input, drawn in picks:
            desired = cf.swap(desired, switch_input, drawn)
        return cc.save(self.data, self.store, 'totk', state, desired, converted), desired

    def test_auto_asks_and_the_answer_is_the_builders(self):
        (self.data / 'controller-type.txt').write_text('auto\n')
        self.assertIsNone(cc.read_state(self.data, self.store, 'totk').by_label)
        self.assertTrue(cc.set_answer(self.data, self.store, 'totk', 'nintendo').by_label)
        self.assertEqual(json.loads(self.store.read_text())['controllers'], {'totk': 'nintendo'})     # S9.1 C3
        (self.data / 'controller-type.txt').write_text(' xbox\n')
        self.assertIs(cc.read_state(self.data, self.store, 'totk').by_label, False)     # the type wins over the answer

    def test_save_backs_up_and_writes_atomically(self):
        self.controls.write_bytes(b'# mine\n')
        result, desired = self.save(cc.read_state(self.data, self.store, 'totk'), [('x', 'l1')])
        self.assertTrue(result.ok, result)
        self.assertEqual(((self.data / result.backup).read_bytes(), effective(self.controls.read_bytes())),
                         (b'# mine\n', desired))
        second, _ = self.save(cc.read_state(self.data, self.store, 'totk'), [('y', 'r1')])
        self.assertNotEqual(second.backup, result.backup)                       # never overwritten
        self.assertEqual(len(list(self.data.glob('controls.txt.before-*'))), 2)
        self.assertEqual(list(self.data.glob('*.partial')), [])

    def test_save_keeps_unconverted_custom_lines(self):                     # the audit's B2: through save()
        self.controls.write_bytes(b'l1 = r tap\nr2 = zr aim l2\n')
        result, desired = self.save(cc.read_state(self.data, self.store, 'totk'), [('a', 'south')])
        self.assertTrue(result.ok, result)
        before, after = cf.parse(b'l1 = r tap\nr2 = zr aim l2\n'), cf.parse(self.controls.read_bytes())
        for name in ('l1', 'l2', 'r2'):
            i = cf.PHYSICAL.index(name)
            self.assertEqual((after.map[i], after.modes[i]), (before.map[i], before.modes[i]), name)
        self.assertEqual((effective(self.controls.read_bytes())['south'], b'l1 = r tap' in self.controls.read_bytes()),
                         ('a', True))

    def test_a_state_without_a_mode_is_refused(self):
        state = cc.read_state(self.data, self.store, 'totk')
        modeless = cc.ControlsState('totk', raw=state.raw, type_raw=state.type_raw)
        self.assertEqual(cc.save(self.data, self.store, 'totk', modeless, state.inputs).reason, 'changed')
        (self.data / 'controller-type.txt').unlink()                     # auto with no answer: no mode to write in
        blind = cc.read_state(self.data, self.store, 'totk')
        self.assertEqual(cc.save(self.data, self.store, 'totk', blind, state.inputs).reason, 'refused')
        self.assertFalse(self.controls.exists())

    def test_nothing_is_written_while_running_or_after_a_change(self):
        self.controls.write_bytes(b'# mine\n')
        state = cc.read_state(self.data, self.store, 'totk')
        self.running.return_value = True
        self.assertEqual(self.save(state, [('x', 'l1')])[0].reason, 'running')
        self.running.return_value = False
        self.controls.write_bytes(b'# edited meanwhile\n')
        self.assertEqual(self.save(state, [('x', 'l1')])[0].reason, 'changed')
        state = cc.read_state(self.data, self.store, 'totk')
        (self.data / 'controller-type.txt').write_text('switch\n')
        self.assertEqual(self.save(state, [('x', 'l1')])[0].reason, 'changed')
        self.assertEqual((self.controls.read_bytes(), list(self.data.glob('controls.txt.before-*'))),
                         (b'# edited meanwhile\n', []))

    def test_a_link_is_written_through_and_restore_moves_it_aside(self):
        target = self.data.parent / 'mine.txt'
        target.write_bytes(b'# linked\n')
        self.controls.symlink_to(target)
        result, desired = self.save(cc.read_state(self.data, self.store, 'totk'), [('x', 'l1')])
        self.assertTrue(result.ok and self.controls.is_symlink())
        self.assertEqual(effective(target.read_bytes()), desired)
        restored = cc.restore(self.data, self.store, 'totk', cc.read_state(self.data, self.store, 'totk'))
        self.assertTrue(restored.ok and not os.path.lexists(self.controls))
        self.assertTrue((self.data / restored.backup).is_symlink() and target.exists())   # nothing deleted

    def test_restore_without_a_file_does_nothing(self):
        state = cc.read_state(self.data, self.store, 'totk')
        self.assertEqual(cc.restore(self.data, self.store, 'totk', state), cc.SaveResult(True))


class Part(ControllerBase):
    def test_save_holds_the_controller_and_reports(self):
        tmp = Path(self.tmp)
        self.ctl.load_jobs = jobs = InlineJobs()
        self.ctl.start.paths = StartPaths(tmp / 'Builder/start-options.json', tmp / 'data')
        (tmp / 'data/TOTK').mkdir(parents=True)
        (tmp / 'data/TOTK/controller-type.txt').write_text('xbox\n')
        self.ctl.select_game('totk')
        self.ctl.controls.read()
        jobs.run_next()
        self.ctl.tick()
        [ready] = [e for e in self.events() if isinstance(e, ControlsReady)]
        with patch.object(cc.game_state, 'game_running', return_value=False):
            self.ctl.controls.save(ready.state, cf.swap(ready.state.inputs, 'x', 'l1'))
            self.assertFalse(self.ctl.idle)
            self.assertRaises(RuntimeError, self.ctl.close)
            jobs.run_next()
            self.ctl.tick()
        [saved] = [e for e in self.events() if isinstance(e, ControlsSaved)]
        self.assertTrue(saved.result.ok and self.ctl.idle)


class IconsHandoff(ControllerBase):                                        # E6 S6.3 B5
    def test_made_icons_go_to_the_mods_part_and_the_controller_stays_held(self):
        from tools.builder import controller_icons, controller_mods
        tmp = Path(self.tmp)
        self.ctl.load_jobs = jobs = InlineJobs()
        self.ctl.start.paths = StartPaths(tmp / 'Builder/start-options.json', tmp / 'data')
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(tmp / 'g.xci'), keys=str(tmp / 'k'))
        built = controller_icons.IconsBuilt(True, work=str(tmp / 'work'), made_from={'controls': 'h'})
        installed = controller_mods.IconsResult(True)
        with patch.object(controller_icons, 'make_icons', return_value=built), \
                patch.object(controller_mods, 'install_icons', return_value=installed) as install:
            self.ctl.controls.update_icons(True, ['xbox-icons'])
            self.ctl.mods.state()                                       # a Mods job queued meanwhile
            jobs.run_next()
            self.ctl.tick()
            self.assertFalse(self.ctl.idle)                              # made, waiting for the Mods part
            jobs.run_next()                                             # the Mods read
            self.ctl.tick()
            self.ctl.tick()                                             # handed over now
            jobs.run_next()
            self.ctl.tick()
        self.assertEqual(install.call_args.args[-3:], (tmp / 'work', {'controls': 'h'}, ('xbox-icons',)))
        self.assertIn(IconsDone(installed), self.events())
        self.assertTrue(self.ctl.idle)


if __name__ == '__main__':
    unittest.main()
