"""The window's Details line (S4.4 layout): a toggle at the top that shows the repository folder and the background
picture's controls (gui_background.py), folded at start. The update check that shared this line was removed
(user 2026-10-30)."""
import tkinter as tk

from tools.builder.gui_style import WRAP, muted
from tools.builder.strings import text


class DetailsPanel:
    def __init__(self, parent, controller, fonts):
        self.frame = tk.Frame(parent)
        self.details_button = muted(tk.Label(self.frame, takefocus=1, cursor='hand2'), fonts['small'])
        self.details_button.pack(side='right')
        for sequence in ('<Button-1>', '<space>', '<Return>'):
            self.details_button.bind(sequence, lambda e: self.toggle_details())
        self.details = tk.Frame(parent)                 # shown under the line by toggle_details
        self.where = muted(tk.Label(self.details, text=text('gui.details.where', repo=controller.repo_root),
                                    anchor='w', justify='left', wraplength=WRAP), fonts['small'])
        self.where.pack(anchor='w')
        self._show_details(False)

    def toggle_details(self):
        self._show_details(not self.details.winfo_manager())

    def _show_details(self, shown):
        if shown:
            self.details.pack(after=self.frame, fill='x', pady=(6, 0))
        else:
            self.details.pack_forget()
        self.details_button.configure(text=text('gui.details') + (' ▾' if shown else ' ▸'))
