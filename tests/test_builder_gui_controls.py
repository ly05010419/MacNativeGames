"""tools/builder/gui_controls.py and gui_pad.py (E6 S6.2, S6.2b) on a real withdrawn Tk root against the
FakeController (its FakeControls), with states made by the real model; gui_controls_table.py and controls_file's
default against runtime/controller_map.h."""
from pathlib import Path
import re
import unittest

from builder_tk_base import Base, tk
from tools.builder import controls_file as cf, gui_controls_table as table
from tools.builder.controller_controls import ControlsState, SaveResult
from tools.builder.session import ControlsReady, ControlsSaved
from tools.builder.strings import text

if tk is not None:
    from tools.builder.gui_style import ACCENT, BUTTON

HEADER = Path(__file__).resolve().parents[1] / 'games/totk/runtime/controller_map.h'


def names(array):
    body = re.search(array + r'\[[A-Z_]+\] = \{(.*?)\};', HEADER.read_text(), re.S)[1]
    return re.findall(r'"([a-z0-9_]+)"', body)


def state(raw=b'', by_label=False, type_word='xbox', answer=None):
    parsed = cf.parse(raw)
    inputs, custom = cf.layout(parsed, by_label) if by_label is not None and not parsed.line else ({}, {})
    return ControlsState('totk', type_word=type_word, answer=answer, by_label=by_label, raw=raw or None,
                         inputs=inputs, custom=custom, error=(parsed.line, parsed.error) if parsed.line else ())


class Table(unittest.TestCase):
    def test_the_names_are_the_runtimes(self):
        switch, physical = names('controller_switch_names'), names('controller_physical_names')
        self.assertEqual(sorted(table.ORDER), sorted(switch))
        self.assertEqual(sorted(p for p, _ in table.PHYSICAL), sorted(physical[:16]))   # no touchpad, no mouse
        self.assertEqual((list(cf.PHYSICAL), list(cf.SWITCH)), (physical, switch))

    def test_the_models_default_is_controller_map_default(self):
        targets, modes = cf.default()
        self.assertIn('map[0] = 1u << 1; map[1] = 1u << 0; map[2] = 1u << 3; map[3] = 1u << 2;', HEADER.read_text())
        self.assertEqual(targets[:16], [1, 0, 3, 2] + list(range(4, 16)))
        self.assertEqual((targets[17:20], modes[18]), ([3, 9, 8], cf.AIM | 17 << 3))
        for by_label in (False, True):                                   # B2: no custom row in the default layout
            self.assertEqual(cf.layout(cf.parse(b''), by_label)[1], {})


