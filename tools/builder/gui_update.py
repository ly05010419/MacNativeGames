"""The window's update line (S4.1; S4.4 layout): whether this repository has a newer version, the Update button, and a
Details box. A view: the controller fetches and fast-forwards; the line only shows what it found.

The line is short and discreet at the top; the repository folder, the waiting commits and the picture's controls are in
Details, folded at start. After an update the builder's own code may have changed, but the running window still has the
old code loaded, so Check and Build stay disabled until the builder is opened again; checking for updates (and updating
again) stays possible, and every later line keeps the reminder to reopen. S4.4b: one click, one command (the pressed
buttons are disabled at once, and Check for updates stays disabled until its answer); a git detail shows at most DETAIL
characters here. The buttons are dark secondary ones (the accent is kept for Play and for what is chosen); the line says
"Up to date." and the branch it follows is in Details."""
import tkinter as tk

from tools.builder.gui_style import WRAP, muted
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import ProcessError, UpdateApplied, UpdateStatusReady
from tools.builder.strings import text

SUBJECTS_SHOWN = 5
DETAIL = 160


class UpdatePanel:
    def __init__(self, parent, controller, fonts):
        self.controller = controller
        self.available, self.restart_needed, self.checking = False, False, False
        self.frame = tk.Frame(parent)
        self.details_button = muted(tk.Label(self.frame, takefocus=1, cursor='hand2'), fonts['small'])
        self.details_button.pack(side='right', padx=(10, 0))
        for sequence in ('<Button-1>', '<space>', '<Return>'):
            self.details_button.bind(sequence, lambda e: self.toggle_details())
        self.update_button = FlatButton(self.frame, text('gui.update.update'), self.update, font=fonts['small'],
                                        state='disabled')
        self.update_button.pack(side='right')
        self.check_button = FlatButton(self.frame, text('gui.update.check'), self.check, font=fonts['small'])
        self.check_button.pack(side='right', padx=(0, 8))
        self.status = muted(tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP - 300),
                            fonts['small'])
        self.status.pack(side='left', anchor='w')
        self.details = tk.Frame(parent)                 # shown under the line by toggle_details
        self.where = muted(tk.Label(self.details, text=text('gui.update.where', repo=controller.repo_root),
                                    anchor='w', justify='left', wraplength=WRAP), fonts['small'])
        self.where.pack(anchor='w')
        self.follows = muted(tk.Label(self.details, text='', anchor='w', justify='left', wraplength=WRAP),
                             fonts['small'])
        self.subjects = muted(tk.Label(self.details, text='', anchor='w', justify='left', wraplength=WRAP),
                              fonts['small'])
        self._show_details(False)

    def toggle_details(self):
        self._show_details(not self.details.winfo_manager())

    def _show_details(self, shown):
        if shown:
            self.details.pack(after=self.frame, fill='x', pady=(6, 0))
        else:
            self.details.pack_forget()
        self.details_button.configure(text=text('gui.details') + (' ▾' if shown else ' ▸'))

    def check(self):
        if self.checking:
            return
        self.available, self.checking = False, True
        self.check_button.configure(state='disabled')
        self.update_button.configure(state='disabled')
        self.status.configure(text=text('gui.update.checking'))
        self.controller.check_updates()

    def update(self):
        if not self.available or not self.controller.idle:
            return
        self.available = False
        self.check_button.configure(state='disabled')
        self.update_button.configure(state='disabled')
        self.status.configure(text=text('gui.update.updating'))
        self.controller.apply_update()

    def set_idle(self, idle):
        self.check_button.configure(state='normal' if idle and not self.checking else 'disabled')
        self.update_button.configure(state='normal' if idle and self.available and not self.checking else 'disabled')

    def _show(self, lines):
        self.status.configure(text='\n'.join(lines + ([text('gui.update.reopen')] if self.restart_needed else [])))

    def on_event(self, event):
        if isinstance(event, UpdateStatusReady):
            s, self.checking = event.status, False
            self.available = s.state.value == 'available'
            if s.upstream:
                self.follows.configure(text=text('gui.update.follows', branch=s.branch, upstream=s.upstream))
                self.follows.pack(anchor='w', after=self.where)
            self._show([text(f'gui.update.{s.state.value}', branch=s.branch, upstream=s.upstream, waiting=s.waiting,
                             detail=short(s.detail))])
            shown = list(s.subjects[:SUBJECTS_SHOWN]) + (['...'] if len(s.subjects) > SUBJECTS_SHOWN else [])
            self.subjects.configure(text='\n'.join(shown))
            if shown:                                   # E4: no empty line in Details
                self.subjects.pack(anchor='w')
            else:
                self.subjects.pack_forget()
        elif isinstance(event, UpdateApplied):
            r, self.available = event.result, False      # this update is spent: check again for the next one
            moved = bool(r.old and r.new and r.old != r.new)  # E1: HEAD moved even if git reported a failure
            self.restart_needed = self.restart_needed or r.ok or moved     # a later failure does not undo one
            self._show([text('gui.update.done', old=r.old, new=r.new) if r.ok
                        else text('gui.update.failed', detail=short(r.detail))])
        elif isinstance(event, ProcessError) and event.where == 'update':
            self.checking = False
            self._show([text('gui.update.failed', detail=short(event.error))])


def short(detail):
    return detail if len(detail or '') <= DETAIL else detail[:DETAIL - 3] + '...'
