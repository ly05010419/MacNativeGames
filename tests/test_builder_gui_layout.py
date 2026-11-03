"""The window's layout (S4.4): two zones, the tabs, the groups and Advanced, the footer's default button, no empty
label, long paths showing their end. A real withdrawn Tk root (builder_tk_base.py) and the scripted FakeController."""
import unittest

from pathlib import Path
from types import SimpleNamespace

from builder_tk_base import Base, tk
from tools.builder.config import PATH_FIELDS
from tools.builder.session import ChecksDone
from tools.builder.strings import text

if tk is not None:
    from tools.builder.gui_files import GAME_SIZE, GROUPS
    from tools.builder.gui_style import ACCENT, BUTTON, BUTTON_OFF, FIELD, FORM_W


class Layout(Base):
    def test_the_chosen_game_is_painted(self):                                  # aqua hides it
        self.start()
        self.loaded('totk')
        colours = {r.cget('value'): r.cget('background') for r in self.view.files.radios}
        self.assertEqual(colours, {'totk': ACCENT, 'botw': FIELD})
        self.radio('botw').invoke()
        colours = {r.cget('value'): r.cget('background') for r in self.view.files.radios}
        self.assertEqual(colours, {'totk': FIELD, 'botw': ACCENT})

    def test_the_home_is_read_from_the_settings_path(self):
        self.start(store=SimpleNamespace(path='/Users/me/Library/Application Support/'
                                              'MacNativeGames/Builder/settings.json'))
        self.assertEqual(self.view.home, Path('/Users/me'))

    def test_the_home_falls_back_to_path_home_without_a_store(self):
        self.start()
        self.assertEqual(self.view.home, Path.home())

    def test_the_form_keeps_its_width_and_the_picture_zone_takes_the_rest(self):
        self.start()
        self.root.update_idletasks()
        for size in ('1200x800', '1600x900'):          # a withdrawn root lays out on a geometry change
            self.root.geometry(size)
            self.root.update_idletasks()
            form = self.view.form                       # S4.4b R7: the body is now the scroll area's inner frame
            self.assertEqual(form.winfo_width(), FORM_W)
            self.assertEqual(self.view.background.canvas.winfo_width(), self.root.winfo_width() - FORM_W)

    def test_the_groups_and_advanced_cover_every_field_once(self):
        self.start()
        grouped = [f for _, names in GROUPS for f in names]
        advanced = [f for f in PATH_FIELDS if f not in grouped]
        self.assertEqual(sorted(grouped + advanced), sorted(PATH_FIELDS))
        self.assertEqual(advanced, ['work', 'out'])

    def test_advanced_is_folded_at_start_and_opens(self):
        self.start()
        self.loaded('totk')
        self.assertEqual({self.entry(f).winfo_manager() for f in ('work', 'out')}, {''})
        self.assertEqual(self.entry('keys').winfo_manager(), 'grid')
        self.view.files.toggle_advanced()
        self.assertEqual({self.entry(f).winfo_manager() for f in ('work', 'out')}, {'grid'})
        self.type('out', '/x/My.app')                                       # still sent like any row
        self.commit('out')
        self.assertIn(('set_paths', {'out': '/x/My.app'}), self.ctl.calls)

    def test_the_tabs_switch_what_the_form_shows(self):
        self.start()
        self.assertEqual(self.view.tabs.current, 'build')
        self.view.tabs.select('mods')
        self.assertEqual((self.view.files.frame.winfo_manager(), self.view.mods.frame.winfo_manager()), ('', 'pack'))
        self.view.tabs.select('build')
        self.assertEqual((self.view.files.frame.winfo_manager(), self.view.checks.frame.winfo_manager(),
                          self.view.mods.frame.winfo_manager()), ('pack', 'pack', ''))

    def test_a_build_started_on_another_tab_shows_when_coming_back(self):
        self.start(checks=[[ChecksDone(True)]])
        self.loaded('totk')
        self.view.pump()                                                    # Check is enabled by the refresh
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.tabs.select('settings')
        self.view.checks.build_button.invoke()
        self.assertEqual(self.view.build.frame.winfo_manager(), '')       # not on this tab
        self.view.tabs.select('build')
        self.assertEqual(self.view.build.frame.winfo_manager(), 'pack')

    def test_build_is_the_blue_button_only_when_ready(self):
        self.start(checks=[[ChecksDone(True)]])
        self.loaded('totk')
        self.view.pump()                                                    # Check is enabled by the refresh
        build, check = self.view.checks.build_button, self.view.checks.check_button
        self.assertEqual((build.cget('state'), build.cget('background'), check.cget('background')),
                         ('disabled', BUTTON_OFF, BUTTON))
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.assertEqual((build.cget('state'), build.cget('background')), ('normal', ACCENT))
        self.assertEqual(self.view.checks.status.cget('text'), text('gui.checks_ready'))

    def test_the_game_choice_is_two_large_toggles(self):
        self.start()
        self.assertEqual([(int(r.cell.cget('width')), int(r.cell.cget('height'))) for r in self.view.files.radios],
                         [GAME_SIZE] * 2)

    def test_no_aqua_button_is_left_in_the_window(self):
        self.start()
        self.loaded('totk')
        found, todo = set(), [self.root]
        while todo:
            widget = todo.pop()
            found.add(widget.winfo_class())
            todo += widget.winfo_children()
        self.assertFalse(found & {'Button', 'Radiobutton'}, found)
        self.assertIn('FlatButton', found)

    def test_the_problems_line_is_only_there_with_something_to_say(self):
        self.start()
        self.assertEqual(self.view.problems.winfo_manager(), 'pack')        # "Reading the saved settings..."
        self.loaded()
        self.assertEqual((self.view.problems.cget('text'), self.view.problems.winfo_manager()), ('', ''))

    def test_a_long_path_shows_its_end(self):
        self.start()
        self.ctl.saved = {'totk': {'game': '/a/very/long/folder/name/' * 8 + 'Tears of the Kingdom.xci'}}
        self.loaded('totk')
        self.root.update_idletasks()
        self.assertEqual(float(self.entry('game').xview()[1]), 1.0)


if __name__ == '__main__':
    unittest.main()
