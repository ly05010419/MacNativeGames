"""S5.7: the builder's own yes/no dialog (gui_confirm) instead of macOS's alert, which closed the builder while a game
controller was connected. Each test answers from inside the dialog's wait (root.after), as a player would."""
from pathlib import Path
import re
import unittest
from unittest.mock import patch

try:
    import tkinter as tk
except ImportError:
    tk = None
if tk is not None:
    from tools.builder import gui_confirm
from tools.builder.strings import text

REPO = Path(__file__).resolve().parents[1]


@unittest.skipIf(tk is None, 'this Python has no Tk')
class Confirm(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.seen, self.errors = {}, []
        self.root.report_callback_exception = lambda *exc: self.errors.append(exc[1])
        self.addCleanup(lambda: self.assertEqual(self.errors, []))

    def dialog(self):
        [top] = [w for w in self.root.winfo_children() if isinstance(w, tk.Toplevel)]
        return top

    def buttons(self, top):
        found = {}

        def walk(widget):
            for child in widget.winfo_children():
                if hasattr(child, 'button'):
                    found[child.button.cget('text')] = child.button
                walk(child)
        walk(top)
        return found

    def ask(self, act):
        """confirm() with act(top) run while it waits; then no dialog and no grab may be left."""
        self.root.after(50, lambda: act(self.dialog()))
        answer = gui_confirm.confirm(self.root, 'Title', 'Remove it?')
        self.assertEqual([w for w in self.root.winfo_children() if isinstance(w, tk.Toplevel)], [])
        self.assertIsNone(self.root.grab_current())
        self.assertEqual(gui_confirm._asking, set())
        return answer

    def test_yes_is_true_and_the_question_is_shown(self):
        def act(top):
            self.seen['question'] = [w.cget('text') for w in top.winfo_children() if isinstance(w, tk.Label)]
            self.buttons(top)[text('gui.confirm.yes')].invoke()
        self.assertTrue(self.ask(act))
        self.assertEqual(self.seen['question'], ['Remove it?'])

    def test_return_on_the_first_focus_answers_no(self):
        def act(top):
            no = self.buttons(top)[text('gui.confirm.no')]
            self.seen['focused_no'] = top.focus_get() == no.frame
            no.frame.event_generate('<Return>')
        self.assertFalse(self.ask(act))
        self.assertTrue(self.seen['focused_no'])

    def test_escape_and_the_close_box_answer_no(self):
        self.assertFalse(self.ask(lambda top: top.event_generate('<Escape>')))
        self.assertFalse(self.ask(lambda top: top.tk.call(top.protocol('WM_DELETE_WINDOW'))))

    def test_a_second_question_while_one_is_open_is_no(self):
        def act(top):
            self.seen['second'] = gui_confirm.confirm(self.root, 'Title', 'Again?')
            self.buttons(top)[text('gui.confirm.yes')].invoke()
        self.assertTrue(self.ask(act))
        self.assertIs(self.seen['second'], False)

    def test_an_error_leaves_no_dialog_no_grab_and_asks_again(self):
        with patch.object(gui_confirm, 'FlatButton', side_effect=RuntimeError('drawing failed')):
            self.assertRaises(RuntimeError, gui_confirm.confirm, self.root, 'Title', 'Remove it?')
        self.assertEqual([w for w in self.root.winfo_children() if isinstance(w, tk.Toplevel)], [])
        self.assertIsNone(self.root.grab_current())
        self.assertFalse(self.ask(lambda top: top.event_generate('<Escape>')))


class NoNativeAlert(unittest.TestCase):
    def test_no_module_of_the_builder_uses_macos_alerts(self):
        imports = re.compile(r'^\s*(from tkinter import .*messagebox|import tkinter\.messagebox)', re.M)
        users = [p.name for p in (REPO / 'tools/builder').glob('*.py') if imports.search(p.read_text())]
        self.assertEqual(users, [])


if __name__ == '__main__':
    unittest.main()
