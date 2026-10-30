"""The window's Controls tab (E6 S6.2 and S6.2b): the game's inputs on the left, an Xbox controller drawn by the
builder on the right, and Restore defaults, Cancel changes and Save controls in the fixed footer. A view: the
controller reads, previews a swap and writes (controller.controls); names and labels are gui_controls_table's.

Picking a row and then a drawn button gives the input to that button and the button's input to the row's old button (a
swap); the change is shown (the row says "-> <button>", the button is drawn chosen with its name in the caption, never
by colour alone) and only Save writes it. A custom button (a mode, 'none', an aim end, a shared input) says so in words
and is changed only after the player confirms converting it. With the controller type 'auto' the tab first asks which
controller is used. A file the game refuses is shown with its line; then only Restore defaults is offered. Save and
Restore say where the old file was kept and that the game reads it at its next start. S6.3: under the drawing, the icons
line (gui_controls_info). E9 S9.1 (revision 2): the game chosen in Build, each with its own names (C2) and files; a
state read for another game is dropped and read again, and a game whose icons the builder cannot make has no icons
button (C5, C6)."""
import tkinter as tk
from pathlib import Path

from tools.builder import keyboard_layout
from tools.builder.games import GAMES
from tools.builder.gui_controls_info import IconsLine
from tools.builder.gui_controls_table import ORDER, PHYSICAL, XBOX, button_for, input_name
from tools.builder.gui_pad import PadDrawing
from tools.builder.gui_style import ACCENT, BAD, BUTTON, CHOSEN_TEXT, GOOD, LINE, MUTED, TEXT, WRAP, muted
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import ControlsReady, ControlsSaved, IconsChecked, IconsDone, ProcessError
from tools.builder.strings import text


