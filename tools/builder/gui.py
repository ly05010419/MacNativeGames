"""The window: its shell, the event pump, the panels and closing (S2.2, W1-W2; S2.3, R1-R6). A view: no rule.

The layout (S4.4): the form left, the picture right, the tabs Build | Settings | Mods | Controls (E6); S4.4b: a fixed
footer with the shown panel's buttons, above it one scrolling body that moves to the panel that changed. __main__
imports it after tkinter. The pump ticks the controller every 200 ms, hands each event to the panels and refreshes which
controls are enabled; it always reschedules itself. Quit signals (SIGINT, SIGHUP when the Terminal window closes,
SIGTERM) only set exit_requested (S3.4, B1), wherever they land; the pump then cancels a running build and closes once
it has stopped (no time limit)."""
import os
import subprocess
import sys
import time
import tkinter as tk
from pathlib import Path

from tools.builder.gui_background import BackgroundPanel
from tools.builder.gui_build import BuildPanel, DonePanel
from tools.builder.gui_checks import ChecksPanel
from tools.builder.gui_confirm import confirm
from tools.builder.gui_controls import ControlsPanel
from tools.builder.gui_exit import _tell, exit_signals, settle, wire_quit      # noqa: F401  re-exported
from tools.builder.gui_files import FilesPanel
from tools.builder.gui_mods import ModsPanel
from tools.builder.gui_saves import SavesPanel
from tools.builder.gui_settings import SettingsPanel
from tools.builder.gui_storage import StoragePanel
from tools.builder.gui_layout import IMAGE_TYPES, ScrollArea, TabBar, dialog_chooser, zones  # noqa: F401
from tools.builder.gui_style import PAD, apply, muted
from tools.builder.gui_update import UpdatePanel
from tools.builder.session import (BackgroundChanged, BuildStarted, CancelStuck, CheckResultReady, ChecksDone,
                                   ChecksInvalidated, Finished, GpuProbed, Heartbeat, LogLine, ModsApplied,
                                   ModsStateReady, PathsIncomplete, PathsSaved, PlayDone, ProcessError,
                                   SettingsLoaded, SettingsNotSaved, StageChanged, StartStateReady,
                                   UpdateApplied, UpdateStatusReady)
from tools.builder.strings import text

PUMP_MS = 200
LOAD_WAIT_S = 10.0             # then the window goes on without the saved settings (S3.4, f4)
CHECK_EVENTS = (PathsIncomplete, CheckResultReady, ChecksDone, ChecksInvalidated)
SETTINGS_EVENTS = (StartStateReady, PlayDone, GpuProbed)
MODS_EVENTS = (ModsStateReady, ModsApplied)
BUILD_EVENTS = (BuildStarted, StageChanged, Heartbeat, LogLine, CancelStuck, SettingsNotSaved, ProcessError)


def home_of(controller):
    """The home the settings file sits under (.../Library/Application Support/MacNativeGames/Builder/...), so a
    window opened with HOME changed still finds the same caches and saves; Path.home() when there is no store."""
    path = getattr(getattr(controller, 'store', None), 'path', None)
    parents = Path(path).parents if path is not None else ()
    if len(parents) > 4 and parents[2].name == 'Application Support' and parents[1].name == 'MacNativeGames':
        return parents[4]
    return Path.home()


