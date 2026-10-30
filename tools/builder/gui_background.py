"""The window's picture zone (S4.3; S4.4 layout): the player's own picture on the right of the form, and its
controls in Details. A view: the controller copies, draws and removes the files; this only shows the PNG it drew.

The picture fills its zone keeping its proportions: the model draws it at the screen's size, and the zone shows the part
of the window's height in the middle and, across, the part at the chosen focus (left, center or right). No text is drawn
on the picture. Without a picture the zone is a plain darker panel. The canvas is redrawn once after a resize settles.
S4.4b: one click, one command: the controls are disabled at once and do nothing while a change runs (a refused focus
click puts the choice back); the note line is hidden while empty."""
import tkinter as tk

from tools.builder.gui_style import PICTURE_BG, WRAP, muted
from tools.builder.gui_widgets import FlatButton, Segmented
from tools.builder.session import ProcessError
from tools.builder.strings import text

FOCI = ('left', 'center', 'right')
SETTLE_MS = 120


def crop(picture_w, picture_h, zone_w, zone_h, focus):
    """The part of a picture_w x picture_h picture that a zone_w x zone_h zone shows: (x0, y0, x1, y1)."""
    zone_w, zone_h = min(zone_w, picture_w), min(zone_h, picture_h)
    across = {'left': 0.25, 'center': 0.5, 'right': 0.75}[focus]
    x0 = min(max(0, round(picture_w * across - zone_w / 2)), picture_w - zone_w)
    y0 = (picture_h - zone_h) // 2
    return x0, y0, x0 + zone_w, y0 + zone_h


class BackgroundPanel:
    def __init__(self, root, controller, choose, controls_parent, fonts):
        self.root, self.controller, self.choose = root, controller, choose
        self.picture = self.shown = None    # the drawn PNG, and the part of it the canvas shows
        self.saved, self._saving, self._pending, self._focus = False, False, None, 'center'
        self.canvas = tk.Canvas(root, background=PICTURE_BG, width=1, height=1)
        self.canvas.bind('<Configure>', lambda e: self._later())
        self.frame = tk.Frame(controls_parent)
        row = tk.Frame(self.frame)
        row.pack(anchor='w', pady=(6, 0))
        muted(tk.Label(row, text=text('gui.background.label')), fonts['small']).pack(side='left')
        self.choose_button = FlatButton(row, text('gui.background.choose'), self.pick, font=fonts['small'])
        self.choose_button.pack(side='left', padx=(8, 0))
        self.remove_button = FlatButton(row, text('gui.background.remove'), self.remove, font=fonts['small'],
                                        state='disabled')
        self.remove_button.pack(side='left', padx=(6, 0))
        self.focus = tk.StringVar(master=root, value='center')
        selector = Segmented(row, [(v, text(f'gui.background.focus.{v}')) for v in FOCI], self.focus, self.set_focus,
                             font=fonts['small'], state='disabled')
        selector.frame.pack(side='left', padx=(14, 0))
        self.focus_buttons = selector.segments
        self.note = muted(tk.Label(self.frame, text='', anchor='w', justify='left', wraplength=WRAP), fonts['small'])
        controller.background('saved', self.screen())

    def screen(self):
        return self.root.winfo_screenwidth(), self.root.winfo_screenheight()

    def say(self, line):
        self.note.configure(text=line)
        if line:
            self.note.pack(anchor='w')
        else:
            self.note.pack_forget()

    def _hold(self):
        """B3: True (and every control disabled) when a change may start; False while one runs."""
        if self.controller.background_busy:
            return False
        for button in [self.choose_button, self.remove_button] + self.focus_buttons:
            button.configure(state='disabled')
        return True

    def pick(self):
        if self.controller.background_busy:
            return
        source = self.choose('image', None)
        if source and self._hold():
            self._saving = True
            self.say(text('gui.background.saving'))
            self.controller.background('save', source, self.screen())

    def remove(self):
        if self._hold():
            self.controller.background('remove')

    def set_focus(self, value):
        if not self._hold():
            self.focus.set(self._focus)                 # Tk already moved the choice: put it back
            return
        self.focus.set(value)                           # shown at once; the model's answer confirms it
        self.controller.background('focus', self.screen(), value)

    def refresh(self):
        busy = self.controller.background_busy
        self.choose_button.configure(state='disabled' if busy else 'normal')
        self.remove_button.configure(state='normal' if self.saved and not busy else 'disabled')
        for button in self.focus_buttons:
            button.configure(state='normal' if self.picture is not None and not busy else 'disabled')

    def on_event(self, event):
        if isinstance(event, ProcessError):
            self.say(text('gui.background.failed', error=event.error))
            return
        b = event.background
        if b.refused:
            self.say(text(f'gui.background.{b.refused}'))
        elif self._saving:
            self.say('')
        self._saving, self.saved = False, b.path is not None
        self.focus.set(b.focus)
        self._focus = b.focus
        self.show(b.shown)

    def show(self, shown):
        self.picture = None
        if shown is not None:
            try:
                self.picture = tk.PhotoImage(master=self.root, file=str(shown))
            except tk.TclError:
                self.say(text('gui.background.unloadable', path=shown))
        self.draw()

    def _later(self):
        if self._pending is not None:
            self.root.after_cancel(self._pending)
        self._pending = self.root.after(SETTLE_MS, self.draw)

    def draw(self):
        """The part of the picture for the zone's size and the focus; the plain panel without a picture."""
        self._pending = None
        self.canvas.delete('all')
        self.shown = None
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if self.picture is None or width < 2 or height < 2:
            return
        x0, y0, x1, y1 = crop(self.picture.width(), self.picture.height(), width, height, self.focus.get())
        self.shown = tk.PhotoImage(master=self.root, width=x1 - x0, height=y1 - y0)
        self.root.tk.call(self.shown, 'copy', self.picture, '-from', x0, y0, x1, y1, '-to', 0, 0)
        self.canvas.create_image(width // 2, height // 2, image=self.shown, anchor='center')
