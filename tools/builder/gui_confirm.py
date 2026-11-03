"""The window's yes/no question, drawn by the builder itself (S5.7). A view's helper: no rule.

macOS's own alert (tkinter.messagebox, an NSAlert) closed the whole builder when a game controller was connected:
GameControllerUI's alert navigation trapped while Tk released the alert. confirm(root, title, question) asks in a
Toplevel instead: transient over the window, modal through a grab, the question and two FlatButtons, Yes and No. No has
the focus, so Return or Space answer No unless the player moves to Yes (Tab) or clicks it; Escape and the close box
answer No. However it ends, an error included, the grab is released and the dialog destroyed. One question at a time: a
question asked while another one is open is answered No at once, so a click or a timer that arrives while the window
waits can never confirm twice."""
import tkinter as tk

from tools.builder.gui_style import PANEL, TEXT
from tools.builder.gui_widgets import FlatButton
from tools.builder.strings import text

WRAP = 420
_asking = set()                     # the roots with a question open


def confirm(root, title, question):
    """True only when the player chose Yes."""
    if root in _asking:
        return False
    _asking.add(root)
    answer, top = [False], None
    try:
        top = tk.Toplevel(root, background=PANEL)
        top.withdraw()
        top.title(title)
        top.transient(root)
        top.resizable(False, False)

        def close(value):
            answer[0] = value
            top.destroy()
        tk.Label(top, text=question, justify='left', anchor='w', wraplength=WRAP, background=PANEL,
                 foreground=TEXT).pack(fill='x', padx=24, pady=(20, 16))
        row = tk.Frame(top, background=PANEL)
        row.pack(anchor='e', padx=24, pady=(0, 20))
        no = FlatButton(row, text('gui.confirm.no'), lambda: close(False))
        FlatButton(row, text('gui.confirm.yes'), lambda: close(True)).pack(side='left', padx=(0, 8))
        no.pack(side='left')
        top.protocol('WM_DELETE_WINDOW', lambda: close(False))
        top.bind('<Escape>', lambda e: close(False))
        _place(root, top)
        top.deiconify()
        top.update_idletasks()
        try:
            top.grab_set()
        except tk.TclError:             # not viewable yet (a withdrawn window in the tests): modal by transient only
            pass
        no.frame.focus_set()
        root.wait_window(top)
    finally:
        _asking.discard(root)
        try:
            if top is not None and top.winfo_exists():
                top.grab_release()
                top.destroy()
        except tk.TclError:             # the whole window went while the question was open
            pass
    return answer[0]


def _place(root, top):
    """Centred over the window when it is shown, else where the system puts it."""
    top.update_idletasks()
    if root.winfo_viewable():
        x = root.winfo_rootx() + (root.winfo_width() - top.winfo_reqwidth()) // 2
        y = root.winfo_rooty() + (root.winfo_height() - top.winfo_reqheight()) // 3
        top.geometry(f'+{max(x, 0)}+{max(y, 0)}')
