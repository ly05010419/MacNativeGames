"""Opening the window for real (S2.2, W1; moved out of gui.py in E14 S14.1 to keep it under 300 lines): the one place
the real effects (the native dialogs, the clipboard, opening an app, the confirmation) are built."""
import subprocess
import time
import tkinter as tk

from tools.builder.gui import GuiView
from tools.builder.gui_confirm import confirm
from tools.builder.gui_exit import exit_signals, settle, wire_quit
from tools.builder.gui_layout import dialog_chooser
from tools.builder.strings import text


def open_window(controller, game=None, paths=None):
    """W1: the window until it is closed; 0. The real effects are built here and only here."""
    root = tk.Tk()
    def copy(value):
        root.clipboard_clear()
        root.clipboard_append(value)
    view = GuiView(root, controller, dialog_chooser(root), copy, lambda argv: subprocess.run(argv, check=False),
                   lambda question: confirm(root, text('gui.title'), question), time.monotonic)
    view.preselect(game, paths)
    wire_quit(root, view)
    with exit_signals(view):
        try:
            root.mainloop()
        finally:
            settle(view, root.update, time.monotonic, time.sleep)
    return 0
