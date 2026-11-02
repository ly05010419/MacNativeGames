"""The window's Mods tab (S5.2): the player's own mods for the latest version of the chosen game, each with its
validation, a toggle only when valid, the order, Add, Remove and Apply. A view: the controller (controller.mods) keeps
the library and applies it; the tab shows the state it sends.

The builder ships no mod. Valid means: its paths fit this app's RomFS (none misspelt) and it replaces at least one file
of the game (the builder cannot know whether it was played, so the tab says nothing about play). An invalid mod stays
listed, greyed, with its reason and no toggle. Paths the app replaces itself and code patches (they need a build with
the mod) and paths another enabled mod also replaces with other bytes (the lower one wins; mod_shared) are listed under
the mod. Apply mods is the footer's primary button, with its reason beside it when it cannot run. Removing a mod deletes
only the builder's copy, after a confirmation; the player's own backups are removed only on request. After a successful
build the applied mods are checked against the new app, and when they no longer fit, the done panel says so (a new
overlay that clashes with a mod would stop the game at start). The full check (and the first-use import) runs on opening
the tab, after Apply and after a build; a toggle or a move reads the library only.

S5.4: Add takes a folder or a .zip, and a refusal is said in words in the status line. A mod whose code check is ok or
unchecked has a second toggle, its code in the next build (a mod with code only has that toggle alone); the tab says
when the app holds other code than the toggles (build again). The Build tab's line (show_code) names the code mods the
next build merges, or says they need the update file; the library is read quickly once per chosen game and app so that
line is right before the tab is opened. A failed build stopped by a patch for another build of main is explained in the
done panel. S5.5: Add archive... takes .zip, .7z and .rar; a pack of several mods is added as one mod each and the
status says how many; a mod made for another version says which and points to TKMM; the enabled mods a newer check
switched off are named until the next full read. Closing the window while an archive is extracted asks first, and its
cancel stops the extraction. S7.5b: every toggle is gui_widgets.Toggle, an on/off switch."""
import tkinter as tk

from tools.builder.gui_settings import short_path
from tools.builder.gui_style import BAD, GOOD, LINE, MUTED, WRAP, muted
from tools.builder.gui_widgets import INDENT, FlatButton, Toggle
from tools.builder.session import ModsApplied, ModsStateReady, ProcessError
from tools.builder.strings import text, version_span


