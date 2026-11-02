"""tools/builder/gui_widgets.py (S5.1 polish): the flat button's looks in each state, its sizes, and the segmented
selector's chosen look, on a real withdrawn Tk root. Aqua's own buttons ignore colours, which is why these exist; what
is tested here is the colours they take, not how aqua draws them (that needs a person's eyes)."""
import types
import unittest

try:
    import tkinter as tk
except ImportError:
    tk = None
if tk is not None:
    from tools.builder.gui_style import (ACCENT, ACCENT_DOWN, ACCENT_HOVER, CHOSEN_TEXT, FOCUS_RING, LINE, MUTED,
                                         TEXT, TOGGLE_OFF)
    from tools.builder.gui_widgets import TRACK, FlatButton, Segmented, Toggle

# The button looks, copied here so a change of gui_style shows as a failing test
NORMAL, HOVER, PRESSED, DISABLED = ('#2B313B', '#F1F3F5'), ('#394352', '#FFFFFF'), ('#232A34', '#FFFFFF'), \
    ('#252A32', '#929BA8')
PRIMARY = ('#3478F6', '#FFFFFF')


@unittest.skipIf(tk is None, 'this Python has no Tk')
class Widgets(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.calls = []

    def colours(self, button):
        self.assertEqual(button.frame.cget('background'), button.label.cget('background'))   # no frame of another fill
        return button.label.cget('background'), button.label.cget('foreground')

    def test_primary_button_states(self):
        play = FlatButton(self.root, 'Play', lambda: self.calls.append('play'), kind='primary', size=(150, 44))
        self.assertEqual(self.colours(play), PRIMARY)
        play._set(hover=True)
        self.assertEqual(self.colours(play), (ACCENT_HOVER, CHOSEN_TEXT))
        play._set(down=True)
        self.assertEqual(self.colours(play), (ACCENT_DOWN, CHOSEN_TEXT))
        play._set(hover=False, down=False)
        play.configure(state='disabled')
        self.assertEqual(self.colours(play), DISABLED)                  # never light text on a light fill
        play.invoke()
        self.assertEqual(self.calls, [])
        play.configure(state='normal')
        play.invoke()
        self.assertEqual((self.calls, play.cget('state')), (['play'], 'normal'))
        self.assertEqual((int(play.frame.cget('width')), int(play.frame.cget('height'))), (150, 44))

    def test_secondary_button_every_state(self):
        check = FlatButton(self.root, 'Check files', lambda: self.calls.append('check'))
        self.assertEqual(self.colours(check), NORMAL)
        check._set(hover=True)
        self.assertEqual(self.colours(check), HOVER)
        check._set(down=True)
        self.assertEqual(self.colours(check), PRESSED)
        check._set(hover=False, down=False)
        check.configure(state='disabled')
        self.assertEqual(self.colours(check), DISABLED)
        check._set(hover=True)
        self.assertEqual(self.colours(check), DISABLED)                 # hover does not light a disabled one
        check._set(hover=False)
        check.configure(state='normal', kind='primary')
        self.assertEqual(self.colours(check), PRIMARY)

    def test_sizes_and_the_keyboard(self):
        choose = FlatButton(self.root, 'Choose...', lambda: self.calls.append('choose'), min_size=(110, 36))
        self.assertEqual((int(choose.frame.cget('width')), int(choose.frame.cget('height'))), (110, 36))
        long = FlatButton(self.root, 'Show in Finder, then something longer', lambda: None)
        width = int(long.frame.cget('width'))
        self.assertGreaterEqual(width, long.label.winfo_reqwidth())     # it grows with its text: nothing cut
        long.configure(text='Hide')
        self.assertLess(int(long.frame.cget('width')), width)           # and fits a new text
        self.assertEqual(str(choose.frame.cget('takefocus')), '1')
        self.assertEqual(choose.frame.cget('highlightcolor'), FOCUS_RING)
        self.assertNotIn(FOCUS_RING, (ACCENT, NORMAL[0], HOVER[0]))     # the ring is neither chosen nor hover
        # a withdrawn root gets no generated key events on aqua (focus needs a shown window): the bindings are
        # checked, and invoke() is what they call
        self.assertTrue(choose.frame.bind('<space>') and choose.frame.bind('<Return>'))
        choose.invoke()
        self.assertEqual(self.calls, ['choose'])

    def test_segmented_shows_the_chosen_one_and_follows_the_variable(self):
        var = tk.StringVar(master=self.root, value='auto')
        picked = []
        seg = Segmented(self.root, [('auto', 'Automatic'), ('on', 'On')], var, picked.append)
        look = {s.value: (s.cget('background'), s.cget('foreground')) for s in seg.segments}
        self.assertEqual(look, {'auto': (ACCENT, CHOSEN_TEXT), 'on': NORMAL})
        seg.segments[1].invoke()
        self.assertEqual(picked, ['on'])                                 # the owner decides; nothing changed yet
        var.set('on')
        self.assertEqual({s.value: s.cget('background') for s in seg.segments}, {'auto': NORMAL[0], 'on': ACCENT})
        seg.configure(state='disabled')
        seg.segments[0].invoke()
        self.assertEqual(picked, ['on'])
        self.assertEqual((seg.segments[0].cget('foreground'), seg.segments[1].cget('background')),
                         (DISABLED[1], ACCENT))                         # the chosen one stays shown
        seg.configure(state='normal')
        seg.segments[0].invoke()
        self.assertEqual(picked, ['on', 'auto'])
        for segment in seg.segments:                                    # Tab reaches each; Space or Return picks
            self.assertEqual(str(segment.cell.cget('takefocus')), '1')
            self.assertTrue(segment.cell.bind('<space>') and segment.cell.bind('<Return>'))

    def test_a_toggle_flips_its_variable_calls_once_and_follows_the_variable(self):
        var = tk.BooleanVar(master=self.root, value=False)
        switch = Toggle(self.root, 'Better grass', var, lambda: self.calls.append(var.get()))

        def look():
            pill = switch.track.itemcget(switch.track.find_withtag('pill')[0], 'fill')
            x0 = switch.track.coords(switch.track.find_withtag('knob')[0])[0]
            return pill, 'right' if x0 > TRACK[0] / 2 else 'left', switch.label.cget('foreground')
        self.assertEqual((look(), switch.cget('text')), ((TOGGLE_OFF, 'left', TEXT), 'Better grass'))
        switch.invoke()                                                 # as a Checkbutton: flipped, then the command
        self.assertEqual((self.calls, look()), ([True], (ACCENT, 'right', TEXT)))
        var.set(False)                                                  # the owner put it back: drawn off
        self.assertEqual(look(), (TOGGLE_OFF, 'left', TEXT))
        switch.configure(state='disabled')
        switch.invoke()
        self.assertEqual((self.calls, var.get(), switch.cget('state')), ([True], False, 'disabled'))
        self.assertEqual(look(), (LINE, 'left', MUTED))
        self.assertNotIn(TOGGLE_OFF, (ACCENT, LINE, NORMAL[0]))
        self.assertEqual(str(switch.frame.cget('takefocus')), '1')     # Tab reaches it; Space or Return switch it
        self.assertTrue(switch.frame.bind('<space>') and switch.frame.bind('<Return>'))
        switch.configure(state='normal')
        switch.pack()
        self.root.update_idletasks()
        # a withdrawn root gets no generated mouse events on aqua (and its widgets are 1 px): the bound handler
        # is called as Tk would
        for widget in (switch.track, switch.label):                     # a click on the pill or the label
            self.assertTrue(widget.bind('<ButtonRelease-1>'))
            switch._release(types.SimpleNamespace(widget=widget, x=0, y=0))
        self.assertEqual(self.calls, [True, True, False])
        switch._release(types.SimpleNamespace(widget=switch.label, x=-5, y=0))     # released outside: nothing
        self.assertEqual(self.calls, [True, True, False])


if __name__ == '__main__':
    unittest.main()
