"""The window's Settings tab (S5.1): the start options of the game chosen in the Build tab, for its built app, and
Play. A view: the controller reads, keeps and starts (controller.start); the options and their choices come with the
state it sends, so the tab holds no table of its own.

The panel: the game and version on top ("Tears of the Kingdom · 1.4.3") with the app's path shortened below it and a
Copy button; one note that the settings apply when the game is launched with Play; each setting as a title, a segmented
selector (the chosen one in the accent) and a short description, 24 px apart; 1.4.3's picture says on its own that it
also holds from Finder. Play is the primary button of the fixed footer, and when it cannot play the reason stands next
to it. Interpolation is hidden when the GPU has no Metal 4 MetalFX frame interpolation, and warned while that is unknown
or still being checked. One click, one command."""
from pathlib import Path
import tkinter as tk

from tools.builder.gui_style import BAD, GOOD, LINE, MUTED, WRAP, muted, section
from tools.builder.gui_widgets import FlatButton, Segmented
from tools.builder.session import GpuProbed, PlayDone, ProcessError, StartStateReady
from tools.builder.strings import text

PATH_CHARS = 64


def short_path(path):
    """The app's path for a secondary line: the home folder as ~, the middle cut when long."""
    shown = str(path).replace(str(Path.home()), '~', 1)
    return shown if len(shown) <= PATH_CHARS else shown[:22] + '…' + shown[-(PATH_CHARS - 23):]


