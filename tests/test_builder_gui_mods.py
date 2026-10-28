"""tools/builder/gui_mods.py (S5.2): the Mods tab on a real withdrawn Tk root (builder_tk_base.py) against the scripted
FakeController and its FakeMods; states are the model's own ModsState, Mod and Validation."""
from pathlib import Path
import unittest

from builder_tk_base import Base, tk
from tools.builder.controller_mods import ModsState
from tools.builder.mod_library import ApplyResult, Mod
from tools.builder.mod_tools import Validation
from tools.builder.session import Finished, ModsApplied, ModsStateReady, Outcome, ProcessError
from tools.builder.strings import text

APP = Path('/Games/Tears of the Kingdom.app')
GOOD = Validation(True, '', 3, 1)


def state(*mods, reason='', **kw):
    return ModsState('totk', 'Tears of the Kingdom', '1.4.3', APP, reason, tuple(mods), **kw)


class ModsBase(Base):
    def ready(self):
        """S5.4: choosing a game reads the library quickly once (the Build tab's code line); answered here."""
        self.start()
        self.loaded('totk')
        self.view.pump()
        self.assertEqual(self.ctl.mods.calls, [('state', 'quick')])
        self.deliver(mods_ready(state(checked=False)))
        self.ctl.mods.calls.clear()

    def opened(self):
        self.ready()
        self.view.tabs.select('mods')
        return self.view.mods

    def deliver(self, *events):
        self.ctl.events.extend(events)
        self.view.pump()