class ControlsPanel:
    EVENTS = (ControlsReady, ControlsSaved, IconsChecked, IconsDone)

    def __init__(self, parent, footer, controller, fonts, confirm, home=None):
        self.controller, self.fonts, self.confirm = controller, fonts, confirm
        self.home = Path(home) if home is not None else Path.home()   # S2.1: where keyboard.txt lives
        self.state, self.desired, self.converted, self.selected, self.rows = None, {}, set(), None, {}
        self._stale = False                             # S9.1: a read owed once the running job ends
        self.frame = tk.Frame(parent)
        self.title = tk.Label(self.frame, text='', anchor='w', font=fonts['section'])
        self.title.pack(anchor='w', pady=(4, 0))
        muted(tk.Label(self.frame, text=text('gui.controls.note'), anchor='w', justify='left', wraplength=WRAP),
              fonts['small']).pack(anchor='w', pady=(6, 0))
        self.mode = tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP, font=fonts['small'])
        self.mode.pack(anchor='w', pady=(6, 0))
        self.ask = tk.Frame(self.frame)
        FlatButton(self.ask, text('gui.controls.answer.xbox'), lambda: self.answer('xbox')).pack(side='left')
        FlatButton(self.ask, text('gui.controls.answer.nintendo'), lambda: self.answer('nintendo')).pack(
            side='left', padx=(8, 0))
        self.body = tk.Frame(self.frame)
        tools = tk.Frame(self.body)
        tools.pack(side='top', fill='x', pady=(0, 12))
        self.preset_button = FlatButton(tools, text('gui.controls.preset.xbox'), self.xbox_letters,
                                        font=fonts['small'])
        self.preset_button.pack(side='left')
        muted(tk.Label(tools, text=text('gui.controls.preset.xbox_note'), anchor='w', justify='left',
                       wraplength=WRAP - 170), fonts['small']).pack(side='left', padx=(10, 0))
        self.list = tk.Frame(self.body)
        self.list.pack(side='left', anchor='n', fill='x', expand=True, padx=(0, 14))
        for switch_input in ORDER:
            self._row(switch_input)
        side = tk.Frame(self.body)
        side.pack(side='right', anchor='n')
        self.pad = PadDrawing(side, self.press, fonts)
        self.pad.canvas.pack()
        self.caption = tk.Label(side, text=text('gui.controls.pick'), anchor='w', justify='left', wraplength=290,
                                font=fonts['small'])
        self.caption.pack(anchor='w', pady=(8, 0))
        self.status = tk.Label(side, text='', anchor='w', justify='left', wraplength=290, font=fonts['small'])
        self.status.pack(anchor='w', pady=(6, 0))
        self.icons = IconsLine(side, self, fonts)       # E6 S6.3
        self.icons_line, self.icons_button = self.icons.line, self.icons.button
        self.foot = tk.Frame(footer)
        tk.Frame(self.foot, height=1, background=LINE).pack(fill='x', pady=(0, 14))
        self.save_button = FlatButton(self.foot, text('gui.controls.save'), self.save, kind='primary',
                                      font=fonts['action'], size=(170, 44), state='disabled')
        self.save_button.pack(side='right')
        self.cancel_button = FlatButton(self.foot, text('gui.controls.cancel'), self.cancel, state='disabled')
        self.cancel_button.pack(side='right', padx=(0, 10))
        self.restore_button = FlatButton(self.foot, text('gui.controls.restore'), self.restore, state='disabled')
        self.restore_button.pack(side='left')
        # S2.1: the key test, under the controller. Bound to the window's toplevel once, and silent unless testing: a
        # press is read, never consumed (no 'break'), so system shortcuts and typing keep working.
        self.test = tk.Frame(self.frame)
        self.test.pack(fill='x', pady=(18, 0))
        tk.Label(self.test, text=text('gui.controls.test.title'), anchor='w',
                 font=fonts['section']).pack(anchor='w')
        muted(tk.Label(self.test, text=text('gui.controls.test.note'), anchor='w', justify='left',
                       wraplength=WRAP), fonts['small']).pack(anchor='w', pady=(4, 0))
        test_row = tk.Frame(self.test)
        test_row.pack(anchor='w', pady=(8, 0))
        self.test_button = FlatButton(test_row, text('gui.controls.test.start'), self.toggle_test,
                                      font=fonts['small'])
        self.test_button.pack(side='left')
        self.test_key = tk.Label(self.test, text='', anchor='w', justify='left', wraplength=WRAP)
        self.test_key.pack(anchor='w', pady=(8, 0))
        self.test_actions = tk.Label(self.test, text='', anchor='w', justify='left', wraplength=WRAP,
                                     font=fonts['small'])
        self.test_actions.pack(anchor='w')
        self.test_note = muted(tk.Label(self.test, text='', anchor='w', justify='left', wraplength=WRAP),
                               fonts['small'])
        self.test_note.pack(anchor='w', pady=(4, 0))
        self._testing, self._pressed, self._test_after, self._layout = False, set(), None, None
        root = self.test.winfo_toplevel()
        root.bind('<KeyPress>', self._on_key, add='+')
        root.bind('<KeyRelease>', self._on_key_release, add='+')

    def _row(self, switch_input):
        row = tk.Frame(self.list, background=BUTTON, cursor='hand2')
        row.pack(fill='x', pady=(0, 4))
        label = tk.Label(row, text='', anchor='w', justify='left', wraplength=150, background=BUTTON,
                         font=self.fonts['small'], padx=10, pady=5)
        label.pack(side='left')
        button = tk.Label(row, text='', anchor='e', justify='right', background=BUTTON, font=self.fonts['small'],
                          padx=8)
        button.pack(side='right')
        for widget in (row, label, button):
            widget.bind('<Button-1>', lambda e, s=switch_input: self.pick(s))
        self.rows[switch_input] = (row, label, button)

    def _paint_row(self, switch_input):
        row, label, button = self.rows[switch_input]
        chosen = switch_input == self.selected
        drawn, before = button_for(self.desired, switch_input), button_for(self.state.inputs, switch_input)
        why = self.state.custom.get(drawn) if drawn not in self.converted else None
        shown = XBOX[drawn] if drawn else text('gui.controls.none')
        shown = ('→ ' if drawn != before else '') + shown
        shown += ' · ' + text(f'gui.controls.custom.{why}') if why else ''
        name, confirmed = input_name(self.state.game, switch_input)
        label.configure(text=name if confirmed else text('gui.controls.unconfirmed', button=name))
        button.configure(text=shown)
        for widget in (row, label, button):
            widget.configure(background=ACCENT if chosen else BUTTON)
        label.configure(foreground=CHOSEN_TEXT if chosen else TEXT if confirmed else MUTED)
        button.configure(foreground=CHOSEN_TEXT if chosen else TEXT)

    def opened(self):
        self.stop_test()
        game = self.controller.selection()[0]
        if self.state is not None and self.state.game != game:
            self._forget()                              # C6: another game's preview never stays on screen
        self.title.configure(text=text('gui.controls.title', name=GAMES[game].display_name) if game
                             else text('gui.settings.choose_game'))
        self._stale = bool(game)
        self._read_owed()

    def _read_owed(self):
        if self._stale and not self.controller.controls.busy and self.controller.selection()[0] is not None:
            self._stale = False
            self.controller.controls.read()

    def _forget(self):
        self.state, self.desired, self.converted, self.selected, self.icons.info = None, {}, set(), None, None
        self.ask.pack_forget()
        self.body.pack_forget()
        self.mode.configure(text='')

    def on_event(self, event):
        if isinstance(event, ControlsReady):
            self.render(event.state)
        elif isinstance(event, ControlsSaved):
            r = event.result
            line = text('gui.controls.saved', backup=r.backup or text('gui.controls.no_backup')) if r.ok else \
                text(f'gui.controls.not_saved.{r.reason}', detail=r.detail)
            self.status.configure(text=line, foreground=GOOD if r.ok else BAD)
            self.controller.controls.read()
        elif isinstance(event, IconsChecked) and self.state is not None:
            self.icons.show(event.info)
        elif isinstance(event, IconsDone):
            r = event.result
            self.status.configure(text=text('gui.controls.icons.done') if r.ok else
                                  text(f'gui.controls.icons.failed', reason=r.reason, detail=r.detail),
                                  foreground=GOOD if r.ok else BAD)
            self.controller.controls.icons_check()
        elif isinstance(event, ProcessError):
            self.status.configure(text=text('gui.settings.error', error=event.error), foreground=BAD)
        self._read_owed()

    def render(self, state):
        if state.game != self.controller.selection()[0]:   # C6: read for the game chosen before: dropped
            self._forget()
            self._stale = True
            return
        self.state, self.converted, self.selected = state, set(), None
        self.desired = dict(state.inputs)
        self.ask.pack_forget()
        self.body.pack_forget()
        if state.reason:
            self.title.configure(text=text(f'gui.controls.{state.reason}'))
            self.mode.configure(text='')
            return
        if state.error:
            self.mode.configure(text=text('gui.controls.broken', line=state.error[0], error=state.error[1]),
                                foreground=BAD)
            return
        if state.by_label is None:
            self.mode.configure(text=text('gui.controls.which'), foreground=TEXT)
            self.ask.pack(anchor='w', pady=(8, 0))
            return
        key = f'gui.controls.mode.{state.type_word}' + (f'.{state.answer}' if state.type_word == 'auto' else '')
        self.mode.configure(text=text(key) + (' ' + text('gui.controls.link') if state.link else ''),
                            foreground=MUTED)
        if state.type_word == 'auto':
            self.ask.pack(anchor='w', pady=(8, 0))
        self.body.pack(fill='x', pady=(14, 0))
        for switch_input in ORDER:
            self._paint_row(switch_input)
        self.pad.mark(None)
        self.caption.configure(text=text('gui.controls.pick'))
        if not self.controller.controls.busy:
            self.controller.controls.icons_check()

    def answer(self, kind):
        if not self.controller.controls.busy:
            self.controller.controls.answer(kind)

    def pick(self, switch_input):
        """A row: it shows as chosen, and so does the button that drives it."""
        if self.state is None or not self.state.inputs:
            return
        previous, self.selected = self.selected, switch_input
        for s in {previous, switch_input} - {None}:
            self._paint_row(s)
        drawn = button_for(self.desired, switch_input)
        self.pad.mark(drawn)
        self._caption('gui.controls.current', switch_input, drawn)

    def press(self, drawn):
        """A drawn button: with a row chosen, the swap (custom buttons first converted, when the player agrees);
        without one, what it does."""
        if self.state is None or not self.state.inputs:
            return
        self.pad.mark(drawn)
        if self.selected is None:
            self.caption.configure(text=text('gui.controls.drives', button=XBOX[drawn],
                                             name=self._name(self.desired.get(drawn))))
            return
        before = button_for(self.desired, self.selected)
        custom = [d for d in (drawn, before) if d and d in self.state.custom and d not in self.converted]
        if custom:
            what = ', '.join(f'{XBOX[d]} ({text("gui.controls.custom." + self.state.custom[d])})' for d in custom)
            if not self.confirm(text('gui.controls.convert', buttons=what)):
                self.pad.mark(before)
                return
            self.converted |= set(custom)
        self.desired = self.controller.controls.swap(self.desired, self.selected, drawn)
        for switch_input in ORDER:
            self._paint_row(switch_input)
        self._caption('gui.controls.pending', self.selected, drawn)

    def xbox_letters(self):
        """The preset: A, B, X, Y on the buttons with those letters (custom buttons it moves converted first, when
        the player agrees); shown, not saved."""
        if self.state is None or not self.state.inputs:
            return
        target = self.controller.controls.xbox_letters(self.desired)
        moved = [d for d, _ in PHYSICAL if target[d] != self.desired[d]]
        custom = [d for d in moved if d in self.state.custom and d not in self.converted]
        if custom:
            what = ', '.join(f'{XBOX[d]} ({text("gui.controls.custom." + self.state.custom[d])})' for d in custom)
            if not self.confirm(text('gui.controls.convert', buttons=what)):
                return
            self.converted |= set(custom)
        self.desired, self.selected = target, None
        for switch_input in ORDER:
            self._paint_row(switch_input)
        self.pad.mark(None)
        self.caption.configure(text=text('gui.controls.preset.done' if moved else 'gui.controls.preset.same'))

    def _name(self, switch_input):
        if switch_input is None:
            return text('gui.controls.none')
        name, confirmed = input_name(self.state.game, switch_input)
        return name if confirmed else text('gui.controls.unconfirmed', button=name)

    def _caption(self, key, switch_input, drawn):
        self.caption.configure(text=text(key, name=self._name(switch_input),
                                         button=XBOX[drawn] if drawn else text('gui.controls.none')))

    def _pending(self):
        return self.state is not None and (self.desired != self.state.inputs or bool(self.converted))

    def save(self):
        if self._can_change() and self._pending():
            self.save_button.configure(state='disabled')
            self.status.configure(text=text('gui.controls.saving'), foreground=MUTED)
            self.controller.controls.save(self.state, self.desired, self.converted)

    def cancel(self):
        """Only the pending changes on screen go; no job is waited for."""
        if self.state is not None:
            self.render(self.state)

    def restore(self):
        if self._can_change() and (self.state.raw is not None or self.state.link) and \
                self.confirm(text('gui.controls.confirm_restore')):
            self.status.configure(text=text('gui.controls.saving'), foreground=MUTED)
            self.controller.controls.restore(self.state)

    def _can_change(self):
        return self.state is not None and not self.controller.controls.busy and self.controller.idle

    def refresh(self, idle):
        can = idle and self._can_change()
        self.save_button.configure(state='normal' if can and self._pending() else 'disabled')
        self.cancel_button.configure(state='normal' if can and self._pending() else 'disabled')
        on_disk = self.state is not None and (self.state.raw is not None or self.state.link)
        self.restore_button.configure(state='normal' if can and on_disk else 'disabled')
        self.icons.refresh(can)

    # --- S2.1: the key test. It reads the builder window's keys (the toplevel binding above) and the game's
    # keyboard.txt, shows the key, and names the action the layout gives it. Nothing is sent to the game, and the
    # handler never returns 'break': system shortcuts and typing are untouched. ---
    def toggle_test(self):
        if self._testing:
            self.stop_test()
            return
        game = self.controller.selection()[0]
        if game is None:
            self.test_key.configure(text=text('gui.settings.choose_game'))
            return
        self._layout = self._layout_for(game)
        self._testing, self._pressed = True, set()
        self.test_button.configure(text=text('gui.controls.test.stop'))
        self.test_key.configure(text=text('gui.controls.test.none'))
        self.test_actions.configure(text='')
        self.test_note.configure(text=text('gui.controls.test.running'))
        if self._layout.problems:
            self.test_note.configure(text=text('gui.controls.test.unread', detail=self._layout.problems[0]))
        elif not self._layout.source or not Path(self._layout.source).is_file():
            self.test_note.configure(text=text('gui.controls.test.defaults'))
        self._test_after = self.test.after(30000, self.stop_test)

    def stop_test(self):
        if self._test_after is not None:
            try:
                self.test.after_cancel(self._test_after)
            except tk.TclError:
                pass
            self._test_after = None
        if not self._testing:
            return
        self._testing, self._pressed = False, set()
        self.test_button.configure(text=text('gui.controls.test.start'))
        self.test_note.configure(text=text('gui.controls.test.stopped'))

    def _layout_for(self, game):
        """The game's keyboard.txt: the runtime's file in Application Support first, then one packed
        with the app; no file at all is the runtime's default table."""
        spec = GAMES[game]
        candidates = [self.home / 'Library' / 'Application Support' / 'MacNativeGames' / spec.data_folder
                      / 'keyboard.txt']
        out = self.controller.selection()[1].get('out')
        if out:
            candidates.append(Path(out) / 'Contents' / 'Resources' / 'keyboard.txt')
        for path in candidates:
            if path.is_file():
                return keyboard_layout.read_layout(path)
        return keyboard_layout.default_layout(candidates[0])

    def _on_key_release(self, event):
        if self._testing and keyboard_layout.is_modifier(event.keysym):
            self._pressed.discard(event.keysym)

    def _on_key(self, event):
        if not self._testing:
            return                              # a plain press elsewhere: read nothing, consume nothing
        keysym = event.keysym
        if keyboard_layout.is_modifier(keysym):
            self._pressed.add(keysym)
        if keysym == 'Escape':
            self.stop_test()
            return
        label = keyboard_layout.label(keysym)
        mods = keyboard_layout.modifiers(event.state, self._pressed | {keysym})
        self.test_key.configure(text=text('gui.controls.test.modifiers', modifiers='+'.join(mods), key=label)
                                if mods else text('gui.controls.test.key', key=label))
        name = keyboard_layout.key_name(keysym)
        actions = keyboard_layout.owner(self._layout, name) if name else ()
        if actions:
            names = ', '.join(keyboard_layout.TITLES.get(action, action) for action in actions)
            self.test_actions.configure(text=text('gui.controls.test.actions', key=label, actions=names),
                                        foreground=TEXT)
        else:
            self.test_actions.configure(text=text('gui.controls.test.unbound', key=label), foreground=MUTED)
        detail = []
        if event.char and event.char.isprintable() and not event.char.isspace():
            detail.append(text('gui.controls.test.char', char=event.char))
        detail.append(text('gui.controls.test.code', code=event.keycode))
        self.test_note.configure(text=' · '.join(detail))
