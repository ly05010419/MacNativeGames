"""The window's Saves tab (S2.4): where each game keeps its save, what it holds, and backups made of plain files plus
backup.json. A view: saves.py lists, copies, compares and hashes; nothing here deletes a backup.

A restore never starts with deleting: the current save is copied to <game>-pre-restore-<time> under the same backup
root first, the difference over the target is shown before the player confirms, and files the backup does not hold are
left where they are. Reading a save folder runs on a thread, like the Storage tab's larger folders."""
import threading
import tkinter as tk
from pathlib import Path

from tools.builder import game_state, saves, storage
from tools.builder.games import GAMES
from tools.builder.gui_style import BAD, GOOD, MUTED, TEXT, WRAP, muted
from tools.builder.gui_widgets import FlatButton
from tools.builder.strings import text

POLL_MS = 150


class SavesPanel:
    EVENTS = ()

    def __init__(self, parent, footer, controller, fonts, choose, confirm, home, environ, now):
        self.controller, self.fonts, self.choose, self.confirm = controller, fonts, choose, confirm
        self.home, self.environ, self.now = Path(home), environ, now
        self._idle, self._closed, self._gen, self._done = True, True, 0, None
        self._after, self._sources = None, ()
        self.frame = tk.Frame(parent)
        tk.Label(self.frame, text=text('gui.saves.title'), anchor='w', font=fonts['section']).pack(
            anchor='w', pady=(4, 0))
        muted(tk.Label(self.frame, text=text('gui.saves.note'), anchor='w', justify='left', wraplength=WRAP),
              fonts['small']).pack(anchor='w', pady=(6, 0))
        self.root_line = muted(tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP),
                               fonts['small'])
        self.root_line.pack(anchor='w', pady=(8, 0))
        self.rows = tk.Frame(self.frame)
        self.rows.pack(fill='x', pady=(8, 0))
        buttons = tk.Frame(self.frame)
        buttons.pack(anchor='w', pady=(12, 0))
        self.restore_button = FlatButton(buttons, text('gui.saves.restore'), self.restore, min_size=(110, 32))
        self.restore_button.pack(side='left')
        FlatButton(buttons, text('gui.saves.refresh'), self.opened, min_size=(110, 32)).pack(
            side='left', padx=(8, 0))
        self.status = tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP,
                               font=fonts['small'])
        self.status.pack(anchor='w', pady=(10, 0))
        self.foot = tk.Frame(footer)

    # --- what the game keeps, read on a thread ---
    def _game(self):
        return self.controller.selection()[0]

    def backup_root(self):
        return saves.default_backup_root(self.home)

    def opened(self):
        self._closed = False
        game = self._game()
        for child in self.rows.winfo_children():
            child.destroy()
        self.status.configure(text='', foreground=MUTED)
        self.root_line.configure(text=text('gui.saves.root', path=self.backup_root()))
        if game is None:
            self._stop_timer()
            muted(tk.Label(self.rows, text=text('gui.settings.choose_game'), anchor='w', wraplength=WRAP),
                  self.fonts['small']).pack(anchor='w')
            return
        spec, home, environ, root = GAMES[game], self.home, self.environ, self.backup_root()
        self._start('read', lambda: {
            'game': game,
            'items': (saves.source(spec, home, environ), *saves.nxnative_sources(home, (spec.title_id,))),
            'backups': saves.find_backups(root), 'root': root})

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

    def _start(self, kind, func):
        self._gen += 1
        gen = self._gen
        self._done = None

        def work():
            try:
                done = (gen, kind, func(), None)
            except Exception as error:      # a bad backup is reported, never lost in a dead thread
                done = (gen, kind, None, str(error) or error.__class__.__name__)
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
        _, kind, value, error = done
        if error is not None:
            self.status.configure(text=text('gui.saves.restore_failed', reason=error), foreground=BAD)
            return
        {'read': self._render, 'backup': self._backed_up,
         'plan': self._confirm, 'restore': self._restored}[kind](value)

    def _render(self, data):
        self._sources = data['items']
        for child in self.rows.winfo_children():
            child.destroy()
        for source in self._sources:
            self._row(source)
        count = len(data['backups'])
        note = (text('gui.saves.list', path=data['root']) + f'  ({count})') if count else \
            text('gui.saves.no_backups', path=data['root'])
        muted(tk.Label(self.rows, text=note, anchor='w', justify='left', wraplength=WRAP),
              self.fonts['small']).pack(anchor='w', pady=(10, 0))
        self._paint()

    def _row(self, source):
        row = tk.Frame(self.rows)
        row.pack(fill='x', pady=(0, 8))
        name = text('gui.saves.nxnative') + f' {source.title_id}' if source.kind == 'nxnative' else \
            text('gui.saves.profile')
        tk.Label(row, text=name, anchor='w', foreground=TEXT).pack(anchor='w')
        muted(tk.Label(row, text=str(source.path), anchor='w', justify='left', wraplength=WRAP),
              self.fonts['small']).pack(anchor='w')
        if source.exists and source.files:
            line = text('gui.saves.files', size=storage.human(source.bytes), files=len(source.files))
        else:
            line = text('gui.saves.missing', path=source.path)
        muted(tk.Label(row, text=line, anchor='w', justify='left', wraplength=WRAP), self.fonts['small']).pack(
            anchor='w')
        button = FlatButton(row, text('gui.saves.backup'), lambda s=source: self.backup(s), min_size=(110, 28),
                            font=self.fonts['small'])
        button.pack(anchor='w', pady=(4, 0))
        button.configure(state='normal' if source.exists and source.files else 'disabled')

    # --- backup ---
    def backup(self, source):
        if not source.exists or not source.files:
            self.status.configure(text=text('gui.saves.missing', path=source.path), foreground=MUTED)
            return
        chosen = self.choose('folder', str(self.backup_root()))
        if not chosen:
            return
        root, game, now = Path(chosen).expanduser(), self._game(), self.now()
        label = game if source.kind == 'profile' else f'{game}-{source.title_id}'
        self.status.configure(text=text('gui.saves.reading'), foreground=MUTED)
        self._start('backup', lambda: saves.backup(source.path, root, label, game or source.game, now,
                                                   source.title_id, with_hash=True))

    def _backed_up(self, value):
        backup, reason = value
        if backup is None:
            self.status.configure(text=text('gui.saves.backup_failed', reason=reason), foreground=BAD)
            return
        self.opened()                   # the list reads again, then the word about the backup stays
        self.status.configure(text=text('gui.saves.backed_up', path=backup.directory), foreground=GOOD)

    # --- restore ---
    def restore(self):
        if self._game() is None:
            self.status.configure(text=text('gui.settings.choose_game'), foreground=MUTED)
            return
        chosen = self.choose('folder', str(self.backup_root()))
        if not chosen:
            return
        self._start('plan', lambda: self._plan(Path(chosen).expanduser()))

    def _plan(self, directory):
        backup = saves.read_backup(directory)
        if backup is None:
            return {'backup': None, 'target': None, 'plan': None}
        target = saves.source(GAMES[self._game()], self.home, self.environ).path
        return {'backup': backup, 'target': target, 'plan': saves.plan_restore(backup, target)}

    def _confirm(self, value):
        backup = value['backup']
        if backup is None:
            self.status.configure(text=text('gui.saves.not_backup'), foreground=BAD)
            return
        game = self._game()
        if backup.game and backup.game != game:
            self.status.configure(text=text('gui.saves.wrong_game', game=GAMES[backup.game].display_name)
                                  if backup.game in GAMES else text('gui.saves.not_backup'), foreground=BAD)
            return
        plan = value['plan']
        if not self.confirm(text('gui.saves.restore_plan', backup=backup.directory, target=value['target'],
                                 write=len(plan.write), kept=len(plan.kept), same=len(plan.same))):
            return
        self.status.configure(text=text('gui.saves.restoring'), foreground=MUTED)
        target, root, now = value['target'], self.backup_root(), self.now()
        data = self.home / 'Library/Application Support/MacNativeGames' / GAMES[game].data_folder
        self._start('restore', lambda: self._do_restore(backup, target, root, now, data))

    def _do_restore(self, backup, target, root, now, data):
        if game_state.game_running(data):   # the game would write its own save back over the restored one
            return {'error': text('gui.saves.running')}
        pre = None
        if saves.list_files(target):        # nothing to keep when the game has not saved yet (a new Mac)
            pre, reason = saves.backup(target, root, f'{backup.game or "save"}-pre-restore', backup.game, now,
                                       backup.title_id, with_hash=False)
            if pre is None:
                return {'error': reason}
        plan, error = saves.restore(backup, target)
        return {'pre': pre, 'plan': plan, 'error': error, 'from': backup.directory}

    def _restored(self, value):
        if value.get('error'):
            self.status.configure(text=text('gui.saves.restore_failed', reason=value['error']), foreground=BAD)
            return
        self.opened()                   # the saved files change under the rows, so read them again
        pre = text('gui.saves.pre_backup', path=value['pre'].directory) + '\n' if value['pre'] else ''
        self.status.configure(text=pre + text('gui.saves.restored', write=len(value['plan'].write),
                                              backup=value['from']), foreground=GOOD)

    def refresh(self, idle):
        self._idle = idle
        self._paint()

    def _paint(self):
        self.restore_button.configure(state='normal' if self._idle and self._game() else 'disabled')
