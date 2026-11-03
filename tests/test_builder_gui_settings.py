"""tools/builder/gui_settings.py (S5.1): the Settings tab on a real withdrawn Tk root (builder_tk_base.py) against the
scripted FakeController and its FakeStart. States are made with the model's own StartState and OPTIONS, so every
option and choice the model can send is rendered here (a missing text would raise)."""
from pathlib import Path
import unittest

from builder_tk_base import Base, tk
from tools.builder.session import GpuProbed, PlayDone, ProcessError, StartStateReady
from tools.builder.start_options import OPTIONS, PlayResult, StartState
from tools.builder.strings import text

APP = Path('/Games/Tears of the Kingdom.app')
if tk is not None:
    from tools.builder.gui_style import ACCENT, FIELD


def state(version='v143', reason='', **choices):
    options = OPTIONS.get(version, ()) if not reason else ()
    label = {'v143': '1.4.3', 'botw190': '1.9.0'}.get(version, '')
    return StartState('totk', APP, version, reason, options, {o.id: choices.get(o.id, 'auto') for o in options},
                      name='Tears of the Kingdom', version_label=label)


class Settings(Base):
    def opened(self, game='totk'):
        self.start()
        self.loaded(game)
        self.view.pump()
        self.view.tabs.select('settings')
        return self.view.settings

    def deliver(self, *events):
        self.ctl.events.extend(events)
        self.view.pump()

    def radios(self, option):
        """The option's segments (gui_widgets.Segmented, since aqua's radio buttons hid the choice)."""
        return self.view.settings.selectors[option].segments

    def test_without_a_game_it_says_so(self):
        self.start()
        self.loaded()
        self.view.tabs.select('settings')
        self.assertEqual(self.view.settings.title.cget('text'), text('gui.settings.choose_game'))
        self.assertEqual(self.ctl.start.calls, [])

    def test_opening_asks_for_the_state_and_probes_once(self):
        self.opened()
        self.assertEqual(self.ctl.start.calls, [('probe',), ('state',)])
        self.assertEqual((self.view.settings.foot.winfo_manager(), self.view.checks.foot.winfo_manager()), ('pack', ''))

    def test_every_option_and_choice_of_both_versions_renders(self):
        panel = self.opened()
        for version in OPTIONS:
            with self.subTest(version=version):
                self.deliver(StartStateReady(state(version)))
                self.assertEqual(len(panel.rows.winfo_children()), len(OPTIONS[version]))
                self.assertEqual(sum(len(s.segments) for s in panel.selectors.values()),
                                 sum(len(o.choices) for o in OPTIONS[version]))
                self.assertEqual({v.get() for v in panel.vars.values()}, {'auto'})

    def test_each_reason_has_its_line(self):
        panel = self.opened()
        for reason in ('no_game_file', 'no_app', 'unknown_version'):
            self.deliver(StartStateReady(state('v121', reason)))
            self.assertEqual(panel.title.cget('text'), text(f'gui.settings.reason.{reason}', app=APP))
            self.assertEqual(panel.play_button.cget('state'), 'disabled')

    def test_a_choice_goes_to_the_controller_once(self):
        panel = self.opened()
        self.deliver(StartStateReady(state()))
        self.ctl.start.pending = False
        interp = self.radios('interpolation')
        interp[2].invoke()                                                # half the refresh rate
        interp[3].invoke()                                                # before the pump: the selectors wait
        self.assertEqual([c for c in self.ctl.start.calls if c[0] == 'choose'], [('choose', 'interpolation', 'interp')])
        self.assertEqual((panel.vars['interpolation'].get(), self.callback_errors), ('interp', []))
        panel.choose('ground', '16')                                      # a command that slips through does nothing
        self.assertEqual((panel.vars['ground'].get(), self.ctl.start.calls.count(('choose', 'ground', '16'))),
                         ('auto', 0))

    def test_the_chosen_choice_is_painted_and_follows_a_click(self):          # aqua hides it
        self.opened()
        self.deliver(StartStateReady(state(smoothing='on')))
        self.ctl.start.pending = False
        for option, chosen in (('smoothing', 'on'), ('interpolation', 'auto')):
            colours = {b.value: b.cget('background') for b in self.radios(option)}
            self.assertEqual({v for v, c in colours.items() if c == ACCENT}, {chosen})
            self.assertEqual({c for v, c in colours.items() if v != chosen}, {FIELD})
        self.radios('interpolation')[1].invoke()                          # 30+30
        colours = {b.value: b.cget('background') for b in self.radios('interpolation')}
        self.assertEqual({v for v, c in colours.items() if c == ACCENT}, {'interp30'})

    def test_the_gpu_decides_the_interpolation_row(self):
        panel = self.opened()
        self.deliver(StartStateReady(state()))
        self.assertIn(text('gui.settings.gpu.checking'), self.labels(panel.rows))
        self.ctl.start.gpu = 'no'
        self.deliver(GpuProbed('no'))
        self.assertNotIn('interpolation', panel.vars)
        self.assertIn(text('gui.settings.gpu.no'), self.labels(panel.rows))
        self.ctl.start.gpu = 'unknown'
        self.deliver(GpuProbed('unknown'))
        self.assertIn(text('gui.settings.gpu.unknown'), self.labels(panel.rows))

    def test_play(self):
        panel = self.opened()
        self.deliver(StartStateReady(state()))
        self.assertEqual(panel.play_button.cget('state'), 'normal')
        panel.play_button.invoke()
        panel.play_button.invoke()                                        # one click, one command
        self.assertEqual(self.ctl.start.calls.count(('play',)), 1)
        self.deliver(PlayDone(PlayResult(False, 'running')))
        self.assertEqual(panel.status.cget('text'), text('gui.settings.play_failed.running'))
        self.deliver(PlayDone(PlayResult(True)))
        self.assertEqual(panel.status.cget('text'), text('gui.settings.played'))
        self.deliver(ProcessError('play', "OSError('x')"))
        self.assertEqual(panel.status.cget('text'), text('gui.settings.error', error="OSError('x')"))

    def test_a_held_interpolation_is_said(self):
        panel = self.opened()
        self.deliver(PlayDone(PlayResult(True, held=('interpolation',))))
        self.assertEqual(panel.status.cget('text'),
                         text('gui.settings.played') + '\n' + text('gui.settings.held.interpolation'))

    def test_opened_while_a_job_runs_reads_again_after_it(self):
        self.start()
        self.loaded('totk')
        self.view.pump()
        self.ctl.start.pending = True                                     # a Play is running
        self.view.tabs.select('settings')
        self.assertNotIn(('state',), self.ctl.start.calls)
        self.ctl.start.pending = False
        self.deliver(PlayDone(PlayResult(True)))
        self.assertEqual(self.ctl.start.calls[-1], ('state',))

    def test_the_game_and_version_on_top_and_one_play_note(self):
        panel = self.opened()
        self.deliver(StartStateReady(state()))
        self.assertEqual(panel.title.cget('text'), 'Tears of the Kingdom · 1.4.3')
        self.assertEqual(panel.path.cget('text'), str(APP))
        panel.copy_button.invoke()
        self.assertEqual(self.copied, [str(APP)])
        found = self.labels(panel.frame)
        self.assertEqual(found.count(text('gui.settings.applies')), 1)
        self.assertNotIn('only when started with Play', ' '.join(found))
        # the settings panel's own choices say so, the one it lacks says Play passes it (after PR #1184)
        files = sum(o.how == 'file' for o in OPTIONS['v143'])
        self.assertEqual((found.count(text('gui.settings.also_finder')), found.count(text('gui.settings.play_only'))),
                         (files, len(OPTIONS['v143']) - files))

    def test_play_is_the_primary_button_and_says_why_it_cannot(self):
        panel = self.opened()
        self.deliver(StartStateReady(state('v121', 'unknown_version')))
        self.assertEqual((panel.play_button.cget('state'), panel.reason.cget('text')),
                         ('disabled', text('gui.settings.why_not.app')))
        self.deliver(StartStateReady(state()))
        self.assertEqual((panel.play_button.cget('state'), panel.reason.cget('text')), ('normal', ''))
        self.assertEqual(panel.play_button.cget('text'), text('gui.settings.play'))
        self.ctl.busy = True
        self.view.refresh()
        self.assertEqual(panel.reason.cget('text'), text('gui.settings.why_not.build'))

    def test_play_waits_while_a_build_runs(self):
        panel = self.opened()
        self.deliver(StartStateReady(state()))
        self.ctl.busy = True
        self.view.refresh()                                               # the fake ends a build on its next tick
        self.assertEqual(panel.play_button.cget('state'), 'disabled')

    def test_notes_show_and_the_running_note_has_its_text(self):
        panel = self.opened()
        s = state()
        self.deliver(StartStateReady(StartState(s.game, s.app, s.version, '', s.options, s.choices, ('running',))))
        self.assertEqual((panel.notes.winfo_manager(), panel.notes.cget('text')),
                         ('pack', text('gui.settings.note.running')))
        self.deliver(StartStateReady(state()))
        self.assertEqual(panel.notes.winfo_manager(), '')


if __name__ == '__main__':
    unittest.main()
