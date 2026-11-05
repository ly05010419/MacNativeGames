"""The Mods tab's Textures section (S9.2; E11 G3: a section, not a tab of its own): the texture pack for the chosen
game's latest version, its switch, Add and Remove. A view: the controller (controller.textures, on the Mods part's
jobs) keeps the pack in the mod library; the section shows the part of the Mods state it sends (ModsState.textures), and
the Mods panel hands it every Mods event and places it. The Mods tab asks for the state once on opening, for both; its
footer's Apply mods applies the library, the pack with it, and this section says the outcome in its own status line.

Tears of the Kingdom 1.4.3 only. The section says this Mac's memory (a warning under 24 GB), the pack's files and size,
and what is left to do: Apply when the game does not hold the library, Build when the app's heaps do not match the
pack's switch; once both are done, that Play starts the game in the 8 GB mode."""
import tkinter as tk

from tools.builder.gui_style import BAD, GOOD, LINE, MUTED, WRAP, muted
from tools.builder.gui_widgets import INDENT, FlatButton, Toggle
from tools.builder.session import ModsApplied, ModsStateReady, ProcessError
from tools.builder.strings import text
from tools.builder.textures import GAMES, MEMORY_CHECKED_GB


def shown(state):
    """Whether the Mods tab shows the section for this state: a game that takes a pack, read without a reason."""
    return state is not None and state.game in GAMES and not state.reason and state.textures is not None


class TexturesPanel:
    def __init__(self, parent, controller, fonts, choose, confirm):
        self.controller, self.fonts, self.choose, self.confirm = controller, fonts, choose, confirm
        self.state, self.toggle, self._adding = None, None, False
        self.frame = tk.Frame(parent)
        tk.Frame(self.frame, height=1, background=LINE).pack(fill='x', pady=(0, 14))
        self.title = tk.Label(self.frame, text='', anchor='w', font=fonts['section'])
        self.title.pack(anchor='w', pady=(4, 0))
        self.body = tk.Frame(self.frame)
        self.body.pack(fill='x')
        self.status = muted(tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP), fonts['small'])
        self.status.pack(anchor='w', pady=(10, 0))

    def opened(self):
        """The Mods tab was opened: it asks for the state; with no game chosen, nothing is shown."""
        if self.controller.selection()[0] is None:
            self._clear()
            self.state = None

    def on_event(self, event):
        if isinstance(event, ModsStateReady):
            if self._adding:
                self._adding = False
                refused = event.state.refused
                self.status.configure(text=text(f'gui.mods.refused.{refused[0]}', detail=refused[1]) if refused
                                      else text('gui.textures.added'), foreground=BAD if refused else GOOD)
            self.render(event.state)
        elif isinstance(event, ModsApplied):
            r = event.result
            lines = [text(f'gui.mods.applied.{r.reason or "ok"}', removed=r.removed_backups)]
            self.status.configure(text='\n'.join(lines + ([] if r.ok else list(r.lines[-3:]))),
                                  foreground=GOOD if r.ok else BAD)
        elif isinstance(event, ProcessError):
            self._adding = False
            self.status.configure(text=text('gui.settings.error', error=event.error), foreground=BAD)

    def _clear(self):
        for child in self.body.winfo_children():
            child.destroy()
        self.toggle = None

    def _line(self, key, colour=MUTED, **values):
        tk.Label(self.body, text=text(key, **values), anchor='w', justify='left', wraplength=WRAP,
                 foreground=colour).pack(anchor='w', pady=(10, 0))

    def render(self, state):
        self.state = state
        self._clear()
        if not shown(state):                            # the Mods tab hides the section
            return
        t = state.textures
        self.title.configure(text=text('gui.textures.title', name=state.name, version=state.version_label))
        self._line('gui.textures.note')
        low = t.memory_gb and t.memory_gb < MEMORY_CHECKED_GB
        self._line('gui.textures.memory_low' if low else 'gui.textures.memory', BAD if low else MUTED, gb=t.memory_gb)
        buttons = tk.Frame(self.body)
        buttons.pack(fill='x', pady=(12, 0))
        FlatButton(buttons, text('gui.textures.add'), self.add, font=self.fonts['small']).pack(side='left')
        FlatButton(buttons, text('gui.textures.add_archive'), lambda: self.add('archive'),
                   font=self.fonts['small']).pack(side='left', padx=(8, 0))
        if not t.present:
            self._line('gui.textures.none')
            return
        self._pack(t)
        if t.enabled and state.not_applied:
            self._line('gui.textures.not_applied', BAD)
        if t.heap_pending:
            self._line('gui.textures.build_on' if t.enabled else 'gui.textures.build_off', BAD)
        elif t.built_with_heap:
            self._line('gui.textures.ready', GOOD)

    def _pack(self, t):
        box = tk.Frame(self.body)
        box.pack(fill='x', pady=(18, 0))
        var = tk.BooleanVar(master=self.frame, value=t.enabled)
        self.toggle = (Toggle(box, t.name, var, lambda: self.switch(var), font=self.fonts['section'],
                              state='normal' if t.valid else 'disabled'), var)
        self.toggle[0].pack(side='left')
        FlatButton(box, text('gui.textures.remove'), lambda: self.remove(t.name),
                   font=self.fonts['small']).pack(side='right')
        detail = text('gui.textures.files', files=t.files, size=f'{t.size / 1e9:.1f}') if t.valid else \
            text('gui.textures.invalid', reason=t.reason)
        tk.Label(self.body, text=detail, anchor='w', justify='left', wraplength=WRAP - 30, font=self.fonts['small'],
                 foreground=MUTED if t.valid else BAD).pack(anchor='w', padx=(INDENT, 0), pady=(3, 0))

    def _busy(self):
        return self.controller.mods.busy or self.state is None

    def add(self, kind='folder'):
        if self._busy():
            return
        picked = self.choose(kind, None)
        if picked and not self._busy():
            self.status.configure(text=text('gui.textures.adding'), foreground=MUTED)
            self._adding = True
            self.controller.textures.add(picked)

    def switch(self, var):
        if self._busy():
            var.set(not var.get())                      # the switch already flipped it: put it back
            return
        self.controller.textures.switch(var.get())

    def remove(self, name):
        if not self._busy() and self.confirm(text('gui.textures.confirm_remove', name=name)) and not self._busy():
            self.controller.textures.remove()
