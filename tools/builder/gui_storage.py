"""The window's Storage tab (S2.3): the build cache and the shader cache -- where they are, what they hold, and the two
things a player does with them, clean and move. A view: storage.py measures, cleans and moves; controller.save_paths
writes the work folder into settings.json without starting a build.

A work folder can hold tens of thousands of files, so measuring, cleaning and moving run on a thread. The thread only
fills self._done; the panel's own timer (POLL_MS) reads it -- never the window's pump, which the view owns. Closing the
window stops the timer without waiting for the thread."""
import threading
import time
import tkinter as tk
from pathlib import Path

from tools.builder import saves, storage
from tools.builder.games import GAMES
from tools.builder.gui_style import BAD, BUTTON, GOOD, LINE, MUTED, TEXT, WRAP, muted
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import PathsSaved
from tools.builder.strings import text

POLL_MS = 150


class StoragePanel:
    EVENTS = (PathsSaved,)

    def __init__(self, parent, footer, controller, fonts, choose, confirm, home, environ, now):
        self.controller, self.fonts, self.choose, self.confirm = controller, fonts, choose, confirm
        self.home, self.environ, self.now = Path(home), environ, now
        self._idle, self._closed, self._gen, self._done = True, True, 0, None
        self._after, self._pending, self._sizes = None, None, {}
        self.frame = tk.Frame(parent)
        tk.Label(self.frame, text=text('gui.storage.title'), anchor='w', font=fonts['section']).pack(
            anchor='w', pady=(4, 0))
        muted(tk.Label(self.frame, text=text('gui.storage.note'), anchor='w', justify='left', wraplength=WRAP),
              fonts['small']).pack(anchor='w', pady=(6, 0))
        self.note = tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP, font=fonts['small'])
        self.rows = {}
        for key, label in (('work', 'gui.storage.work'), ('shader', 'gui.storage.shader')):
            self.rows[key] = self._box(label)
        self.status = tk.Label(self.frame, text='', anchor='w', justify='left', foreground=MUTED,
                               wraplength=WRAP, font=fonts['small'])
        self.status.pack(anchor='w', pady=(12, 0))
        self.foot = tk.Frame(footer)

    def _box(self, label):
        frame = tk.Frame(self.frame)
        frame.pack(fill='x', pady=(12, 0))
        tk.Label(frame, text=text(label), anchor='w', foreground=TEXT).pack(anchor='w')
        path = tk.Label(frame, text='', anchor='w', justify='left', wraplength=WRAP, foreground=MUTED)
        path.pack(anchor='w', pady=(2, 0))
        detail = muted(tk.Label(frame, text='', anchor='w', justify='left', wraplength=WRAP), self.fonts['small'])
        detail.pack(anchor='w', pady=(2, 0))
        buttons = tk.Frame(frame)
        buttons.pack(anchor='w', pady=(6, 0))
        key = 'work' if label == 'gui.storage.work' else 'shader'
        clean = FlatButton(buttons, text('gui.storage.clean'), lambda: self.clean(key), font=self.fonts['small'])
        clean.pack(side='left')
        choose = None
        if key == 'work':   # the shader cache stays where the launcher puts it: it reads no folder chosen here
            choose = FlatButton(buttons, text('gui.storage.choose'),
                                lambda: self.choose_folder(key), font=self.fonts['small'])
            choose.pack(side='left', padx=(8, 0))
        return {'frame': frame, 'path': path, 'detail': detail, 'clean': clean, 'choose': choose}

    # --- the paths this game uses now (storage.py's own rules) ---
    def _game(self):
        return self.controller.selection()[0]

    def _path(self, key):
        game = self._game()
        if game is None:
            return None
        spec, paths = GAMES[game], self.controller.selection()[1]
        saved = self.controller.saved_paths()
        if key == 'work':
            return storage.work_dir(spec, paths.get('game'), saved.work if saved else None)
        return storage.shader_dir(spec, self.home, self.environ)

    def _protected(self):
        game, paths = self._game(), self.controller.selection()[1]
        items = [paths.get('game'), paths.get('out')]
        if game is not None:
            spec = GAMES[game]
            items.append(saves.profile_dir(spec, self.home, self.environ))
        items.append(saves.nxnative_root(self.home))
        return tuple(item for item in items if item)

    def _refuse(self, path):
        """None when this folder may be a cache path and be cleaned, else the reason id."""
        path = Path(path).expanduser()
        if path.exists():
            return storage.refuse_reason(path, self.home, self._protected())
        resolved, home = path.resolve(), self.home.resolve()
        if resolved == Path(resolved.anchor):
            return 'root'
        if resolved == home or resolved in home.parents:
            return 'home'
        for item in self._protected():
            item = Path(item).expanduser().resolve()
            if resolved == item or item in resolved.parents:
                return 'protected'
        return None

    # --- reading, on a thread ---
    def opened(self):
        self._closed = False
        game = self._game()
        for key in self.rows:
            path = self._path(key)
            self.rows[key]['path'].configure(text=str(path) if path else text('gui.storage.missing'))
            self.rows[key]['detail'].configure(text=text('gui.storage.measuring') if path else '')
        self.status.configure(text='', foreground=MUTED)
        if game is None:
            self._stop_timer()
            return
        spec = GAMES[game]
        self.note.configure(text=text('gui.storage.shader_note', name=storage.shader_env(spec)[0]))
        self._start('measure', None, lambda: {key: storage.measure(self._path(key)) for key in ('work', 'shader')})

    def close(self):
        self._closed = True
        self._stop_timer()

    def _stop_timer(self):
        if self._after is not None:
            try:
                self.frame.after_cancel(self._after)
            except tk.TclError:
                pass
            self._after = None

    def _start(self, kind, key, func):
        self._gen += 1
        gen = self._gen
        self._done = None

        def work():
            try:
                done = (gen, kind, key, func(), None)
            except Exception as error:      # a bad path is reported, never lost in a dead thread
                done = (gen, kind, key, None, str(error) or error.__class__.__name__)
            if gen == self._gen:            # a newer request already owns the window
                self._done = done
        threading.Thread(target=work, daemon=True).start()
        self._stop_timer()
        self._after = self.frame.after(POLL_MS, lambda: self._poll(gen))

    def _poll(self, gen):
        self._after = None
        if self._closed or gen != self._gen:
            return
        done = self._done
        if done is None or done[0] != gen:
            self._after = self.frame.after(POLL_MS, lambda: self._poll(gen))
            return
        self._done = None
        _, kind, key, value, error = done
        if kind == 'measure':
            if value is not None:
                for name, size in value.items():
                    self._sizes[name] = size
                    self.rows[name]['detail'].configure(text=self._size_line(name, size))
        elif kind == 'clean':
            self._cleaned(key, value, error)
        elif kind == 'move':
            self._moved(key, value, error)
        self._paint()

    def _size_line(self, key, size):
        if size is None or not self._path(key) or not self._path(key).is_dir():
            return text('gui.storage.missing')
        line = text('gui.storage.exists', size=storage.human(size.bytes), files=size.files)
        if size.errors:
            line += ' ' + text('gui.storage.errors', detail=size.errors[0])
        return line

    # --- clean ---
    def clean(self, key):
        path = self._path(key)
        if path is None:
            self.status.configure(text=text('gui.storage.missing'), foreground=MUTED)
            return
        size = self._sizes.get(key)
        question = text('gui.storage.confirm_clean', path=path,
                        size=storage.human(size.bytes) if size else '?', files=size.files if size else '?')
        if not self.confirm(question):
            return
        self.status.configure(text=text('gui.storage.measuring'), foreground=MUTED)
        self._start('clean', key, lambda: storage.clean(path, self.home, self._protected()))

    def _cleaned(self, key, value, error):
        if value is None:
            self.status.configure(text=text('gui.storage.clean_failed', reason=error or '?'), foreground=BAD)
            return
        removed, reason = value
        if reason is not None:
            shown = text(f'gui.storage.refused.{reason}') if reason in (
                'missing', 'root', 'home', 'system', 'protected', 'file', 'symlink') else reason
            self.status.configure(text=text('gui.storage.clean_failed', reason=shown), foreground=BAD)
            return
        self.status.configure(text=text('gui.storage.cleaned', size=storage.human(removed.bytes),
                                        files=removed.files), foreground=GOOD)
        self._measure_again()

    # --- choose a folder, then move what is there ---
    def choose_folder(self, key):
        old = self._path(key)
        start = str(old.parent) if old and key == 'work' else str(self.default_root())
        chosen = self.choose('folder', start)
        if not chosen:
            return
        new = Path(chosen).expanduser()
        reason = self._refuse(new)
        if reason is not None:
            shown = text(f'gui.storage.refused.{reason}')
            self.status.configure(text=shown, foreground=BAD)
            return
        self._pending = new
        self.controller.save_paths(work=str(new))
        if old and old.exists() and new != old and any(old.iterdir()):
            question = text('gui.storage.confirm_move', path=old, target=new)
            if self.confirm(question):
                self.status.configure(text=text('gui.storage.measuring'), foreground=MUTED)
                self._start('move', key, lambda: storage.move(old, new))
                return
        self.status.configure(text=text('gui.storage.move_target'), foreground=MUTED)

    def _moved(self, key, value, error):
        if value is None or error is not None:
            self.status.configure(text=text('gui.storage.move_failed', reason=error or '?'), foreground=BAD)
            return
        count, reason = value
        if reason is not None:
            shown = text(f'gui.storage.refused.{reason}') if reason in (
                'missing', 'inside_source', 'target_not_empty') else reason
            self.status.configure(text=text('gui.storage.move_failed', reason=shown), foreground=BAD)
            return
        self.status.configure(text=text('gui.storage.moved', count=count), foreground=GOOD)
        self._measure_again()

    def _measure_again(self):
        self._start('measure', None, lambda: {key: storage.measure(self._path(key)) for key in ('work', 'shader')})

    def default_root(self):
        return self.home / 'Documents'

    def on_event(self, event):
        if isinstance(event, PathsSaved):
            if not event.ok:
                self.status.configure(text=text('gui.storage.not_saved', error=event.error), foreground=BAD)
            elif self._pending is not None:
                self.status.configure(text=text('gui.storage.chosen', path=self._pending), foreground=GOOD)
                self._pending = None
            self._measure_again()

    def refresh(self, idle):
        self._idle = idle
        self._paint()

    def _paint(self):
        for box in self.rows.values():
            state = 'normal' if self._idle else 'disabled'
            box['clean'].configure(state=state)
            if box['choose'] is not None:
                box['choose'].configure(state=state)