class ModsTab(ModsBase):

    def test_opening_asks_for_the_library(self):
        self.opened()
        self.assertEqual(self.ctl.mods.calls, [('state',)])
        self.assertEqual((self.view.mods.foot.winfo_manager(), self.view.settings.foot.winfo_manager()), ('pack', ''))

    def test_a_valid_mod_has_a_toggle_and_an_invalid_one_does_not(self):
        panel = self.opened()
        bad = Validation(False, 'only_adds')
        self.deliver(mods_ready(state(Mod('a', 'Better Grass', False, GOOD), Mod('b', 'Off', False, bad))))
        self.assertEqual({m: t.cget('state') for m, (t, _) in panel.toggles.items()}, {'a': 'normal', 'b': 'disabled'})
        found = self.labels(panel.rows)
        self.assertIn(text('gui.mods.valid', replaced=3, added=1), found)
        self.assertIn(text('gui.mods.invalid.only_adds', paths=''), found)
        toggle, var = panel.toggles['a']
        toggle.invoke()
        self.assertEqual(self.ctl.mods.calls[-1], ('enable', 'a', True))

    def test_notes_for_taken_code_and_conflicts(self):
        panel = self.opened()
        v = Validation(True, '', 1, 0, taken=('System/HeapSize/x.byml.zs',), code='other_build')
        self.deliver(mods_ready(state(Mod('a', 'A', True, v, conflicts=('Pack/A.pack.zs',)))))
        found = self.labels(panel.rows)
        self.assertIn(text('gui.mods.taken', paths='System/HeapSize/x.byml.zs'), found)
        self.assertIn(text('gui.mods.code.other_build'), found)
        self.assertIn(text('gui.mods.conflicts', n=1, paths='Pack/A.pack.zs'), found)

    def test_every_reason_and_invalid_kind_has_a_text(self):
        panel = self.opened()
        for reason in ('no_game_file', 'no_app', 'not_latest', 'not_supported'):
            self.deliver(mods_ready(state(reason=reason)))
            self.assertEqual(panel.title.cget('text'),
                             text(f'gui.mods.reason.{reason}', name='Tears of the Kingdom', version='1.4.3'))
            self.assertEqual(panel.apply_button.cget('state'), 'disabled')
        for kind in ('misspelt', 'only_adds', 'all_taken', 'no_romfs', 'no_app_romfs', 'unchecked'):
            self.deliver(mods_ready(state(Mod('a', 'A', False, Validation(False, kind)))))

    def test_add_remove_and_backups_ask_first(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD), player_backups=('v143.backup-1',))))
        self.answer = '/mods/New'
        panel.add_button.invoke()
        self.assertEqual(self.ctl.mods.calls[-1], ('add', '/mods/New'))
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD), player_backups=('v143.backup-1',))))
        panel.remove('a', 'A')
        self.assertNotIn(('remove', 'a'), self.ctl.mods.calls)            # the confirmation said no
        self.confirm_answer = True
        panel.remove('a', 'A')
        self.assertEqual(self.ctl.mods.calls[-1], ('remove', 'a'))
        self.assertEqual(len(self.asked), 2)

    def test_a_removal_is_checked_again_after_yes(self):               # S5.7: the window runs while it asks
        panel = self.opened()
        both = state(Mod('a', 'A', False, GOOD), Mod('b', 'B', False, GOOD), player_backups=('k1', 'k2'))
        self.deliver(mods_ready(both))
        self.ctl.mods.calls.clear()
        def gone(question):                                               # the library changed meanwhile
            self.deliver(mods_ready(state(Mod('b', 'B', False, GOOD), player_backups=('k2',))))
            return True
        panel.confirm = gone
        panel.remove('a', 'A')
        panel.remove_backups()
        self.assertEqual(self.ctl.mods.calls, [('remove_backups', ('k2',))])
        self.deliver(mods_ready(both))
        self.ctl.mods.calls.clear()
        def busy(question):                                               # another change started meanwhile
            self.ctl.mods.pending = True
            return True
        panel.confirm = busy
        panel.remove('b', 'B')
        self.assertEqual(self.ctl.mods.calls, [])

    def test_apply_its_reason_and_the_result(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('a', 'A', True, GOOD), changed=True)))
        self.assertIn(text('gui.mods.changed'), self.labels(panel.frame))
        self.assertEqual((panel.apply_button.cget('state'), panel.reason.cget('text')), ('normal', ''))
        panel.apply_button.invoke()
        panel.apply_button.invoke()                                       # one click, one command
        self.assertEqual(self.ctl.mods.calls.count(('apply',)), 1)
        self.deliver(ModsApplied(ApplyResult(True, '', ('lines',))))
        self.assertEqual(self.ctl.mods.calls[-1], ('state',))             # read again after Apply
        self.assertIn(text('gui.mods.applied.ok', removed=0), panel.status.cget('text'))
        self.deliver(ProcessError('mods', "OSError('x')"))
        self.assertEqual(panel.status.cget('text'), text('gui.settings.error', error="OSError('x')"))

    def test_after_a_successful_build_the_mods_are_checked_and_the_done_panel_told(self):     # B1
        self.ready()
        self.ctl.events.append(Finished(Outcome.SUCCESS, 0, '/Games/TOTK.app', None, '/logs/x.log'))
        self.view.pump()
        self.assertEqual(self.ctl.mods.calls, [('state',)])
        self.deliver(mods_ready(state(Mod('a', 'A', True, GOOD), changed=True)))
        self.assertIn(text('gui.mods.after_build'), self.labels(self.view.done.body))

    def test_a_failed_build_checks_nothing(self):
        self.ready()
        self.ctl.events.append(Finished(Outcome.FAILED, 1, None, 'boom', '/logs/x.log'))
        self.view.pump()
        self.assertEqual(self.ctl.mods.calls, [])

    def test_a_quick_read_keeps_the_last_warning(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('a', 'A', True, GOOD), changed=True)))
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD), changed=False, checked=False)))
        self.assertIn(text('gui.mods.changed'), self.labels(panel.frame))
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD), changed=False)))
        self.assertEqual(panel.changed.winfo_manager(), '')

    def test_apply_waits_while_a_build_runs(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('a', 'A', True, GOOD))))
        self.ctl.busy = True
        self.view.refresh()
        self.assertEqual((panel.apply_button.cget('state'), panel.reason.cget('text')),
                         ('disabled', text('gui.settings.why_not.build')))


def mods_ready(s):
    return ModsStateReady(s)


if __name__ == '__main__':
    unittest.main()