class ControlsTab(Base):
    def opened(self, *events):
        self.start()
        self.loaded('totk')
        self.view.pump()
        self.view.tabs.select('controls')
        self.ctl.events.extend(events)
        self.view.pump()
        return self.view.controls

    def test_opening_reads_and_shows_the_layout(self):
        panel = self.opened(ControlsReady(state()))
        self.assertEqual(self.ctl.controls.calls[0], ('read',))
        self.assertEqual((panel.title.cget('text'), panel.rows['x'][2].cget('text')),
                         (text('gui.controls.title', name='Tears of the Kingdom'), 'Y'))
        self.assertEqual(panel.mode.cget('text'), text('gui.controls.mode.xbox'))
        self.assertEqual(panel.save_button.cget('state'), 'disabled')

    def test_a_pick_and_a_press_swap_and_save_sends_it(self):
        panel = self.opened(ControlsReady(state()))
        panel.pick('x')
        self.assertEqual((panel.pad.look('north'), panel.rows['x'][0].cget('background')), ((ACCENT, 3.0), ACCENT))
        panel.press('l1')
        self.assertEqual((panel.rows['x'][2].cget('text'), panel.rows['l'][2].cget('text')), ('→ LB', '→ Y'))
        self.assertEqual((panel.pad.look('l1'), panel.pad.look('north')), ((ACCENT, 3.0), (BUTTON, 1.0)))
        self.assertEqual(panel.caption.cget('text'), text('gui.controls.pending', name='Jump', button='LB'))
        self.view.pump()
        self.assertEqual((panel.save_button.cget('state'), panel.cancel_button.cget('state')), ('normal', 'normal'))
        panel.save_button.invoke()
        call = self.ctl.controls.calls[-1]
        self.assertEqual((call[0], call[1]['l1'], call[1]['north'], call[2]), ('save', 'x', 'l', ()))
        self.assertFalse(self.ctl.idle)                                  # Save holds the controller until it ends

    def test_the_xbox_letters_preset_in_one_click(self):
        panel = self.opened(ControlsReady(state()))
        panel.preset_button.invoke()
        self.assertEqual([panel.rows[s][2].cget('text') for s in ('a', 'b', 'x', 'y')],
                         ['→ A', '→ B', '→ X', '→ Y'])
        self.assertEqual(panel.caption.cget('text'), text('gui.controls.preset.done'))
        self.view.pump()
        panel.save_button.invoke()
        sent = self.ctl.controls.calls[-1][1]
        self.assertEqual([sent[d] for d in ('south', 'east', 'west', 'north')], ['a', 'b', 'x', 'y'])

    def test_the_preset_asks_before_moving_a_custom_button(self):
        panel = self.opened(ControlsReady(state(b'east = a tap\n')))
        panel.preset_button.invoke()                                      # the confirmation says no
        self.assertEqual((len(self.asked), panel.desired, panel.converted), (1, panel.state.inputs, set()))
        self.confirm_answer = True
        panel.preset_button.invoke()
        self.assertEqual((panel.converted, panel.desired['south']), ({'east'}, 'a'))

    def test_cancel_puts_the_layout_back(self):
        panel = self.opened(ControlsReady(state()))
        panel.pick('x')
        panel.press('l1')
        panel.cancel()
        self.assertEqual(panel.rows['x'][2].cget('text'), 'Y')

    def test_a_custom_button_is_converted_only_when_the_player_agrees(self):
        panel = self.opened(ControlsReady(state(b'l1 = r tap\n')))
        self.assertEqual(panel.rows['r'][2].cget('text'), 'LB · ' + text('gui.controls.custom.tap'))
        panel.pick('x')
        panel.press('l1')                                               # the confirmation says no
        self.assertEqual((len(self.asked), panel.rows['x'][2].cget('text')), (1, 'Y'))
        self.confirm_answer = True
        panel.press('l1')
        self.assertEqual((panel.converted, panel.rows['x'][2].cget('text')), ({'l1'}, '→ LB'))
        self.view.pump()
        panel.save_button.invoke()
        self.assertEqual(self.ctl.controls.calls[-1][2], ('l1',))

    def test_auto_asks_which_controller_first(self):
        panel = self.opened(ControlsReady(state(by_label=None, type_word='auto')))
        self.assertEqual((panel.ask.winfo_manager(), panel.body.winfo_manager()), ('pack', ''))
        self.view.pump()
        panel.answer('nintendo')
        self.assertEqual(self.ctl.controls.calls[-1], ('answer', 'nintendo'))
        self.ctl.events.append(ControlsReady(state(by_label=True, type_word='auto', answer='nintendo')))
        self.view.pump()
        self.assertEqual((panel.mode.cget('text'), panel.rows['a'][2].cget('text')),
                         (text('gui.controls.mode.auto.nintendo'), 'A'))     # by label: the A button is A

    def test_a_broken_file_offers_only_restore(self):
        panel = self.opened(ControlsReady(state(b'south = q\n')))
        self.assertEqual(panel.mode.cget('text'), text('gui.controls.broken', line=1, error=cf.ERRORS['switch']))
        self.assertEqual(panel.body.winfo_manager(), '')
        self.view.pump()
        self.assertEqual(panel.restore_button.cget('state'), 'normal')
        self.confirm_answer = True
        panel.restore_button.invoke()
        self.assertEqual(self.ctl.controls.calls[-1], ('restore',))

    def test_the_result_is_said_and_the_files_read_again(self):
        panel = self.opened(ControlsReady(state()))
        self.ctl.events.append(ControlsSaved(SaveResult(True, backup='controls.txt.before-1')))
        self.view.pump()
        self.assertEqual(panel.status.cget('text'), text('gui.controls.saved', backup='controls.txt.before-1'))
        self.assertEqual(self.ctl.controls.calls[-1], ('read',))
        self.ctl.events.append(ControlsSaved(SaveResult(False, 'changed')))
        self.view.pump()
        self.assertEqual(panel.status.cget('text'), text('gui.controls.not_saved.changed', detail=''))

    def test_the_icons_line_and_the_update_with_its_two_questions(self):    # E6 S6.3
        from tools.builder.controller_controls import controller_icons
        from tools.builder.controller_mods import IconsResult
        from tools.builder.session import IconsChecked, IconsDone
        panel = self.opened(ControlsReady(state()))
        self.assertEqual(self.ctl.controls.calls[-1], ('icons_check',))
        info = controller_icons.IconsInfo(True, env='missing', status='outdated', font_mods=(('xbox-icons', 'XI'),))
        self.ctl.events.append(IconsChecked(info))
        self.view.pump()
        self.assertEqual(panel.icons_line.cget('text'), text('gui.controls.icons.outdated'))
        self.assertEqual(panel.icons_button.cget('state'), 'normal')
        panel.icons_button.invoke()                                    # no to the download: nothing runs
        self.assertEqual((len(self.asked), self.ctl.controls.calls[-1]), (1, ('icons_check',)))
        self.confirm_answer = True
        panel.icons_button.invoke()
        self.assertEqual(self.asked[-2:], [text('gui.controls.icons.download.missing'),
                                           text('gui.controls.icons.switch_off', mods='XI')])
        self.assertEqual(self.ctl.controls.calls[-1], ('update_icons', True, ('xbox-icons',)))
        self.ctl.events.append(IconsDone(IconsResult(True)))
        self.view.pump()
        self.assertEqual((panel.status.cget('text'), self.ctl.controls.calls[-1]),
                         (text('gui.controls.icons.done'), ('icons_check',)))
        nintendo = controller_icons.IconsInfo(False, 'nintendo')
        self.ctl.events.append(IconsChecked(nintendo))
        self.view.pump()
        self.assertEqual((panel.icons_line.cget('text'), panel.icons_button.cget('state')),
                         (text('gui.controls.icons.not.nintendo'), 'disabled'))

    def test_every_button_is_drawn_and_clickable(self):
        panel = self.opened(ControlsReady(state()))
        for physical, _ in table.PHYSICAL:
            with self.subTest(physical=physical):
                self.assertTrue(panel.pad.canvas.find_withtag(physical))
                self.assertTrue(panel.pad.canvas.tag_bind(physical, '<Button-1>'))


if __name__ == '__main__':
    unittest.main()
