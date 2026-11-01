"""tools/builder/gui_play.py (E14 S14.1): the Play bar from the Mods state, on a real withdrawn Tk root against the
FakeController. Each row of S14.1's table with its line; Apply mods, Build and Rebuild clicked once; the sequences after
an Apply and a build; a refused or failed Apply; a late Mods answer; the bar never asks Mods for anything."""
from dataclasses import replace
import unittest

from builder_tk_base import Base
from test_builder_gui_mods import state as mods_state
from test_builder_gui_settings import state
from tools.builder.gui_play import fit
from tools.builder.mod_library import ApplyResult
from tools.builder.session import Finished, ModsApplied, ModsStateReady, Outcome, ProcessError, StartStateReady
from tools.builder.strings import text
from tools.builder.textures import TexturesState

PLAY = text('gui.settings.play')


class PlayFromMods(Base):
    def ready(self, **kw):
        """totk chosen, its Settings state read, and the Mods tab's read of it answered with kw."""
        self.start()
        self.loaded('totk')
        self.view.pump()
        self.deliver(StartStateReady(state()))
        self.mods_said(**kw)
        return self.view.play

    def deliver(self, *events):
        self.ctl.events.extend(events)
        self.view.pump()

    def mods_said(self, **kw):
        """The Mods job running (or the last one) answers kw: its tick sets answered, then the event, as the real
        one does."""
        self.deliver(ModsStateReady(mods_state(**kw)))

    def said(self, key, **kw):
        return fit(self.view.fonts['small'], text(key, **kw))

    def shown(self):
        bar = self.view.play
        return bar.button.cget('text'), bar.button.cget('state'), bar.line.cget('text')

    def test_each_row_of_the_table(self):
        self.ready()
        name = dict(name='Tears of the Kingdom', version='1.4.3')
        heap = dict(present=True, valid=True, heap_pending=True)
        rows = ((dict(reason='not_supported'), (PLAY, 'normal', '')),
                (dict(reason='no_game_file'), (PLAY, 'disabled', self.said('gui.mods.reason.no_game_file'))),
                (dict(reason='no_app'), (text('gui.play.build'), 'normal', self.said('gui.mods.reason.no_app'))),
                (dict(reason='not_latest'), (PLAY, 'normal', self.said('gui.mods.reason.not_latest', **name))),
                (dict(code_pending=True), (text('gui.play.rebuild'), 'normal', self.said('gui.mods.code_pending'))),
                (dict(textures=TexturesState(**heap, enabled=True)),
                 (text('gui.play.rebuild'), 'normal', self.said('gui.textures.build_on'))),
                (dict(textures=TexturesState(**heap)), (text('gui.play.rebuild'), 'normal',
                                                        self.said('gui.play.why.heap_off'))),
                (dict(not_applied=True), (text('gui.play.apply'), 'normal', self.said('gui.play.why.apply'))),
                (dict(code_pending=True, not_applied=True),                                 # both: Rebuild first
                 (text('gui.play.rebuild'), 'normal', self.said('gui.mods.code_pending'))),
                (dict(), (PLAY, 'normal', '')))
        for kw, expected in rows:
            with self.subTest(**{k: str(v) for k, v in kw.items()}):
                self.mods_said(**kw)
                self.assertEqual(self.shown(), expected)
        self.mods_said(not_applied=True)
        self.ctl.busy = True                                                # a build runs
        self.view.refresh()
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'disabled', self.said('gui.settings.why_not.build')))

    def test_nothing_until_the_mods_tab_answers_and_a_failed_read_is_said(self):
        self.start()
        self.loaded('totk')
        self.view.pump()
        self.deliver(StartStateReady(state()))
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.play.reading')))
        self.view.pump()
        self.deliver(ProcessError('mods', "OSError('disk')"))              # the Mods tab's read failed
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.error', error="OSError('disk')")))
        asked = len(self.ctl.mods.calls)
        for _ in range(3):
            self.view.pump()
        self.assertEqual(len(self.ctl.mods.calls), asked)                   # no retry on its own

    def test_apply_mods_once_then_play_only_after_the_read_after_it(self):
        bar = self.ready(not_applied=True)
        bar.button.invoke()
        bar.button.invoke()
        self.assertEqual(self.ctl.mods.calls.count(('apply',)), 1)
        self.view.refresh()
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'disabled', self.said('gui.mods.applying')))
        self.assertNotIn(('play',), self.ctl.start.calls)                  # Apply never launches
        self.deliver(ModsApplied(ApplyResult(True)))
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.play.reading')))   # not Play yet
        self.assertEqual(self.ctl.mods.calls[-1], ('state',))              # the Mods tab reads again
        self.mods_said()
        self.assertEqual(self.shown(), (PLAY, 'normal', ''))

    def test_a_refused_or_failed_apply_stays_apply_mods_with_its_reason(self):
        bar = self.ready(not_applied=True)
        bar.button.invoke()
        self.deliver(ModsApplied(ApplyResult(False, 'running')))
        refused = self.said('gui.mods.applied.running')
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'disabled', self.said('gui.play.reading')))
        self.mods_said(not_applied=True)                                    # that read keeps the reason
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'normal', refused))
        bar.button.invoke()
        self.deliver(ProcessError('mods', "OSError('x')"))
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'normal',
                                        self.said('gui.error', error="OSError('x')")))
        self.assertEqual(self.ctl.mods.calls.count(('apply',)), 2)

    def test_build_and_rebuild_open_the_build_tab_and_check_but_start_nothing(self):
        for kw in (dict(reason='no_app'), dict(code_pending=True)):
            with self.subTest(**{k: str(v) for k, v in kw.items()}):
                bar = self.ready(**kw)
                self.view.tabs.select('mods')                       # the tab's own full read, answered
                self.mods_said(**kw)
                bar.button.invoke()
                self.assertEqual((self.view.tabs.current, self.ctl.calls.count('run_checks')), ('build', 1))
                self.assertNotIn('start_build', self.ctl.calls)
                self.doCleanups()

    def test_rebuild_then_apply_mods_then_play(self):
        bar = self.ready(code_pending=True, not_applied=True)
        self.assertEqual(self.shown()[0], text('gui.play.rebuild'))
        self.deliver(Finished(Outcome.SUCCESS, 0, None, None, None))      # the build the checks led to
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.play.reading')))
        self.mods_said(not_applied=True)                                    # the Mods tab's read after the build
        self.deliver(StartStateReady(state()))                              # and the bar's own
        self.assertEqual(self.shown()[:2], (text('gui.play.apply'), 'normal'))
        bar.button.invoke()
        self.deliver(ModsApplied(ApplyResult(True)))
        self.assertEqual(self.shown()[:2], (PLAY, 'disabled'))
        self.mods_said()
        self.assertEqual(self.shown(), (PLAY, 'normal', ''))

    def test_a_switch_turned_off_after_an_apply_asks_for_apply_again(self):
        bar = self.ready()
        self.view.mods.state = mods_state()
        self.ctl.mods.enable('a', False)                                    # the Mods tab's change, answered
        self.mods_said(not_applied=True)
        self.assertEqual(self.shown(), (text('gui.play.apply'), 'normal', self.said('gui.play.why.apply')))
        bar.button.invoke()
        self.assertEqual(self.ctl.mods.calls[-1], ('apply',))

    def test_a_late_mods_answer_for_another_app_is_not_used(self):
        self.ready()
        self.ctl.set_paths(out='/Games/Other.app')                          # the Mods tab reads Other.app
        self.view.refresh()
        self.ctl.set_paths(out='/Games/Third.app')                          # changed again while that read runs
        self.view.refresh()
        self.deliver(ModsStateReady(mods_state()))                          # Other.app's read ends: its answer
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.play.reading')))
        self.assertEqual(self.ctl.mods.asked[1][2], '/Games/Third.app')     # the tab asked for Third.app now
        self.mods_said(not_applied=True)                                    # its answer
        self.assertEqual(self.shown()[0], text('gui.play.apply'))

    def test_play_waits_for_a_mods_read_that_does_not_hold(self):
        bar = self.ready()
        self.ctl.mods.state(full=False)                                     # a quick read: the controller stays idle
        self.view.refresh()
        self.assertEqual((self.ctl.idle, self.shown()), (True, (PLAY, 'disabled', self.said('gui.play.reading'))))
        bar.play()
        self.assertNotIn(('play',), self.ctl.start.calls)
        self.mods_said()
        self.assertEqual(self.shown(), (PLAY, 'normal', ''))

    def test_the_settings_state_asks_for_a_build_for_the_picture(self):                 # E17 S17.1
        self.ready(not_applied=True)
        for why in ('picture_4k', 'native_4k_8gb'):
            with self.subTest(why=why):
                self.deliver(StartStateReady(replace(state(), rebuild=why)))
                self.assertEqual(self.shown(), (text('gui.play.rebuild'), 'normal', self.said(f'gui.play.why.{why}')))
        self.deliver(StartStateReady(state()))
        self.assertEqual(self.shown()[0], text('gui.play.apply'))

    def test_the_bar_never_asks_mods_for_anything(self):
        self.ready()
        self.view.mods.refresh = lambda idle: None                          # the Mods tab asks nothing either
        before = list(self.ctl.mods.calls)
        self.ctl.set_paths(out='/Games/Other.app')
        for _ in range(3):
            self.view.pump()
        self.deliver(Finished(Outcome.FAILED, 1, None, None, 'x'))
        self.assertEqual(self.ctl.mods.calls, before)
        self.assertEqual(self.shown(), (PLAY, 'disabled', self.said('gui.play.reading')))


if __name__ == '__main__':
    unittest.main()