class GuiView:
    def __init__(self, root, controller, choose, copy, opener, confirm, monotonic, home=None):
        self.root, self.controller, self.confirm, self.monotonic = root, controller, confirm, monotonic
        self.home = Path(home) if home is not None else home_of(controller)   # S2.3/S2.4: under home
        self._after, self._closed, self._preselect, self._loaded = None, False, None, False
        self._load_since, self._load_late = monotonic(), False
        self._close_pending, self._building = False, False
        self.exit_requested = False     # B1: set by a quit signal's handler, acted on by the pump
        self.fonts = apply(root)                                # S4.4: before any widget
        root.title(text('gui.title'))
        root.event_add('<<Commit>>', '<Return>', '<FocusOut>')
        form, picture_column = zones(root)
        self.form = form
        tk.Label(form, text=text('gui.title'), font=self.fonts['title']).pack(anchor='w', padx=PAD, pady=(22, 6))
        top = tk.Frame(form)
        top.pack(fill='x', padx=PAD)
        self.updates = UpdatePanel(top, controller, self.fonts)  # S4.1: discreet, at the top
        self.updates.frame.pack(fill='x')
        self.tabs = TabBar(form, self.fonts['section'], self._tab)
        self.tabs.frame.pack(fill='x', padx=PAD, pady=(14, 0))
        self.footer = tk.Frame(form)                            # S4.4b, B1: packed before the body, so it always fits
        self.footer.pack(side='bottom', fill='x', padx=PAD, pady=(8, 20))
        self.area = ScrollArea(form)
        self.area.frame.pack(fill='both', expand=True, padx=(PAD, 8), pady=(10, 0))
        self.body, self._reveal, self._await_result = self.area.inner, None, False
        self.problems = muted(tk.Label(self.body, text=text('gui.loading'), anchor='w', justify='left'))
        self.settings = SettingsPanel(self.body, self.footer, controller, self.fonts, copy)  # S5.1
        self.mods = ModsPanel(self.body, self.footer, controller, self.fonts, choose, confirm,
                              lambda line: self.done.note(line), lambda line: self.checks.show_code(line))  # S5.4
        self.controls = ControlsPanel(self.body, self.footer, controller, self.fonts, confirm,
                                      home=self.home)                                  # E6 S6.2
        self.storage = StoragePanel(self.body, self.footer, controller, self.fonts, choose, confirm,
                                    self.home, os.environ, time.time)                    # S2.3
        self.saves = SavesPanel(self.body, self.footer, controller, self.fonts, choose, confirm,
                                self.home, os.environ, time.time)                          # S2.4
        self.files = FilesPanel(self.body, controller, choose, self._paths_changed, self.fonts, self.area.sync)
        self.checks = ChecksPanel(self.body, self.footer, controller, copy, self.files.commit_all, self.start_build,
                                  self._checking, self.fonts, home=self.home, choose=choose, now=time.time)
        self.build = BuildPanel(self.body, self.footer, controller, monotonic, self._show_log)
        self.done = DonePanel(self.body, self.footer, controller, opener, copy, self.new_build)
        self.background = BackgroundPanel(root, controller, choose, self.updates.details, self.fonts)   # S4.3
        self.background.frame.pack(fill='x')
        self.background.canvas.grid(row=0, column=picture_column, sticky='nsew')
        self._panels = (self.files, self.checks)
        self.tabs.select('build')
        self._show_problems()
        root.protocol('WM_DELETE_WINDOW', self.close)
        controller.load_settings()
        self.updates.check()                                    # the window looks for updates on its own
        self._schedule()

    @property
    def closed(self):
        return self._closed

    def _show(self, *panels):
        """The Build tab's panels; placed now when that tab is the one shown."""
        self._panels = panels
        if self.tabs.current == 'build':
            self._tab('build')

    def _tab(self, name):
        panels = (self.files, self.checks, self.build, self.done, self.settings, self.mods, self.controls,
                  self.storage, self.saves)
        for frame in [p.frame for p in panels] + [p.foot for p in panels[1:]]:
            frame.pack_forget()
        if name != 'build':                             # another tab: its panel, its footer, fresh state
            tab = {'settings': self.settings, 'mods': self.mods, 'controls': self.controls,
                   'storage': self.storage, 'saves': self.saves}[name]
            tab.frame.pack(fill='x')
            tab.foot.pack(fill='x')
            tab.opened()
        else:
            for panel in self._panels:
                panel.frame.pack(fill='x', pady=4)
            self._panels[-1].foot.pack(fill='x')                # the shown panel's buttons, in the fixed footer
        self.area.sync()

    def _reveal_later(self, widget):
        """S4.4b, R3: after the next events, scroll the body to the panel (or row) that changed."""
        self._reveal = widget

    def _checking(self):
        """Check pressed: show the panel now, and again when its first answer arrives (N2: it may take a tick)."""
        self._reveal_later(self.checks.frame)
        self._await_result = True

    def _show_log(self):
        """N1: Show log brings the log into sight."""
        self.area.reveal(self.build.log)

    def _show_problems(self):
        """The problems line, at the top of the body, only while it says something (S4.4: no empty label)."""
        if self.problems.cget('text') and not self.problems.winfo_manager():
            slaves = [w for w in self.body.pack_slaves() if w is not self.problems]
            self.problems.pack(anchor='w', pady=(0, 6), **({'before': slaves[0]} if slaves else {}))
        elif not self.problems.cget('text') and self.problems.winfo_manager():
            self.problems.pack_forget()

    def preselect(self, game, paths):
        """W2: kept until the saved settings are in, so select_game fills from them (g2)."""
        self._preselect = (game, dict(paths or {})) if game else None

    def _paths_changed(self):
        """Closing audit 3: a sent path or game change makes the shown checks stale at once, before the pump
        delivers the controller's ChecksInvalidated."""
        self.checks.clear()

    def start_build(self):
        """R2: the checks panel's Build, after its commit found nothing changed."""
        self.controller.start_build()
        self._building = True
        self.build.start()
        self._show(self.files, self.build)
        self._reveal_later(self.build.frame)
        self.refresh()

    def new_build(self):
        """R4: back to the files and checks, once idle (the button is disabled before)."""
        self.checks.clear()
        self._show(self.files, self.checks)
        self.refresh()

    def _schedule(self):
        if self._after is not None:
            self.root.after_cancel(self._after)
        self._after = self.root.after(PUMP_MS, self.pump)

    def pump(self):
        if self._closed:                                # a direct call after close: the widgets are gone
            return
        try:
            self.controller.tick()
            events = self.controller.take_events()
            for event in events:
                self.on_event(event)
            if events or self._reveal is not None:              # C2: rows came or went
                self.area.sync()
            if self._reveal is not None and self.tabs.current == 'build':
                self.area.see(self._reveal)
                self._reveal = None
            if not self._loaded and self.monotonic() - self._load_since >= LOAD_WAIT_S:
                self._load_late = True                  # f4: go on without them; a late answer changes no choice
                self.problems.configure(text=text('gui.load_slow', seconds=int(LOAD_WAIT_S)))
                self._settings_in()
            self.refresh()
            if self.exit_requested:
                self._exit()
            if self._close_pending and self.controller.idle:
                self.close()                            # R6: the build asked to stop has stopped
        except Exception:       # g1: Tk's own report, and the window stays alive; the view's one broad except
            self.root.report_callback_exception(*sys.exc_info())
        finally:
            if not self._closed:
                self._schedule()                # cancels a pending pump first: at most one is ever pending

    def on_event(self, event):
        where = event.where if isinstance(event, ProcessError) else None
        for panel, kinds, wheres in ((self.updates, (UpdateStatusReady, UpdateApplied), ('update',)),
                                     (self.background, (BackgroundChanged,), ('background',)),
                                     (self.settings, SETTINGS_EVENTS, ('settings', 'play')),
                                     (self.mods, MODS_EVENTS, ('mods',)),
                                     (self.controls, self.controls.EVENTS, ('controls',)),
                                     (self.storage, self.storage.EVENTS, ()),
                                     (self.saves, self.saves.EVENTS, ())):
            if isinstance(event, kinds) or where in wheres:
                return panel.on_event(event)
        if isinstance(event, SettingsLoaded):
            lines = [text('gui.settings_problem', problem=p) for p in event.problems]
            self.problems.configure(text='\n'.join([text('gui.load_late')] * self._load_late + lines))
            self._settings_in()
        elif isinstance(event, ProcessError) and event.where == 'load':
            self.problems.configure(text=text('gui.load_error', error=event.error))
            self._settings_in()
        elif isinstance(event, CHECK_EVENTS) or (isinstance(event, ProcessError) and event.where == 'checks'):
            self.checks.on_event(event)
            if self._await_result and not isinstance(event, ChecksInvalidated):
                self._await_result = False              # N2: the first answer after Check
                self._reveal_later(self.checks.frame)
            if isinstance(event, ChecksDone) and self.checks.first_problem is not None:
                self._reveal_later(self.checks.first_problem)       # the first line that is not OK
        elif isinstance(event, (SettingsNotSaved, ProcessError)) and not self._building:
            self.done.note(text('gui.settings_not_saved', error=event.error) if isinstance(event, SettingsNotSaved)
                           else text('gui.process_error', where=event.where, error=event.error))   # closing audit 1
        elif isinstance(event, Finished):
            self._building = False
            self.build.stop()
            self.done.show(event, list(self.build.lines), self.build.notes)
            self._show(self.files, self.done)
            self._reveal_later(self.done.frame)
            self.mods.build_finished(event)             # S5.2 B1: a new app may clash with the applied mods
        elif isinstance(event, BUILD_EVENTS):
            self.build.on_event(event)

    def refresh(self):
        """R5: what is enabled follows the controller: nothing while it is busy (preparing, running or saving)."""
        idle = self.controller.idle
        self.files.set_busy(not idle)
        self.updates.set_idle(idle)
        self.background.refresh()
        for tab in (self.settings, self.mods, self.controls, self.storage, self.saves):
            tab.refresh(idle)
        self._show_problems()
        can_build = not self.updates.restart_needed             # S4.1: the old code is loaded until reopened
        self.checks.set_enabled(self.controller.selection()[0] is not None and can_build, idle and can_build)
        self.done.set_idle(idle)
        if self._building:
            self.build.refresh()

    def _settings_in(self):
        if self._loaded:
            return
        self._loaded = True
        self.files.enable_games()
        if self._preselect:
            (game, paths), self._preselect = self._preselect, None
            self.controller.select_game(game)
            if paths:
                self.controller.set_paths(**paths)
            self.files.fill()

    def _exit(self):
        """B1: a quit signal. No question: cancel a running build, then close once it has stopped; the pump keeps
        following it as long as it takes."""
        if self.controller.idle:
            self.close()
        elif not self._close_pending:
            self._close_pending = True
            self.problems.configure(text=text('gui.exit_requested'))
            if self._building:
                self.build.cancel()
            else:
                self.controller.cancel()

    def close(self):
        """WM_DELETE_WINDOW. While busy (R6): ask once; on yes, cancel and close when idle. Idempotent."""
        if self._closed:
            return
        if not self.controller.idle:
            if not self._close_pending and self.confirm(text('gui.confirm_close')):
                self._close_pending = True
                if self._building:
                    self.build.cancel()                 # closing audit 2: the panel shows the cancel and its retry
                else:
                    self.controller.cancel()
            return
        self._closed = self.files.closed = True
        self.storage.close()
        self.saves.close()
        self.build.stop()
        if self._after is not None:
            self.root.after_cancel(self._after)
            self._after = None
        self.controller.close()
        self.root.destroy()


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
