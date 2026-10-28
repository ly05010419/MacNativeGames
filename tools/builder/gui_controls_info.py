"""The Controls tab's icons line (E6 S6.3; E9 S9.1 C5), out of gui_controls.py to keep that file under 300 lines:
under the drawing, whether the game's prompts show the buttons of the saved layout, and "Update the game's button
icons" (after confirming the fontTools download when the builder's venv is missing or broken, and switching off other
font mods), which makes the icons mod and applies the mods. A game whose icons the builder cannot make is told so in
words, with no button."""
import tkinter as tk

from tools.builder.gui_style import BAD, MUTED
from tools.builder.gui_widgets import FlatButton
from tools.builder.strings import text


class IconsLine:
    def __init__(self, parent, panel, fonts):
        self.panel, self.info = panel, None             # the last IconsInfo
        self.line = tk.Label(parent, text='', anchor='w', justify='left', wraplength=290, font=fonts['small'])
        self.line.pack(anchor='w', pady=(12, 0))
        self.button = FlatButton(parent, text('gui.controls.icons.update'), self.update, font=fonts['small'],
                                 state='disabled')
        self.button.pack(anchor='w', pady=(6, 0))

    def show(self, info):
        self.info = info
        if info.reason == 'not_offered':
            self.button.pack_forget()
        elif not self.button.winfo_manager():
            self.button.pack(anchor='w', pady=(6, 0))
        if not info.offered:
            self.line.configure(text=text(f'gui.controls.icons.not.{info.reason}'), foreground=MUTED)
            return
        line = text(f'gui.controls.icons.{info.status}')
        if info.shared:
            line += ' ' + text('gui.controls.icons.shared', inputs=', '.join(info.shared))
        self.line.configure(text=line, foreground=BAD if info.status == 'outdated' else MUTED)

    def update(self):
        info, panel = self.info, self.panel
        if info is None or not info.offered or not panel._can_change() or panel._pending():
            return
        install = info.env != 'ready'
        if install and not panel.confirm(text(f'gui.controls.icons.download.{info.env}')):
            return
        names = ', '.join(name for _, name in info.font_mods)
        if info.font_mods and not panel.confirm(text('gui.controls.icons.switch_off', mods=names)):
            panel.status.configure(text=text('gui.controls.icons.kept', mods=names), foreground=BAD)
            return
        panel.status.configure(text=text('gui.controls.icons.making'), foreground=MUTED)
        panel.controller.controls.update_icons(install, [m for m, _ in info.font_mods])

    def refresh(self, can):
        offered = self.info is not None and self.info.offered and not self.panel._pending()
        self.button.configure(state='normal' if can and offered else 'disabled')
