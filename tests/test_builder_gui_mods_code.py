"""tools/builder/gui_mods.py, S5.4: the code toggle, the Build tab's code line and its quick read per game and app, a
refused Add in words, the .zip picker, and the done panel's line for a code patch made for another build of main.
Same harness as test_builder_gui_mods.py (a real withdrawn Tk root, the scripted FakeController)."""
import unittest

from test_builder_gui_mods import GOOD, ModsBase, mods_ready, state
from tools.builder.mod_library import Mod
from tools.builder.mod_tools import Validation
from tools.builder.session import Finished, Outcome
from tools.builder.strings import text

CODE_ONLY = Validation(False, 'no_romfs', code='ok')
REFUSALS = ('other_game', 'several', 'too_big', 'cheats_only', 'nothing', 'links', 'archive_kind', 'zip_entries',
            'zip_path', 'zip_link', 'zip_case', 'zip_space', 'zip_big', 'zip_bad', 'tkmm_package', 'cancelled')


class CodeMods(ModsBase):
    def test_the_quick_read_runs_once_per_game_and_app(self):
        self.ready()
        self.view.pump()
        self.assertEqual(self.ctl.mods.calls, [])                       # the same game and app: not again
        self.ctl.set_paths(game='/other.xci')
        self.view.pump()
        self.assertEqual(self.ctl.mods.calls[-1], ('state', 'quick'))

    def test_the_build_tab_line_follows_the_update_file(self):
        self.ready()
        self.deliver(mods_ready(state(code_next=('Stamina',), checked=False)))
        line = self.view.checks.code
        self.assertEqual((line.cget('text'), line.winfo_manager()),
                         (text('gui.mods.code_line', names='Stamina'), 'pack'))
        self.ctl.set_paths(update=None)
        self.view.pump()
        self.assertEqual(line.cget('text'), text('gui.mods.code_line_no_update', names='Stamina'))
        self.deliver(mods_ready(state(checked=False)))
        self.assertEqual(line.winfo_manager(), '')                      # nothing switched on: no line

    def test_a_line_for_another_game_is_not_shown(self):
        self.ready()
        self.deliver(mods_ready(state(code_next=('Stamina',), checked=False)))
        self.radio('botw').invoke()
        self.view.pump()
        self.assertEqual(self.view.checks.code.winfo_manager(), '')

    def test_a_code_only_mod_has_the_code_toggle_alone(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('s', 'Stamina', False, CODE_ONLY))))
        self.assertEqual((panel.toggles['s'][0].cget('state'), panel.toggles['s:code'][0].cget('state')),
                         ('disabled', 'normal'))
        panel.toggles['s:code'][0].invoke()
        self.assertEqual(self.ctl.mods.calls[-1], ('code', 's', True))
        self.assertIn(text('gui.mods.code.ok'), self.labels(panel.rows))

    def test_code_that_cannot_be_merged_has_no_toggle(self):
        panel = self.opened()
        for code in ('other_build', 'whole_module', 'none'):
            self.deliver(mods_ready(state(Mod('a', 'A', False, Validation(True, '', 1, 0, code=code)))))
            self.assertNotIn('a:code', panel.toggles)
        self.deliver(mods_ready(state(Mod('a', 'A', False, Validation(True, '', 1, 0, code='whole_module')))))
        self.assertIn(text('gui.mods.code.whole_module'), self.labels(panel.rows))

    def test_a_toggle_while_busy_is_put_back(self):
        panel = self.opened()
        self.deliver(mods_ready(state(Mod('s', 'Stamina', False, CODE_ONLY))))
        self.ctl.mods.pending = True
        switch, var = panel.toggles['s:code']
        switch.invoke()
        self.assertFalse(var.get())
        self.assertNotIn(('code', 's', True), self.ctl.mods.calls)

    def test_notes_and_the_build_again_line(self):
        panel = self.opened()
        ok = Validation(True, '', 1, 0, code='ok')
        self.deliver(mods_ready(state(Mod('a', 'A', False, ok, code=True, notes=('cheats_ignored',)),
                                      code_pending=True)))
        self.assertIn(text('gui.mods.notes.cheats_ignored'), self.labels(panel.rows))
        self.assertEqual(panel.pending.winfo_manager(), 'pack')
        self.assertTrue(panel.toggles['a:code'][1].get())                # the toggle shows the library's choice
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD))))
        self.assertEqual(panel.pending.winfo_manager(), '')

    def test_a_refused_add_is_said_in_words_and_a_good_one_too(self):
        panel = self.opened()
        self.deliver(mods_ready(state()))
        self.answer = '/mods/Pack'
        panel.add_button.invoke()
        self.deliver(mods_ready(state(refused=('several', 'A, B'), checked=False)))
        self.assertEqual(panel.status.cget('text'), text('gui.mods.refused.several', detail='A, B'))
        panel.zip_button.invoke()
        self.assertEqual((self.picks[-1], self.ctl.mods.calls[-1]), (('archive', None), ('add', '/mods/Pack')))
        self.deliver(mods_ready(state(Mod('a', 'A', False, GOOD), checked=False)))
        self.assertEqual(panel.status.cget('text'), text('gui.mods.added'))

    def test_a_pack_says_how_many_and_switched_off_mods_are_named(self):        # S5.5 C3, C1b
        panel = self.opened()
        self.deliver(mods_ready(state()))
        self.assertEqual(panel.zip_button.cget('text'), text('gui.mods.add_archive'))
        self.answer = '/mods/unbreakable_v141.7z'
        panel.zip_button.invoke()
        self.deliver(mods_ready(state(added=('unbreakable_v141', 4), checked=False)))
        self.assertEqual(panel.status.cget('text'), text('gui.mods.added_pack', n=4, name='unbreakable_v141'))
        self.deliver(mods_ready(state(switched_off=(('Ambient', ('1.1.0', '1.2.1')), ('Old', ())))))
        self.assertEqual((panel.switched.winfo_manager(), panel.switched.cget('text')),
                         ('pack', text('gui.mods.switched_off', mods='Ambient (1.1.0-1.2.1), Old')))
        self.deliver(mods_ready(state()))
        self.assertEqual(panel.switched.winfo_manager(), '')

    def test_a_mod_for_another_version_and_a_universal_one(self):                    # S5.5 C1
        panel = self.opened()
        old = Validation(False, 'other_version', 1, 6, versions=('1.1.0', '1.1.1', '1.2.1'))
        both = Validation(True, '', 1, 1, other_ignored=True)
        self.deliver(mods_ready(state(Mod('a', 'Ambient', False, old), Mod('u', 'Universal', False, both))))
        found = self.labels(panel.rows)
        self.assertIn(text('gui.mods.invalid.other_version', versions='1.1.0-1.2.1'), found)
        self.assertIn(text('gui.mods.other_ignored'), found)
        self.assertEqual(panel.toggles['a'][0].cget('state'), 'disabled')

    def test_every_refusal_has_its_text(self):
        for reason in REFUSALS:
            self.assertNotEqual(text(f'gui.mods.refused.{reason}', detail='x'), f'gui.mods.refused.{reason}')

    def test_a_patch_for_another_build_of_main_is_explained(self):          # R1
        self.ready()
        error = 'ERROR: exefs/main.pchtxt:1: this patch is for another build of main (@nsobid-abc), ...'
        self.deliver(Finished(Outcome.FAILED, 1, None, error, '/logs/x.log'))
        self.assertIn(text('gui.mods.code_other_build'), self.labels(self.view.done.body))
        self.assertEqual(self.ctl.mods.calls, [])


if __name__ == '__main__':
    unittest.main()
