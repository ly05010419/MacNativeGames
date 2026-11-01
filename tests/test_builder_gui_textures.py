"""tools/builder/gui_textures.py (S9.2; E11 G3: a section of the Mods tab, no tab of its own) on a real withdrawn Tk
root against the FakeController, whose FakeMods answers for the library; controller.textures is recorded here. States
are the model's own ModsState and TexturesState."""
from pathlib import Path
import unittest

from test_builder_gui_mods import ModsBase, mods_ready, state
from tools.builder.controller_mods import ModsState
from tools.builder.mod_library import ApplyResult
from tools.builder.session import ModsApplied
from tools.builder.strings import text
from tools.builder.textures import TexturesState

PACK = TexturesState(True, 'HD Pack', True, True, '', 10414, 6_500_000_000, False, True, 64)


class FakeTextures:
    """controller.textures: refused while a Mods job runs, like the real one (it submits on the Mods part)."""

    def __init__(self, owner):
        self.owner, self.calls = owner, []

    def _call(self, *call):
        self.calls.append(call)
        if self.owner.mods.pending:
            raise RuntimeError('a mods change is still running')
        self.owner.mods.pending = True

    def add(self, source):
        self._call('add', source)

    def switch(self, on):
        self._call('switch', on)

    def remove(self):
        self._call('remove')


class TexturesSection(ModsBase):
    def opened(self):
        self.ready()
        self.ctl.textures = FakeTextures(self.ctl)
        self.view.tabs.select('mods')
        return self.view.mods.textures

    def show(self, textures=PACK, **kw):
        self.deliver(mods_ready(state(textures=textures, **kw)))
        self.ctl.mods.pending = False
        return self.labels(self.view.mods.textures.frame)

    def test_opening_mods_reads_once_and_shows_the_section_in_it(self):
        panel = self.opened()
        self.assertEqual(self.ctl.mods.calls, [('state',)])                 # one read serves both
        self.assertEqual(panel.frame.winfo_manager(), '')                   # nothing shown before the answer
        self.show()
        self.assertEqual((panel.frame.winfo_manager(), panel.frame.master), ('pack', self.view.mods.frame))
        self.assertEqual(self.view.mods.foot.winfo_manager(), 'pack')       # Mods' footer, its Apply
        self.assertNotIn('textures', self.view.tabs.tabs)

    def test_no_pack_then_an_add_said_in_words(self):
        panel = self.opened()
        found = self.show(TexturesState(memory_gb=64))
        self.assertIn(text('gui.textures.none'), found)
        self.assertIn(text('gui.textures.memory', gb=64), found)
        self.answer = '/Downloads/HDPacks.rar'
        self.button(panel.body, text('gui.textures.add_archive')).invoke()
        self.assertEqual((self.picks[-1][0], self.ctl.textures.calls), ('archive', [('add', '/Downloads/HDPacks.rar')]))
        self.assertEqual(panel.status.cget('text'), text('gui.textures.adding'))
        self.show(refused=('nothing', '/x'))
        self.assertEqual(panel.status.cget('text'), text('gui.mods.refused.nothing', detail='/x'))
        self.button(panel.body, text('gui.textures.add')).invoke()
        self.show()
        self.assertEqual(panel.status.cget('text'), text('gui.textures.added'))

    def test_what_is_left_to_do_is_said(self):
        panel = self.opened()
        found = self.show(PACK, not_applied=True)
        self.assertIn(text('gui.textures.files', files=10414, size='6.5'), found)
        self.assertIn(text('gui.textures.not_applied'), found)
        self.assertIn(text('gui.textures.ready'), found)
        found = self.show(TexturesState(True, 'HD Pack', True, True, '', 1, 1, True, False, 64))
        self.assertIn(text('gui.textures.build_on'), found)
        self.assertNotIn(text('gui.textures.not_applied'), found)
        found = self.show(TexturesState(True, 'HD Pack', False, True, '', 1, 1, True, True, 64))
        self.assertIn(text('gui.textures.build_off'), found)
        found = self.show(TexturesState(True, 'HD Pack', False, False, 'other_version', 1, 1, False, False, 16))
        self.assertIn(text('gui.textures.invalid', reason='other_version'), found)
        self.assertIn(text('gui.textures.memory_low', gb=16), found)
        self.assertEqual(panel.toggle[0].cget('state'), 'disabled')

    def test_the_switch_remove_and_the_tabs_apply(self):
        panel = self.opened()
        self.show()
        toggle, var = panel.toggle
        self.ctl.mods.pending = True
        toggle.invoke()
        self.assertEqual((var.get(), self.ctl.textures.calls), (True, []))     # busy: put back
        self.ctl.mods.pending = False
        toggle.invoke()
        self.assertEqual(self.ctl.textures.calls, [('switch', False)])
        self.show()
        remove = self.button(panel.body, text('gui.textures.remove'))
        remove.invoke()
        self.assertEqual((self.asked[-1], self.ctl.textures.calls[-1]),
                         (text('gui.textures.confirm_remove', name='HD Pack'), ('switch', False)))   # answered no
        self.confirm_answer = True
        remove.invoke()
        self.assertEqual(self.ctl.textures.calls[-1], ('remove',))
        self.show()
        self.view.pump()
        self.assertEqual(self.view.mods.apply_button.cget('state'), 'normal')
        self.view.mods.apply_button.invoke()
        self.assertEqual(self.ctl.mods.calls[-1], ('apply',))
        before = len(self.ctl.textures.calls), len(self.picks), len(self.asked)
        self.button(panel.body, text('gui.textures.add')).invoke()          # Apply runs: no change beside it
        self.button(panel.body, text('gui.textures.remove')).invoke()
        panel.toggle[0].invoke()
        self.assertEqual((len(self.ctl.textures.calls), len(self.picks), len(self.asked)), before)
        self.deliver(ModsApplied(ApplyResult(False, 'textures_failed', ('no such table',))))
        self.assertEqual(panel.status.cget('text'),
                         text('gui.mods.applied.textures_failed', removed=0) + '\nno such table')

    def test_the_section_is_hidden_where_no_pack_goes(self):
        panel = self.opened()
        self.show()
        self.deliver(mods_ready(ModsState('botw', 'Breath of the Wild', '1.9.0', Path('/b.app'))))
        self.assertEqual(panel.frame.winfo_manager(), '')
        self.show(reason='no_app')
        self.assertEqual(panel.frame.winfo_manager(), '')

    def button(self, frame, label):
        """The FlatButton under frame whose text is label (its frame carries it as .button)."""
        for child in frame.winfo_children():
            button = getattr(child, 'button', None)
            if button is not None and button.cget('text') == label:
                return button
            found = self.button(child, label)
            if found:
                return found
        return None

if __name__ == '__main__':
    unittest.main()
