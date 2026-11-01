"""tools/builder/gui_play.py (E13 S13.3): the window's Play bar on a real withdrawn Tk root against the FakeController
and its FakeStart. Play is in sight in every tab, says why it cannot play, plays once per click, and owns the Settings
part's reads: a change of selection disables it at once, a late answer for an earlier selection is not shown, a change
during a job is read once the job has ended, a build makes it read again, and refresh() starts no read on its own. Its
line under the bar holds at most two lines. The Settings tab's Play cases moved here from test_builder_gui_settings.
E14 S14.1: each case starts from a Mods state with nothing pending (mods_ready); the Mods rows are in
test_builder_gui_play_mods."""
import unittest

from builder_tk_base import Base
from tools.builder.gui_play import fit
from test_builder_gui_mods import state as mods_state
from test_builder_gui_settings import APP, state
from tools.builder.session import Finished, ModsStateReady, Outcome, PlayDone, ProcessError, StartStateReady
from tools.builder.start_options import PlayResult
from tools.builder.strings import text


class PlayBar(Base):
    def chosen(self, game='totk'):
        """A game chosen: the bar asks for its state once (and nothing more on the next refreshes)."""
        self.start()
        self.loaded(game)
        self.view.pump()
        self.assertEqual(self.ctl.start.calls, [('state',)])
        self.view.pump()
        self.assertEqual(self.ctl.start.calls, [('state',)])        # no read from refresh() alone
        self.mods_ready()
        return self.view.play

    def mods_ready(self, **kw):
        """E14 S14.1: the Mods tab's read of the chosen game and app answers, nothing pending (its tick, then the
        event)."""
        self.deliver(ModsStateReady(mods_state(**kw)))

    def deliver(self, *events):
        self.ctl.events.extend(events)
        self.view.pump()

    def test_in_sight_in_every_tab_and_says_why_it_cannot(self):
        bar = self.chosen()
        self.assertEqual(bar.button.cget('state'), 'disabled')
        self.deliver(StartStateReady(state('v121', 'unknown_version')))
        self.assertEqual((bar.button.cget('state'), bar.line.cget('text')),
                         ('disabled', text('gui.settings.why_not.app')))
        self.deliver(StartStateReady(state()))
        for tab in ('build', 'settings', 'mods', 'controls', 'storage', 'saves'):
            with self.subTest(tab=tab):
                self.view.tabs.select(tab)
                self.view.pump()
                self.assertEqual((bar.button.cget('state'), bar.button.frame.winfo_manager()), ('normal', 'pack'))
        self.ctl.busy = True
        self.view.refresh()
        self.assertEqual((bar.button.cget('state'), bar.line.cget('text')),
                         ('disabled', text('gui.settings.why_not.build')))

    def test_one_click_one_play_and_its_outcome(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state()))
        self.view.tabs.select('mods')                                       # Play from another tab
        self.mods_ready()                                                   # once that tab's own read has ended
        bar.button.invoke()
        bar.button.invoke()
        self.assertEqual(self.ctl.start.calls.count(('play',)), 1)
        self.deliver(PlayDone(PlayResult(False, 'running')))
        self.assertEqual(bar.line.cget('text'), text('gui.settings.play_failed.running'))
        self.deliver(PlayDone(PlayResult(True, held=('interpolation',))))
        self.assertEqual(bar.line.cget('text'), fit(self.view.fonts['small'], text('gui.settings.played') + '\n'
                                                    + text('gui.settings.held.interpolation')))
        self.assertEqual(self.view.settings.status.cget('text'),                # the whole text stays in Settings
                         text('gui.settings.played') + '\n' + text('gui.settings.held.interpolation'))
        self.deliver(ProcessError('play', "OSError('x')"))
        self.assertEqual(bar.line.cget('text'), text('gui.settings.error', error="OSError('x')"))

    def test_a_long_error_is_cut_to_two_lines(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state()))
        self.deliver(ProcessError('play', 'OSError(' + 'a very long reason ' * 60 + ')'))
        shown = bar.line.cget('text')
        self.assertTrue(shown.endswith('…'))
        self.root.update_idletasks()
        line_height = self.view.fonts['small'].metrics('linespace')
        self.assertLessEqual(bar.line.winfo_reqheight(), 2 * line_height + 8)
        self.assertIn('a very long reason ' * 30, self.view.settings.status.cget('text'))

    def test_a_change_of_selection_disables_at_once_and_a_late_answer_is_not_shown(self):
        bar = self.chosen()
        self.ctl.start.pending = False
        self.ctl.set_paths(out='/Games/Other.app')                          # asked for totk's app; now another one
        self.view.refresh()
        self.assertEqual(bar.button.cget('state'), 'disabled')
        self.assertEqual(self.ctl.start.calls.count(('state',)), 2)         # the new selection is read
        self.ctl.set_paths(out='/Games/Third.app')                          # changed again while that read runs
        self.view.refresh()
        self.deliver(StartStateReady(state()))                              # the answer for /Games/Other.app
        self.assertEqual((bar.state, bar.button.cget('state')), (None, 'disabled'))
        self.view.pump()
        self.assertEqual(self.ctl.start.calls.count(('state',)), 3)         # the latest one, once the job ended
        self.mods_ready()                                                   # and the Mods tab's read of it
        self.deliver(StartStateReady(state()))
        self.assertEqual(bar.button.cget('state'), 'normal')

    def test_the_settings_tab_does_not_show_a_late_answer(self):
        self.chosen()
        self.view.tabs.select('settings')
        self.ctl.set_paths(out='/Games/Other.app')
        self.deliver(StartStateReady(state()))                              # asked before the change
        self.assertEqual(self.view.settings.title.cget('text'), text('gui.settings.reading'))

    def test_a_change_during_a_play_is_read_once_it_ends(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state()))
        bar.button.invoke()
        self.ctl.set_paths(out='/Games/Other.app')                          # while Play runs
        self.view.refresh()
        self.assertEqual(self.ctl.start.calls.count(('state',)), 1)         # nothing beside the running job
        self.deliver(PlayDone(PlayResult(True)))
        self.assertEqual(self.ctl.start.calls[-1], ('state',))

    def test_a_failed_read_is_said_and_read_again_only_when_asked(self):
        bar = self.chosen()
        self.view.tabs.select('mods')
        self.deliver(ProcessError('settings', "OSError('disk')"))          # the bar's first read failed
        self.assertEqual((bar.button.cget('state'), bar.line.cget('text')),
                         ('disabled', text('gui.settings.error', error="OSError('disk')")))
        for _ in range(3):
            self.view.pump()
        self.assertEqual(self.ctl.start.calls.count(('state',)), 1)         # no retry on its own
        self.view.tabs.select('settings')                                   # a tab asks: read again
        self.view.pump()
        self.assertEqual(self.ctl.start.calls.count(('state',)), 2)
        self.deliver(StartStateReady(state()))
        self.assertEqual((bar.button.cget('state'), bar.line.cget('text')), ('normal', ''))

    def test_an_earlier_selections_play_outcome_is_not_shown(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state()))
        bar.button.invoke()
        self.ctl.set_paths(out='/Games/Other.app')                          # while that Play runs
        self.view.refresh()
        for late in (PlayDone(PlayResult(False, 'running')), ProcessError('play', "OSError('x')")):
            with self.subTest(late=type(late).__name__):
                self.deliver(late)
                self.assertEqual((bar.line.cget('text'), self.view.settings.status.cget('text')),
                                 (text('gui.play.reading'), ''))            # S14.1: the new app's mods are read
                bar._job, bar._asked = 'play', ('totk', ())                 # as if asked for an earlier one again

    def test_a_build_reads_again(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state(reason='no_app')))
        self.deliver(Finished(Outcome.SUCCESS, 0, None, None, None))
        self.assertEqual(self.ctl.start.calls.count(('state',)), 2)
        self.deliver(StartStateReady(state()))
        self.assertEqual(bar.state.app, APP)

    def test_e12s_hold_disables_it(self):
        bar = self.chosen()
        self.deliver(StartStateReady(state()))
        self.view.tabs.select('settings')
        self.view.pump()
        self.ctl.start.pending = False
        self.deliver(StartStateReady(state()))
        self.view.settings.choose('ground', '16')                           # a choice being saved holds
        self.view.refresh()
        self.assertEqual((self.ctl.start.holds, bar.button.cget('state')), (True, 'disabled'))


if __name__ == '__main__':
    unittest.main()
