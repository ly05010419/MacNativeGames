"""The base of the window tests (S2.2, moved here in S2.3): a real Tk root that is never shown (withdrawn), the
scripted FakeController, and fakes for every effect (picker, clipboard, opener, confirm, clock). Not a test module,
so discover loads it once. Without Tk, the tests that use it are skipped; a broken gui module still fails (c1).

S4.4b: an exception inside a Tk callback (a button's command, the pump) is only printed by Tk, so the base records
them and every test fails at cleanup if one was recorded that it did not expect (expected_errors). The fake controller
refuses a second update or picture change before its next tick, as the real one does, and the fake's
background('saved', ...) at construction is pending until the first tick: pump before pressing picture controls.
Cleanup clears what is pending and checks the window really closed."""
import unittest

from builder_fake_controller import FakeController

try:
    import tkinter as tk
except ImportError:
    tk = None
if tk is not None:
    from tools.builder.gui import GuiView

SAVED = {'totk': {'game': '/t.xci', 'keys': '/t.keys', 'update': '/u.nsp'},
         'botw': {'game': '/b.xci', 'keys': '/b.keys'}}


@unittest.skipIf(tk is None, 'this Python has no Tk')
class Base(unittest.TestCase):
    def start(self, home=None, **fake):
        self.root = tk.Tk()
        self.home = home
        self.root.withdraw()
        self.ctl = FakeController(**fake)
        self.ctl.saved = SAVED
        self.picks, self.answer, self.copied, self.opened, self.asked = [], None, [], [], []
        self.confirm_answer, self.now = False, 1000.0
        made, self.callback_errors, self.expected_errors = [], [], 0
        self.root.report_callback_exception = lambda *exc: self.callback_errors.append(exc[1])
        self.addCleanup(self.check_callback_errors)                            # runs last: sees close()'s errors
        self.addCleanup(self.teardown_window, self.root, self.ctl, made)       # before any assertion
        self.view = GuiView(self.root, self.ctl, self.choose, self.copied.append, self.opened.append, self.confirm,
                            lambda: self.now, home=self.home)
        made.append(self.view)
        return self.view

    def check_callback_errors(self):
        self.assertEqual(len(self.callback_errors), self.expected_errors, f'Tk callback errors: {self.callback_errors}')

    def teardown_window(self, root, ctl, made):
        ctl.busy = ctl.update_pending = ctl.background_pending = ctl.background_forced = ctl.start.pending = False
        ctl.mods.pending = ctl.controls.pending = False
        if made:
            made[0].close()
            self.assertTrue(made[0].closed, 'the window did not close at cleanup')
        elif root.winfo_exists():
            root.destroy()

    def choose(self, kind, current):
        self.picks.append((kind, current))
        return self.answer

    def confirm(self, question):
        self.asked.append(question)
        return self.confirm_answer

    def loaded(self, game=None):
        self.view.pump()                                        # the fake's first tick delivers the load batch
        if game:
            self.radio(game).invoke()

    def radio(self, game):
        return next(r for r in self.view.files.radios if r.cget('value') == game)

    def entry(self, field):
        return self.view.files.rows[field][0]

    def type(self, field, value):
        self.entry(field).delete(0, 'end')
        self.entry(field).insert(0, value)

    def commit(self, field, sequence='<<Commit>>'):
        self.entry(field).winfo_id()                            # the widget's window must exist for events
        self.entry(field).event_generate(sequence)

    def after_info(self):
        return self.root.tk.call('after', 'info')

    def pending(self):
        info = self.after_info()
        return len(info) if isinstance(info, tuple) else int(bool(info))

    def labels(self, frame):
        """Every Label text under frame, in order."""
        found = []
        for child in frame.winfo_children():
            if child.winfo_class() == 'Label':
                found.append(child.cget('text'))
            found += self.labels(child)
        return found
