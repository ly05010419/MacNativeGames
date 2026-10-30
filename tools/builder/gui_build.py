"""The window's build and done panels (S2.3, R2-R4). Views: no rule.

The effects (monotonic, opener, copy) are injected without defaults; only open_window builds the real ones, so no test
can open anything. S4.4b: each panel's buttons (`foot`) live in the form's fixed footer; the build log keeps its own
scrolling, 12 lines high."""
from collections import deque
import tkinter as tk
from tkinter import ttk

from tools.builder.gui_style import BAD, WRAP
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import (BuildStarted, CancelStuck, Heartbeat, LogLine, Outcome, ProcessError,
                                   SettingsNotSaved, StageChanged)
from tools.builder.strings import cancel_report, text

LOG_LINES = 500
LAST_LINES = 20


class BuildPanel:
    def __init__(self, parent, footer, controller, monotonic, on_log):
        self.controller, self.monotonic, self.on_log = controller, monotonic, on_log
        self.frame = tk.Frame(parent)
        self.foot = tk.Frame(footer)
        self.stage = tk.Label(self.frame, text='', anchor='w')
        self.stage.pack(anchor='w')
        self.elapsed = tk.Label(self.frame, text='', anchor='w')
        self.elapsed.pack(anchor='w')
        self.bar = ttk.Progressbar(self.frame, mode='indeterminate')
        self.bar.pack(fill='x', pady=4)
        self.log_path = tk.Label(self.frame, text='', anchor='w')
        self.log_path.pack(anchor='w')
        self.messages = tk.Label(self.frame, text='', anchor='w', justify='left', foreground=BAD, wraplength=WRAP)
        self.messages.pack(anchor='w')
        self.cancel_button = FlatButton(self.foot, text('gui.cancel'), self.cancel)
        self.cancel_button.pack(side='right')
        self.log_button = FlatButton(self.foot, text('gui.show_log'), self.toggle_log)
        self.log_button.pack(side='right', padx=(0, 10))
        self.log = tk.Text(self.frame, height=12, state='disabled')
        self.started_at, self.cancelled, self.lines = None, False, deque(maxlen=LAST_LINES)
        self.notes = []

    def start(self):
        """Shown on the Build click; the elapsed time counts from here."""
        self.started_at, self.cancelled = self.monotonic(), False
        self.lines.clear()
        self.notes = []
        for label in (self.stage, self.elapsed, self.log_path, self.messages):
            label.configure(text='')
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')
        self.cancel_button.configure(text=text('gui.cancel'), state='normal')
        self.bar.start(50)

    def stop(self):
        self.bar.stop()                                 # its timer would otherwise outlive the panel

    def cancel(self):
        self.cancelled = True
        self.controller.cancel()
        self.cancel_button.configure(text=text('gui.cancelling'), state='disabled')

    def refresh(self):
        """Each pump: the elapsed time, and Retry cancel only while the last signal was refused (S1.8a)."""
        if self.started_at is not None:
            seconds = int(self.monotonic() - self.started_at)
            self.elapsed.configure(text=text('gui.elapsed', minutes=seconds // 60, seconds=seconds % 60))
        if self.cancelled:
            retry = self.controller.cancel_failed
            self.cancel_button.configure(text=text('gui.retry_cancel' if retry else 'gui.cancelling'),
                                         state='normal' if retry else 'disabled')

    def toggle_log(self):
        if self.log.winfo_manager():
            self.log.pack_forget()
            self.log_button.configure(text=text('gui.show_log'))
        else:
            self.log.pack(fill='both', expand=True)
            self.log_button.configure(text=text('gui.hide_log'))
            self.on_log()                               # S4.4b N1: the view scrolls to it

    def on_event(self, event):
        if isinstance(event, BuildStarted):
            self.log_path.configure(text=text('gui.log_path', log=event.log_path))
        elif isinstance(event, StageChanged):
            self.stage.configure(text=text('gui.stage', index=event.index, total=event.total, name=event.name,
                                           text=event.text))
        elif isinstance(event, LogLine):
            self.lines.append(event.text)
            self.log.configure(state='normal')
            self.log.insert('end', event.text + '\n')
            extra = int(self.log.index('end-1c').split('.')[0]) - 1 - LOG_LINES
            if extra > 0:
                self.log.delete('1.0', f'{extra + 1}.0')
            self.log.configure(state='disabled')
        elif isinstance(event, ProcessError):
            self._note(text('gui.process_error', where=event.where, error=event.error))
        elif isinstance(event, CancelStuck):
            self._note(text('gui.cancel_stuck'))
        elif isinstance(event, SettingsNotSaved):
            self._note(text('gui.settings_not_saved', error=event.error))
        elif isinstance(event, Heartbeat):
            self.refresh()

    def _note(self, line):
        self.notes.append(line)
        self.messages.configure(text='\n'.join(self.notes))


class DonePanel:
    def __init__(self, parent, footer, controller, opener, copy, on_new):
        self.controller, self.opener, self.copy, self.on_new = controller, opener, copy, on_new
        self.frame = tk.Frame(parent)
        self.foot = tk.Frame(footer)
        self.body = tk.Frame(self.frame)
        self.body.pack(fill='x')
        self.new_button = FlatButton(self.foot, text('gui.new_build'), on_new, state='disabled')
        self.new_button.pack(side='right')

    def show(self, finished, last_lines, notes):
        """R4: the outcome, its app or error, the last lines on a failure, the notes, and the log path."""
        for child in self.body.winfo_children():
            child.destroy()
        f = finished
        self._line(cancel_report(f) if f.outcome is Outcome.CANCELLED else text(f'outcome.{f.outcome.value}'))
        if f.outcome is Outcome.SUCCESS and f.app:
            self._line(text('gui.app', app=f.app))
            row = self._row()
            FlatButton(row, text('gui.open_app'), lambda: self.opener(['open', f.app])).pack(side='left')
            FlatButton(row, text('gui.show_in_finder'),
                       lambda: self.opener(['open', '-R', f.app])).pack(side='left', padx=(8, 0))
        if f.error:
            self._line(text('gui.error', error=f.error))
        if f.outcome in (Outcome.FAILED, Outcome.INCOMPLETE) and last_lines:
            self._line(text('gui.last_lines', n=len(last_lines)))
            self._line('\n'.join(last_lines))
        for note in notes:
            self._line(note)
        if f.log_path:
            row = self._row()
            tk.Label(row, text=text('gui.log_path', log=f.log_path), anchor='w').pack(side='left')
            FlatButton(row, text('gui.copy'), lambda: self.copy(f.log_path)).pack(side='left', padx=(8, 0))

    def note(self, line):
        """A line that arrives after Finished (a failed save of the run's paths or last build)."""
        self._line(line)

    def set_idle(self, idle):
        self.new_button.configure(state='normal' if idle else 'disabled')

    def _row(self):
        row = tk.Frame(self.body)
        row.pack(fill='x', pady=2)
        return row

    def _line(self, line):
        tk.Label(self.body, text=line, anchor='w', justify='left', wraplength=WRAP).pack(anchor='w')
