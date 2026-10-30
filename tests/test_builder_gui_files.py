"""tools/builder/gui.py and gui_files.py (S2.2): the window's shell and its game and path rows, on a real Tk root that
is never shown (withdrawn), against the scripted FakeController. Skipped when this Python has no Tk. The base is
in builder_tk_base.py (S2.3)."""
import unittest
from unittest.mock import Mock

from builder_fake_controller import FakeController
from builder_tk_base import Base, tk
from tools.builder.session import ProcessError, SettingsLoaded
from tools.builder.strings import text

if tk is not None:
    from tools.builder.gui_files import KINDS
    from tools.builder.gui_style import BUTTON


class Games(Base):
    def test_nothing_before_the_saved_settings(self):
        self.start()
        self.assertEqual(self.view.problems.cget('text'), text('gui.loading'))
        self.assertEqual({r.cget('state') for r in self.view.files.radios}, {'disabled'})
        self.assertEqual({self.entry(f).cget('state') for f in KINDS}, {'disabled'})
        self.loaded()
        self.assertEqual({r.cget('state') for r in self.view.files.radios}, {'normal'})

    def test_no_game_looks_chosen_at_start(self):                              # S4.1 F1
        self.start()
        self.loaded()
        self.assertNotIn(self.view.files.game.get(), {r.cget('value') for r in self.view.files.radios})
        self.assertEqual({r.cget('background') for r in self.view.files.radios}, {BUTTON})

    def test_choosing_a_game_fills_its_saved_paths(self):
        self.start()
        self.loaded('totk')
        self.assertIn(('select_game', 'totk'), self.ctl.calls)
        self.assertEqual((self.entry('keys').cget('state'), self.entry('keys').get()), ('normal', '/t.keys'))

    def test_switching_leaves_nothing_of_the_other_game(self):
        self.start()
        self.loaded('totk')
        self.radio('botw').invoke()
        self.assertEqual([self.entry(f).get() for f in ('game', 'keys', 'update')], ['/b.xci', '/b.keys', ''])

    def test_a_click_commits_what_was_typed_first(self):                       # no FocusOut in aqua
        self.start()
        self.loaded('totk')
        self.type('keys', '/k')
        self.radio('botw').invoke()
        self.assertEqual(self.ctl.calls[-2:], [('set_paths', {'keys': '/k'}), ('select_game', 'botw')])
        before = len(self.ctl.calls)
        self.commit('keys')                                     # refilled: nothing changed since
        self.assertEqual(len(self.ctl.calls), before)

    def test_the_current_game_again_keeps_the_typed_paths(self):
        self.start()
        self.loaded('totk')
        self.type('keys', '/k')
        self.radio('totk').invoke()
        self.assertEqual(self.ctl.calls.count(('select_game', 'totk')), 1)
        self.assertEqual((self.ctl.calls[-1], self.entry('keys').get()), (('set_paths', {'keys': '/k'}), '/k'))

    def test_preselect_waits_for_the_saved_settings(self):
        self.start()
        self.view.preselect('botw', {'keys': '/cli.keys'})
        self.assertEqual(self.ctl.calls, ['load_settings'])
        self.loaded()
        self.assertEqual(self.ctl.calls[1:], [('select_game', 'botw'), ('set_paths', {'keys': '/cli.keys'})])
        self.assertEqual((self.view.files.game.get(), self.entry('keys').get(), self.entry('game').get()),
                         ('botw', '/cli.keys', '/b.xci'))


class Rows(Base):
    def test_commit_on_focus_out_only_when_changed(self):
        self.start()
        self.loaded('totk')
        self.assertEqual(self.root.event_info('<<Commit>>'), ('<Key-Return>', '<FocusOut>'))
        self.type('keys', '/k')
        self.commit('keys', '<FocusOut>')
        self.commit('keys')
        self.assertEqual(self.ctl.calls.count(('set_paths', {'keys': '/k'})), 1)

    def test_an_emptied_row_clears_the_path(self):
        self.start()
        self.loaded('totk')
        self.type('update', '')
        self.commit('update')
        self.assertEqual(self.ctl.calls[-1], ('set_paths', {'update': None}))

    def test_choose(self):
        self.start()
        self.loaded('totk')
        self.assertEqual(tuple(KINDS), self.ctl.path_fields())
        for field, kind in KINDS.items():
            self.view.files.rows[field][1].invoke()             # cancelled: nothing changes
            self.assertEqual(self.picks[-1][0], kind)
        self.assertEqual((self.entry('game').get(), self.picks[0][1]), ('/t.xci', '/t.xci'))
        self.assertEqual(KINDS['out'], 'save')
        self.answer = '/picked.xci'
        self.view.files.rows['game'][1].invoke()
        self.assertEqual(self.entry('game').get(), '/picked.xci')
        self.assertEqual(self.ctl.calls[-1], ('set_paths', {'game': '/picked.xci'}))

    def test_every_field_has_a_label(self):
        for field in FakeController().path_fields():
            self.assertTrue(text(f'gui.field.{field}'))


class Shell(Base):
    def test_settings_problems_are_shown(self):
        self.start(load=[[SettingsLoaded(('format_newer',))]])
        self.loaded()
        self.assertEqual(self.view.problems.cget('text'), 'Saved settings: format_newer')

    def test_a_load_error_is_shown_and_enables_the_games(self):
        self.start(load=[[ProcessError('load', 'boom')]])
        self.loaded()
        self.assertEqual(self.view.problems.cget('text'), 'The saved settings could not be read: boom')
        self.assertEqual({r.cget('state') for r in self.view.files.radios}, {'normal'})

    def test_the_pump_survives_an_error(self):
        self.start()
        self.root.report_callback_exception = Mock()
        self.view.on_event = Mock(side_effect=[ValueError('once')] + [None] * 10)
        self.root.after_cancel(self.view._after)                # only the pump's own reschedule is counted
        self.assertEqual(self.pending(), 0)
        self.view.pump()
        self.assertEqual((self.root.report_callback_exception.call_count, self.pending()), (1, 1))
        ticks = self.ctl.ticks
        self.view.pump()
        self.view.pump()
        self.assertEqual(self.ctl.ticks, ticks + 2)

    def test_a_stuck_load_goes_on_and_a_late_answer_changes_nothing(self):    # S3.4, f4
        self.start(load=[])                                     # the load never answers
        self.view.pump()
        self.assertEqual(self.radio('totk').cget('state'), 'disabled')
        self.now += 10
        self.view.pump()
        self.assertEqual((self.radio('totk').cget('state'), self.view.problems.cget('text')),
                         ('normal', text('gui.load_slow', seconds=10)))
        self.radio('totk').invoke()
        self.type('keys', '/mine')
        self.commit('keys')
        self.ctl.events.append(SettingsLoaded(()))              # the answer comes late
        self.view.pump()
        self.assertEqual((self.entry('keys').get(), self.view.problems.cget('text')), ('/mine', text('gui.load_late')))

    def test_close_is_refused_while_busy(self):
        self.start()
        self.ctl.busy = True
        self.view.close()                                       # S2.3: R6 asks first; the base's confirm answers no
        self.assertEqual((self.root.winfo_exists(), self.ctl.closes), (1, 0))

    def test_close(self):
        self.start()
        self.view.close()
        self.assertEqual((self.ctl.closes, self.after_info()), (1, ''))
        self.view.close()
        self.assertEqual(self.ctl.closes, 1)


if __name__ == '__main__':
    unittest.main()
