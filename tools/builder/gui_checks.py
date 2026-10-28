"""The window's checks panel (S2.3, R1; S4.4 layout): the results, and a fixed footer with the status, Check files and
Build app. A view: no rule.

Check and Build commit the path rows first (aqua gives no focus out on a click). Build is enabled only by a ready
ChecksDone and disabled again by Check, ChecksInvalidated or a commit that changed a path; the controller decides
readiness, the panel only shows it. When enabled, Build is the primary (blue) button; disabled, it has the disabled look
like every other button (no aqua button in the window). S4.4b: the footer (`foot`) lives in the form's fixed footer; the
results are rows in the scrolling body, and Check asks the view to bring them into sight (on_check)."""
import tkinter as tk

from tools.builder.gui_style import BAD, GOOD, LINE, MUTED, TEXT, WRAP
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import CheckResultReady, ChecksDone, ChecksInvalidated, PathsIncomplete, ProcessError
from tools.builder.strings import check_sentence, text

COLOURS = {'OK': GOOD, 'BLOCK': BAD}


class ChecksPanel:
    def __init__(self, parent, footer, controller, copy, commit_all, on_build, on_check, fonts):
        self.controller, self.copy, self.commit_all, self.on_build = controller, copy, commit_all, on_build
        self.on_check, self.ready = on_check, False
        self.first_problem = None           # the first result row that is not OK, for the view to show
        self.frame = tk.Frame(parent)
        self.foot = foot = tk.Frame(footer)
        tk.Frame(foot, height=1, background=LINE).pack(fill='x', pady=(0, 14))
        self.build_button = FlatButton(foot, text('gui.build'), self.build, kind='primary', font=fonts['action'],
                                       size=(150, 44), state='disabled')
        self.build_button.pack(side='right')
        self.check_button = FlatButton(foot, text('gui.check'), self.check, state='disabled', min_size=(110, 36))
        self.check_button.pack(side='right', padx=(0, 10))
        self.status = tk.Label(foot, text=text('gui.choose_game'), foreground=MUTED, wraplength=WRAP - 300,
                               justify='left')
        self.status.pack(side='left')
        self.code = tk.Label(self.frame, text='', anchor='w', justify='left', foreground=MUTED, wraplength=WRAP)
        self.rows = tk.Frame(self.frame)
        self.rows.pack(fill='x', pady=(8, 0))

    def show_code(self, line):
        """S5.4: which code mods the next build includes (set by the Mods tab); hidden when none."""
        self.code.configure(text=line)
        if line:
            self.code.pack(anchor='w', pady=(6, 0), before=self.rows)
        else:
            self.code.pack_forget()

    def set_enabled(self, can_check, idle):
        self.check_button.configure(state='normal' if can_check and idle else 'disabled')
        ready = self.ready and idle
        self.build_button.configure(state='normal' if ready else 'disabled')

    def clear(self, status=''):
        self.ready, self.first_problem = False, None
        self.build_button.configure(state='disabled')
        for child in self.rows.winfo_children():
            child.destroy()
        self.status.configure(text=status, foreground=MUTED)

    def check(self):
        self.commit_all()
        self.clear()                                    # g1: run_checks() drops the ready checks without an event
        self.controller.run_checks()
        self.on_check()

    def build(self):
        if self.commit_all():                           # a changed path makes these checks stale
            self.clear(text('gui.checks_stale'))
            return
        self.on_build()

    def on_event(self, event):
        if isinstance(event, PathsIncomplete):
            fields = ', '.join(text(f'gui.field.{f}') for f in event.missing)
            self.status.configure(text=text('gui.paths_missing', fields=fields), foreground=BAD)
        elif isinstance(event, CheckResultReady):
            self._row(event.result)
        elif isinstance(event, ProcessError) and event.where == 'checks':
            self._line(text('gui.process_error', where=event.where, error=event.error), BAD)
        elif isinstance(event, ChecksDone):
            self.ready = event.ready
            self.status.configure(text=text('gui.checks_ready' if event.ready else 'gui.checks_blocked'),
                                  foreground=GOOD if event.ready else BAD)
        elif isinstance(event, ChecksInvalidated):
            self.clear()

    def _row(self, r):
        label = text(f'check.{r.id}')
        if r.status.name == 'OK':
            self._line(text('gui.check_ok', label=label), GOOD)
        else:
            sentence = check_sentence(r.message_id, r.detail)
            row = self._line(text('gui.check_line', status=r.status.name, label=label, sentence=sentence),
                             COLOURS.get(r.status.name, TEXT))
            self.first_problem = self.first_problem or row
        if r.fix:
            row = tk.Frame(self.rows)
            row.pack(fill='x', padx=(24, 0))
            tk.Label(row, text=text('gui.fix', fix=r.fix), anchor='w', wraplength=WRAP - 120,
                     justify='left').pack(side='left')
            FlatButton(row, text('gui.copy'), lambda: self.copy(r.fix)).pack(side='left', padx=(8, 0))

    def _line(self, line, colour=TEXT):
        label = tk.Label(self.rows, text=line, anchor='w', justify='left', foreground=colour, wraplength=WRAP)
        label.pack(anchor='w', pady=1)
        return label
