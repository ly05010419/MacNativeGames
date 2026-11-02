"""The window's game choice and path rows (S2.2, W3; S4.4 layout). A view: it holds no rule.

The games and the fields come from the controller, every text from strings.py. The game is chosen with two large
segments (gui_widgets.Segmented); the paths come in groups by importance (game files, then update and system fonts), and
the rest under Advanced, folded at start. An entry is sent to the controller on <<Commit>> (Return or focus out) when
its text changed. Clicking a game or a Choose... button first commits every row (commit_all), because in aqua a click
does not take the focus. A long path shows its end, the file name."""
import tkinter as tk

from tools.builder.gui_style import ROW_GAP, muted, section
from tools.builder.gui_widgets import FlatButton, Segmented
from tools.builder.strings import text

KINDS = {'game': 'file', 'keys': 'file', 'update': 'file', 'firmware': 'folder', 'shared_fonts': 'folder',
         'work': 'folder', 'out': 'save'}
HINTED = ('work', 'out')
GROUPS = (('gui.section.files', ('game', 'keys')), ('gui.section.update_fonts', ('update', 'firmware', 'shared_fonts')))
GAME_SIZE = (220, 48)


class FilesPanel:
    def __init__(self, parent, controller, choose, on_changed, fonts, on_layout):
        self.controller, self.choose, self.on_changed, self.fonts = controller, choose, on_changed, fonts
        self.on_layout = on_layout          # S4.4b: Advanced changed the body's height
        self.frame = tk.Frame(parent)
        self.frame.columnconfigure(1, weight=1)
        self.game = tk.StringVar(master=parent, value='')
        self.radios, self.rows, self.last = [], {}, {}
        self.closed, self.loaded, self.advanced_open = False, False, False
        section(self.frame, text('gui.section.game'), fonts['section']).grid(row=0, column=0, columnspan=3, sticky='w')
        games = tk.Frame(self.frame)
        games.grid(row=1, column=0, columnspan=3, sticky='w', pady=(6, 0))
        selector = Segmented(games, tuple(controller.games()), self.game, self.pick_game, font=fonts['choice'],
                             size=GAME_SIZE, state='disabled', gap=16)
        selector.frame.pack(side='left')
        self.radios = selector.segments                 # the chosen game is the accent; none at start
        fields = tuple(controller.path_fields())
        grouped = [f for _, names in GROUPS for f in names]
        row = 2
        for key, names in GROUPS:
            row = self._heading(text(key), row)
            for field in (f for f in names if f in fields):
                row = self._field(field, row)[0]
        self.advanced_button = section(self.frame, '', fonts['section'])
        self.advanced_button.configure(takefocus=1, cursor='hand2')
        self.advanced_button.grid(row=row, column=0, columnspan=3, sticky='w', pady=(ROW_GAP + 4, 2))
        for sequence in ('<Button-1>', '<space>', '<Return>'):
            self.advanced_button.bind(sequence, lambda e: self.toggle_advanced())
        row += 1
        self.advanced_rows = []
        for field in (f for f in fields if f not in grouped):          # work, out, and any field added later
            row, made = self._field(field, row)
            self.advanced_rows += made
        self._show_advanced(False)

    def _heading(self, title, row):
        section(self.frame, title, self.fonts['section']).grid(row=row, column=0, columnspan=3, sticky='w',
                                                                pady=(ROW_GAP + 4, 2))
        return row + 1

    def _field(self, field, row):
        """One path row (label, entry about 32 px high, Choose...) and its hint; (next row, the widgets made)."""
        label = tk.Label(self.frame, text=text(f'gui.field.{field}'))
        label.grid(row=row, column=0, sticky='w', padx=(0, 14), pady=ROW_GAP // 2)
        entry = tk.Entry(self.frame, state='disabled')
        entry.grid(row=row, column=1, sticky='we', ipady=2)
        entry.bind('<<Commit>>', lambda e, f=field: self.commit(f))
        button = FlatButton(self.frame, text('gui.choose'), lambda f=field: self.pick_path(f), state='disabled',
                            min_size=(110, 36))
        button.grid(row=row, column=2, padx=(10, 0))
        self.rows[field], self.last[field] = (entry, button), ''
        made = [label, entry, button]
        row += 1
        if field in HINTED:
            hint = muted(tk.Label(self.frame, text=text('gui.hint.default_next_to_game')), self.fonts['small'])
            hint.grid(row=row, column=1, sticky='w')
            made.append(hint)
            row += 1
        return row, made

    def toggle_advanced(self):
        self._show_advanced(not self.advanced_open)

    def _show_advanced(self, shown):
        self.advanced_open = shown
        for widget in self.advanced_rows:
            widget.grid() if shown else widget.grid_remove()
        self.advanced_button.configure(text=('▾  ' if shown else '▸  ') + text('gui.section.advanced'))
        self.on_layout()

    def enable_games(self):
        self.loaded = True
        for radio in self.radios:
            radio.configure(state='normal')

    def set_busy(self, busy):
        """S2.3, R5: everything disabled while the controller is busy; afterwards, as before the build."""
        game_chosen = self.controller.selection()[0] is not None
        for radio in self.radios:
            radio.configure(state='normal' if self.loaded and not busy else 'disabled')
        for entry, button in self.rows.values():
            state = 'normal' if game_chosen and not busy else 'disabled'
            entry.configure(state=state)
            button.configure(state=state)

    def pick_game(self, game_id):
        """The game button's command: commit what was typed for the current game, then switch (g3: not to itself)."""
        self.commit_all()
        if game_id == self.controller.selection()[0]:
            return
        self.controller.select_game(game_id)
        self.fill()
        self.on_changed()

    def fill(self):
        """Rows enabled first (a disabled Entry ignores insert), then filled from the controller; last values reset."""
        self.game.set(self.controller.selection()[0] or '')
        _, fields = self.controller.selection()
        for field, (entry, button) in self.rows.items():
            entry.configure(state='normal')
            button.configure(state='normal')
            entry.delete(0, 'end')
            entry.insert(0, fields.get(field) or '')
            entry.xview('end')
            self.last[field] = fields.get(field) or ''

    def commit(self, field):
        """Sends the row to the controller when its text changed; an emptied row sends None (clears that path).
        True when something was sent (S2.3: Build then knows the checks are stale)."""
        if self.closed or self.controller.selection()[0] is None:
            return False
        value = self.rows[field][0].get().strip()
        if value == self.last[field]:
            return False
        self.last[field] = value
        self.controller.set_paths(**{field: value or None})
        self.on_changed()
        return True

    def commit_all(self):
        return [self.commit(field) for field in self.rows].count(True) > 0

    def pick_path(self, field):
        """A Choose... button: commit every row, ask the picker from the committed text, commit its answer."""
        self.commit_all()
        entry = self.rows[field][0]
        chosen = self.choose(KINDS[field], self.last[field])
        if chosen is None:
            return
        entry.delete(0, 'end')
        entry.insert(0, chosen)
        entry.xview('end')
        self.commit(field)