class ModsPanel:
    def __init__(self, parent, footer, controller, fonts, choose, confirm, note, show_code):
        self.controller, self.fonts, self.choose, self.confirm = controller, fonts, choose, confirm
        self.note_done, self.show_code = note, show_code    # a line in the done panel; the Build tab's code line
        self.state, self.toggles, self._stale, self._after_build, self._changed = None, {}, False, False, False
        self._code, self._code_line, self._primed, self._adding = (None, ()), '', None, False
        self.frame = tk.Frame(parent)
        self.title = tk.Label(self.frame, text='', anchor='w', font=fonts['section'])
        self.title.pack(anchor='w', pady=(4, 0))
        self.path = muted(tk.Label(self.frame, text='', anchor='w'), fonts['small'])
        self.path.pack(anchor='w')
        self.note = muted(tk.Label(self.frame, text=text('gui.mods.note'), anchor='w', justify='left', wraplength=WRAP))
        top = tk.Frame(self.frame)
        top.pack(fill='x', pady=(10, 0))
        self.add_button = FlatButton(top, text('gui.mods.add'), self.add, font=fonts['small'])
        self.zip_button = FlatButton(top, text('gui.mods.add_archive'), lambda: self.add('archive'),
                                     font=fonts['small'])
        self.changed = tk.Label(self.frame, text=text('gui.mods.changed'), anchor='w', justify='left', foreground=BAD,
                                wraplength=WRAP)
        self.pending = tk.Label(self.frame, text=text('gui.mods.code_pending'), anchor='w', justify='left',
                                foreground=BAD, wraplength=WRAP)
        self.switched = tk.Label(self.frame, text='', anchor='w', justify='left', foreground=BAD, wraplength=WRAP)
        self.not_applied = tk.Label(self.frame, text=text('gui.mods.not_applied'), anchor='w', justify='left',
                                    foreground=BAD, wraplength=WRAP)   # S7.3 N2
        self.rows = tk.Frame(self.frame)
        self.rows.pack(fill='x')
        self.backups = tk.Frame(self.frame)
        self.foot = tk.Frame(footer)
        tk.Frame(self.foot, height=1, background=LINE).pack(fill='x', pady=(0, 14))
        self.apply_button = FlatButton(self.foot, text('gui.mods.apply'), self.apply, kind='primary',
                                       font=fonts['action'], size=(170, 44), state='disabled')
        self.apply_button.pack(side='right')
        self.reason = muted(tk.Label(self.foot, text='', anchor='e'), fonts['small'])
        self.reason.pack(side='right', padx=(0, 12))
        self.status = muted(tk.Label(self.foot, text='', anchor='w', justify='left', wraplength=WRAP - 350),
                            fonts['small'])
        self.status.pack(side='left')

    def opened(self):
        if self.controller.selection()[0] is None:
            self._clear()
            self.state = None
            self.title.configure(text=text('gui.settings.choose_game'))
            return
        self._stale = self.controller.mods.busy
        if not self._stale:
            self.title.configure(text=text('gui.settings.reading'))
            self.controller.mods.state()

    def build_finished(self, finished):
        """B1: after a successful build, check the applied mods against the new app; the done panel hears if they no
        longer fit. S5.4 R1: a build stopped by a code patch for another build of main is explained."""
        if finished.outcome.name == 'FAILED' and 'for another build of main' in (finished.error or ''):
            self.note_done(text('gui.mods.code_other_build'))
        if finished.outcome.name != 'SUCCESS' or self.controller.selection()[0] is None:
            return
        self._after_build = True
        if self.controller.mods.busy:
            self._stale = True
        else:
            self.controller.mods.state()

    def on_event(self, event):
        if isinstance(event, ModsStateReady):
            if self._after_build and event.state.checked:
                self._after_build = False
                if event.state.changed:
                    self.note_done(text('gui.mods.after_build'))
            self.render(event.state)
        elif isinstance(event, ModsApplied):
            r = event.result
            lines = [text(f'gui.mods.applied.{r.reason or "ok"}', removed=r.removed_backups)]
            if not r.ok:                                # a success is one short line
                lines += list(r.lines[-3:])
            self.status.configure(text='\n'.join(lines), foreground=GOOD if r.ok else BAD)
            self._stale = True                          # read the library again after Apply
        elif isinstance(event, ProcessError):
            self.status.configure(text=text('gui.settings.error', error=event.error), foreground=BAD)
        if self._stale and not self.controller.mods.busy and self.controller.selection()[0] is not None:
            self._stale = False
            self.controller.mods.state()

    def _clear(self):
        for frame in (self.rows, self.backups):
            for child in frame.winfo_children():
                child.destroy()
        self.toggles = {}
        for widget in (self.note, self.changed, self.pending, self.switched, self.not_applied, self.backups,
                       self.add_button, self.zip_button):
            widget.pack_forget()

    def render(self, state):
        self.state, self._code = state, (state.game, state.code_next)
        self._clear()
        if self._adding:
            self._adding = False
            refused = state.refused and text(f'gui.mods.refused.{state.refused[0]}', detail=state.refused[1])
            many = state.added and state.added[1] > 1 and text('gui.mods.added_pack', n=state.added[1],
                                                               name=state.added[0])
            self.status.configure(text=refused or many or text('gui.mods.added'), foreground=BAD if refused else GOOD)
        self.path.configure(text=short_path(state.app) if state.app else '')
        if state.reason:
            self.title.configure(text=text(f'gui.mods.reason.{state.reason}', name=state.name,
                                           version=state.version_label))
            return
        self.title.configure(text=text('gui.settings.title', name=state.name, version=state.version_label))
        self.note.pack(anchor='w', pady=(10, 0), before=self.rows)
        self.add_button.pack(side='left')
        self.zip_button.pack(side='left', padx=(8, 0))
        self._changed = state.changed if state.checked else self._changed     # a quick read keeps the last check
        if self._changed:
            self.changed.pack(anchor='w', pady=(10, 0), before=self.rows)
        if state.code_pending:
            self.pending.pack(anchor='w', pady=(10, 0), before=self.rows)
        if state.not_applied:
            self.not_applied.pack(anchor='w', pady=(10, 0), before=self.rows)
        if state.switched_off:                          # S5.5 C1b: until the next full read
            self.switched.configure(text=text('gui.mods.switched_off', mods=', '.join(
                f'{name} ({version_span(v)})' if v else name for name, v in state.switched_off)))
            self.switched.pack(anchor='w', pady=(10, 0), before=self.rows)
        if not state.mods:
            muted(tk.Label(self.rows, text=text('gui.mods.empty'), anchor='w')).pack(anchor='w', pady=(14, 0))
        for index, mod in enumerate(state.mods):
            self._row(mod, index, len(state.mods))
        if state.player_backups:
            self.backups.pack(anchor='w', fill='x', pady=(18, 0))
            muted(tk.Label(self.backups, text=text('gui.mods.backups', n=len(state.player_backups)), anchor='w',
                           wraplength=WRAP - 150, justify='left'), self.fonts['small']).pack(side='left')
            FlatButton(self.backups, text('gui.mods.remove_backups'), self.remove_backups,
                       font=self.fonts['small']).pack(side='right')

    def _row(self, mod, index, count):
        v = mod.validation
        box = tk.Frame(self.rows)
        box.pack(fill='x', pady=(18, 0))
        line = tk.Frame(box)
        line.pack(fill='x')
        var = tk.BooleanVar(master=self.frame, value=mod.enabled)
        toggle = Toggle(line, mod.name, var, lambda m=mod.id, b=var: self.enable(m, b), font=self.fonts['section'],
                        state='normal' if v.valid else 'disabled')
        toggle.pack(side='left')
        self.toggles[mod.id] = (toggle, var)
        if v.code in ('ok', 'unchecked'):               # S5.4 C3: mod_code.INCLUDABLE, a build merges it
            code = tk.BooleanVar(master=self.frame, value=mod.code)
            switch = Toggle(box, text('gui.mods.code_toggle'), code, lambda m=mod.id, b=code: self.code(m, b),
                            font=self.fonts['small'])
            switch.pack(anchor='w', padx=(INDENT, 0), pady=(4, 0))
            self.toggles[mod.id + ':code'] = (switch, code)
        for label, step, allowed in (('▲', -1, index > 0), ('▼', 1, index < count - 1)):
            FlatButton(line, label, lambda m=mod.id, s=step: self.move(m, s), font=self.fonts['small'],
                       state='normal' if allowed else 'disabled').pack(side='right', padx=(4, 0))
        FlatButton(line, text('gui.mods.remove'), lambda m=mod.id, n=mod.name: self.remove(m, n),
                   font=self.fonts['small']).pack(side='right', padx=(4, 0))
        details = [text('gui.mods.valid', replaced=v.replaced, added=v.added) if v.valid
                   else text(f'gui.mods.invalid.{v.reason}', paths=', '.join(v.misspelt[:3]),
                             versions=version_span(v.versions) if v.versions else '')]
        if v.other_ignored:
            details.append(text('gui.mods.other_ignored'))
        if v.taken:
            details.append(text('gui.mods.taken', paths=', '.join(v.taken[:3])))
        if v.code != 'none':
            details.append(text(f'gui.mods.code.{v.code}'))
        details += [text(f'gui.mods.notes.{n}') for n in mod.notes]
        if mod.id in self.state.outdated:                   # E6 S6.3: the icons mod
            details.append(text('gui.mods.icons_outdated'))
        if mod.conflicts:
            details.append(text('gui.mods.conflicts', n=len(mod.conflicts), paths=', '.join(mod.conflicts[:2])))
        colour = MUTED if v.valid or v.code in ('ok', 'unchecked') else BAD
        for detail in details:
            tk.Label(box, text=detail, anchor='w', justify='left', wraplength=WRAP - 30, font=self.fonts['small'],
                     foreground=colour).pack(anchor='w', padx=(INDENT, 0), pady=(3, 0))

    def _busy(self):
        return self.controller.mods.busy or self.state is None

    def add(self, kind='folder'):
        if self._busy():
            return
        picked = self.choose(kind, None)
        if picked:
            self.status.configure(text=text('gui.mods.adding'), foreground=MUTED)
            self._adding = True
            self.controller.mods.add(picked)

    def enable(self, mod_id, var):
        if self._busy():
            var.set(not var.get())                      # the switch already flipped it: put it back
            return
        self.controller.mods.enable(mod_id, var.get())

    def code(self, mod_id, var):
        if self._busy():
            var.set(not var.get())
            return
        self.controller.mods.code(mod_id, var.get())

    def move(self, mod_id, step):
        if not self._busy():
            self.controller.mods.move(mod_id, step)

    def remove(self, mod_id, name):
        """S5.7: the window keeps running while the question is open, so the removal is checked again after Yes."""
        if not self._busy() and self.confirm(text('gui.mods.confirm_remove', name=name)) and not self._busy() \
                and any(m.id == mod_id for m in self.state.mods):
            self.controller.mods.remove(mod_id)

    def remove_backups(self):
        names = self.state.player_backups if self.state else ()
        if names and not self._busy() and self.confirm(text('gui.mods.confirm_backups', n=len(names))) \
                and not self._busy():
            names = tuple(n for n in names if n in self.state.player_backups)
            if names:
                self.controller.mods.remove_backups(names)

    def apply(self):
        if not self._can_apply():
            return
        self.apply_button.configure(state='disabled')
        self.status.configure(text=text('gui.mods.applying'), foreground=MUTED)
        self.controller.mods.apply()

    def _can_apply(self):
        return (self.state is not None and not self.state.reason and self.controller.idle
                and not self.controller.mods.busy and not self.controller.start.busy)

    def refresh(self, idle):
        game, fields = self.controller.selection()
        key = (game, fields.get('game'), fields.get('out'))
        if game is not None and key != self._primed and not self.controller.mods.busy:
            self._primed = key                          # S5.4: the Build tab's code line, before the tab is opened
            self.controller.mods.state(full=False)
        names = self._code[1] if self._code[0] == game else ()
        kind = 'gui.mods.code_line' if fields.get('update') else 'gui.mods.code_line_no_update'
        line = text(kind, names=', '.join(names)) if names else ''
        if line != self._code_line:
            self._code_line = line
            self.show_code(line)
        can = idle and self._can_apply()
        self.apply_button.configure(state='normal' if can else 'disabled')
        busy = self.controller.mods.busy or self.controller.start.busy
        self.reason.configure(text='' if can or busy else
                              text('gui.settings.why_not.build' if not idle else 'gui.mods.why_not'))
