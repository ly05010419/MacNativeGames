"""The window's checks panel (S2.3, R1; S4.4 layout): the results, a summary of where the logs are, and a fixed footer
with the status, Check files and Build app. A view: no rule.

Check and Build commit the path rows first (aqua gives no focus out on a click). Build is enabled only by a ready
ChecksDone and disabled again by Check, ChecksInvalidated or a commit that changed a path; the controller decides
readiness, the panel only shows it. When enabled, Build is the primary (blue) button; disabled, it has the disabled look
like every other button (no aqua button in the window). S4.4b: the footer (`foot`) lives in the form's fixed footer; the
results are rows in the scrolling body, and Check asks the view to bring them into sight (on_check).

S2.3: under the results, the logs (the builder's own and the game's) and Export report, which writes the same report
the CLI's --check-only prints its log lines from (check_report.py, one data source for both)."""
import tkinter as tk
from pathlib import Path

from tools.builder import check_report
from tools.builder.games import GAMES
from tools.builder.gui_style import BAD, GOOD, LINE, MUTED, TEXT, WRAP
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import CheckResultReady, ChecksDone, ChecksInvalidated, PathsIncomplete, ProcessError
from tools.builder.strings import check_sentence, text

COLOURS = {'OK': GOOD, 'BLOCK': BAD}


class ChecksPanel:
    def __init__(self, parent, footer, controller, copy, commit_all, on_build, on_check, fonts,
                 home=None, choose=None, now=None):
        self.controller, self.copy, self.commit_all, self.on_build = controller, copy, commit_all, on_build
        self.fonts = fonts
        self.on_check, self.ready = on_check, False
        self.home, self.choose, self.now = (Path(home) if home is not None else None), choose, now
        self.first_problem = None           # the first result row that is not OK, for the view to show
        self.results = []                   # the CheckResults the shown checks produced, for the report
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
        self.logs_title = tk.Label(self.frame, text=text('gui.checks.logs'), anchor='w', foreground=MUTED,
                                   font=fonts['small'])
        self.logs_title.pack(anchor='w', pady=(14, 0))
        self.logs_box = tk.Frame(self.frame)
        self.logs_box.pack(fill='x')
        self.report_row = tk.Frame(self.frame)
        self.report_row.pack(anchor='w', pady=(8, 0))
        self.export_button = FlatButton(self.report_row, text('gui.checks.export'), self.export,
                                        min_size=(140, 30), font=fonts['small'])
        self.export_button.pack(side='left')
        self.report_status = tk.Label(self.report_row, text='', anchor='w', justify='left',
                                      wraplength=WRAP - 180, font=fonts['small'])
        self.report_status.pack(side='left', padx=(10, 0))
        self.show_logs()

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
        self.results = []
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
            self.show_logs()
        elif isinstance(event, ChecksInvalidated):
            self.clear()

    def _row(self, r):
        self.results.append(r)
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

    # --- where the logs are (one source: check_report.py, the CLI's too) ---
    def show_logs(self):
        for child in self.logs_box.winfo_children():
            child.destroy()
        if self.home is None:
            return
        folder = check_report.logs_dir(self.home)
        self._log_line(text('gui.checks.logs_dir', path=folder), MUTED)
        game = self.controller.selection()[0]
        if game is None:
            return
        files = check_report.log_files(folder, GAMES[game].id)
        for kind, key in (('builder', 'gui.checks.log_builder'), ('game', 'gui.checks.log_game')):
            found = [log for log in files if log.kind == kind]
            first = found[0] if found else None
            shown = self._log_text(first, key) if first and first.exists else \
                text('gui.checks.log_missing')
            self._log_line(shown, TEXT if first and first.exists else MUTED)

    def _log_text(self, log, key):
        size = f' ({log.size} bytes)' if log.size else ''
        return text(key, path=log.path) + size

    def _log_line(self, line, colour=MUTED):
        label = tk.Label(self.logs_box, text=line, anchor='w', justify='left', foreground=colour,
                         wraplength=WRAP, font=self.fonts['small'])
        label.pack(anchor='w', pady=1)
        return label

    # --- the report ---
    def export(self):
        if not self.results:
            self.report_status.configure(text=text('gui.checks.export_empty'), foreground=MUTED)
            return
        game = self.controller.selection()[0]
        if game is None or self.choose is None or self.now is None or self.home is None:
            self.report_status.configure(text=text('gui.checks.export_empty'), foreground=MUTED)
            return
        spec = GAMES[game]
        logs = check_report.logs_dir(self.home)
        target = self.choose('report', str(check_report.suggested_path(logs, spec.id, self.now())))
        if not target:
            return
        try:
            data = check_report.report_data(spec, self.results, logs, self.controller.selection()[1], self.now(),
                                            labels={r.id: text(f'check.{r.id}') for r in self.results})
            check_report.render(data, target)
        except OSError as error:
            self.report_status.configure(text=text('gui.checks.export_failed',
                                                   error=error.strerror or error), foreground=BAD)
            return
        self.report_status.configure(text=text('gui.checks.exported', path=target), foreground=GOOD)