class SettingsPanel:
    def __init__(self, parent, footer, controller, fonts, copy):
        self.controller, self.fonts, self.copy = controller, fonts, copy
        self.state, self.vars, self.selectors, self._stale = None, {}, {}, False
        self.frame = tk.Frame(parent)
        self.title = tk.Label(self.frame, text='', anchor='w', font=fonts['section'])
        self.title.pack(anchor='w', pady=(4, 0))
        where = tk.Frame(self.frame)
        where.pack(anchor='w', fill='x')
        self.path = muted(tk.Label(where, text='', anchor='w', justify='left', wraplength=WRAP - 90), fonts['small'])
        self.path.pack(side='left')
        self.copy_button = FlatButton(where, text('gui.copy'), self._copy_path, font=fonts['small'])
        self.note = muted(tk.Label(self.frame, text=text('gui.settings.applies'), anchor='w', justify='left',
                                   wraplength=WRAP))
        self.rows = tk.Frame(self.frame)
        self.rows.pack(fill='x')
        self.notes = muted(tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP), fonts['small'])
        self.foot = tk.Frame(footer)
        tk.Frame(self.foot, height=1, background=LINE).pack(fill='x', pady=(0, 14))
        self.play_button = FlatButton(self.foot, text('gui.settings.play'), self.play, kind='primary',
                                      font=fonts['action'], size=(150, 44), state='disabled')
        self.play_button.pack(side='right')
        self.reason = muted(tk.Label(self.foot, text='', anchor='e', justify='right'), fonts['small'])
        self.reason.pack(side='right', padx=(0, 12))
        self.status = muted(tk.Label(self.foot, text='', anchor='w', justify='left', wraplength=WRAP - 330))
        self.status.pack(side='left')

    def opened(self):
        """The tab was shown: ask for the chosen game's state, and probe the GPU once."""
        if self.controller.selection()[0] is None:
            self._clear()
            self.state = None
            self.title.configure(text=text('gui.settings.choose_game'))
            self.path.configure(text='')
            return
        self.controller.start.probe()
        self._stale = self.controller.start.busy        # a job runs (a Play): read again once it has answered
        if not self._stale:
            self.title.configure(text=text('gui.settings.reading'))
            self.controller.start.state()

    def on_event(self, event):
        if isinstance(event, StartStateReady):
            self.render(event.state)
        elif isinstance(event, GpuProbed) and self.state is not None:
            self.render(self.state)
        elif isinstance(event, PlayDone):
            r = event.result
            lines = [text('gui.settings.played') if r.ok else text(f'gui.settings.play_failed.{r.reason}',
                                                                   detail=r.detail)]
            lines += [text(f'gui.settings.held.{h}') for h in r.held]
            self.status.configure(text='\n'.join(lines), foreground=GOOD if r.ok and not r.held else BAD)
        elif isinstance(event, ProcessError):
            self.status.configure(text=text('gui.settings.error', error=event.error), foreground=BAD)
        if self._stale and not self.controller.start.busy and self.controller.selection()[0] is not None:
            self._stale = False
            self.controller.start.state()

    def _clear(self):
        for child in self.rows.winfo_children():
            child.destroy()
        self.vars, self.selectors = {}, {}
        self.note.pack_forget()
        self.copy_button.pack_forget()

    def _copy_path(self):
        if self.state is not None and self.state.app is not None:
            self.copy(str(self.state.app))

    def render(self, state):
        self.state = state
        self._clear()
        if state.reason:
            self.title.configure(text=text(f'gui.settings.reason.{state.reason}', app=state.app))
        else:
            self.title.configure(text=text('gui.settings.title', name=state.name, version=state.version_label))
            self.note.pack(anchor='w', pady=(10, 0), before=self.rows)
        self.path.configure(text=short_path(state.app) if state.app else '')
        if state.app:
            self.copy_button.pack(side='left', padx=(10, 0))
        for option in state.options:
            self._setting(option, state)
        notes = [text(f'gui.settings.note.{n}') if n == 'running' else n for n in state.notes]
        self.notes.configure(text='\n'.join(notes))
        if notes:
            self.notes.pack(anchor='w', pady=(14, 0))
        else:
            self.notes.pack_forget()

    def _setting(self, option, state):
        """Title, selector, short description; 24 px between settings."""
        box = tk.Frame(self.rows)
        box.pack(fill='x', pady=(24, 0))
        section(box, text(f'gui.settings.option.{option.id}'), self.fonts['section']).pack(anchor='w')
        gpu = self.controller.start.gpu
        if option.id == 'interpolation' and gpu == 'no':
            self._line(box, text('gui.settings.gpu.no'), BAD)
            return
        var = self.vars[option.id] = tk.StringVar(master=self.frame, value=state.choices.get(option.id, 'auto'))
        selector = Segmented(box, [(c, text(f'gui.settings.choice.{c}')) for c in option.choices], var,
                             lambda c, o=option.id: self.choose(o, c), font=self.fonts['text'])
        selector.frame.pack(anchor='w', pady=(8, 0))
        self.selectors[option.id] = selector
        self._line(box, text(f'gui.settings.desc.{option.id}'), MUTED)
        if option.how == 'file':
            self._line(box, text('gui.settings.also_finder'), MUTED)
        else:
            self._line(box, text('gui.settings.play_only'), MUTED)
        if option.id == 'interpolation' and gpu in ('checking', 'unknown'):
            self._line(box, text(f'gui.settings.gpu.{gpu}'), MUTED if gpu == 'checking' else BAD)

    def _line(self, parent, line, colour):
        tk.Label(parent, text=line, anchor='w', justify='left', foreground=colour, wraplength=WRAP,
                 font=self.fonts['small']).pack(anchor='w', pady=(6, 0))

    def choose(self, option, choice):
        """A segment was picked: it shows as chosen and goes to the controller, unless a change is still running."""
        if self.controller.start.busy or self.state is None or choice == self.vars[option].get():
            return
        self.vars[option].set(choice)
        for selector in self.selectors.values():
            selector.configure(state='disabled')
        self.controller.start.choose(option, choice)

    def play(self):
        if not self._can_play():
            return
        self.play_button.configure(state='disabled')
        self.status.configure(text=text('gui.settings.playing'), foreground=MUTED)
        self.controller.start.play()

    def _can_play(self):
        return (self.state is not None and not self.state.reason and self.controller.idle
                and not self.controller.start.busy)

    def refresh(self, idle):
        can = idle and self._can_play()
        self.play_button.configure(state='normal' if can else 'disabled')
        if can or self.controller.start.busy:
            self.reason.configure(text='')
        else:
            self.reason.configure(text=text('gui.settings.why_not.build' if not idle else 'gui.settings.why_not.app'))
        for selector in self.selectors.values():
            selector.configure(state='disabled' if self.controller.start.busy else 'normal')
