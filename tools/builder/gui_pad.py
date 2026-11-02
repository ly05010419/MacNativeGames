"""An Xbox controller drawn by the builder for the Controls tab (E6 S6.2): our own shapes on a Tk canvas, no picture.
A view's helper: no rule.

Every button is one or more canvas items tagged with its physical name (controls_table.PHYSICAL: south east west north
l1 r1 l2 r2 l3 r3 menu view up down left right), drawn with its Xbox label (A B X Y, LB RB, LT RT, L3 R3, View, Menu,
the d-pad's arrows), and clicking it calls on_click(physical). mark(physical) shows one button as chosen: the accent
fill and white text, and its name in the caption under the drawing (never by colour alone). The centre button is
drawn plain and does nothing (it is not a game input)."""
import tkinter as tk

from tools.builder.gui_style import ACCENT, BUTTON, BUTTON_HOVER, CHOSEN_TEXT, LINE, PANEL, TEXT

SIZE, SCALE = (300, 222), 0.88    # drawn on a 340 x 252 grid, then scaled to fit beside the list in the form
BODY_FILL = '#323844'
BODY = ((70, 60), (270, 60), (310, 85), (328, 150), (318, 215), (290, 240), (258, 232), (232, 195), (170, 188),
        (108, 195), (82, 232), (50, 240), (22, 215), (12, 150), (30, 85))
ROUND = {'south': (255, 131, 12), 'east': (280, 106, 12), 'west': (230, 106, 12), 'north': (255, 81, 12),
         'l3': (95, 106, 24), 'r3': (215, 160, 22), 'view': (147, 102, 8), 'menu': (193, 102, 8)}
BOXES = {'l2': (42, 6, 112, 30), 'r2': (228, 6, 298, 30), 'l1': (32, 36, 122, 54), 'r1': (218, 36, 308, 54)}
DPAD = (130, 160)
ARMS = {'up': (-9, -32, 9, -9), 'down': (-9, 9, 9, 32), 'left': (-32, -9, -9, 9), 'right': (9, -9, 32, 9)}
ARROWS = {'up': '▲', 'down': '▼', 'left': '◀', 'right': '▶'}
LABELS = {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'l3': 'L3', 'r3': 'R3', 'l1': 'LB', 'r1': 'RB',
          'l2': 'LT', 'r2': 'RT'}


class PadDrawing:
    def __init__(self, parent, on_click, fonts):
        self.on_click, self.chosen, self.fonts = on_click, None, fonts
        self.canvas = tk.Canvas(parent, width=SIZE[0], height=SIZE[1], background=PANEL, highlightthickness=0)
        c = self.canvas
        c.create_polygon(*[v for p in BODY for v in p], smooth=True, fill=BODY_FILL, outline=LINE, width=2)
        c.create_oval(158, 66, 182, 90, fill=BUTTON, outline=LINE)                     # the centre button: no input
        self.fills = {}
        for name, (x0, y0, x1, y1) in BOXES.items():
            self._button(name, c.create_rectangle(x0, y0, x1, y1, fill=BUTTON, outline=LINE, width=1),
                         ((x0 + x1) / 2, (y0 + y1) / 2), LABELS[name], fonts['small'])
        for name, (x, y, r) in ROUND.items():
            label = LABELS.get(name, '')
            self._button(name, c.create_oval(x - r, y - r, x + r, y + r, fill=BUTTON, outline=LINE, width=1),
                         (x, y), label, fonts['small'] if r > 10 else None)
        for name in ('view', 'menu'):
            x, y, _ = ROUND[name]
            c.create_text(x, y + 17, text=name.capitalize(), fill=TEXT, font=fonts['tiny'], tags=(name,))
        for name, (x0, y0, x1, y1) in ARMS.items():
            cx, cy = DPAD
            self._button(name, c.create_rectangle(cx + x0, cy + y0, cx + x1, cy + y1, fill=BUTTON, outline=LINE),
                         (cx + (x0 + x1) / 2, cy + (y0 + y1) / 2), ARROWS[name], fonts['tiny'])
        c.scale('all', 0, 0, SCALE, SCALE)

    def _button(self, name, shape, centre, label, font):
        self.fills[name] = [shape]
        if label and font is not None:
            self.fills[name].append(self.canvas.create_text(*centre, text=label, fill=TEXT, font=font, tags=(name,)))
        self.canvas.addtag_withtag(name, shape)
        self.canvas.tag_bind(name, '<Button-1>', lambda e, n=name: self.on_click(n))
        self.canvas.tag_bind(name, '<Enter>', lambda e, n=name: self._hover(n, True))
        self.canvas.tag_bind(name, '<Leave>', lambda e, n=name: self._hover(n, False))

    def _hover(self, name, on):
        self.canvas.configure(cursor='hand2' if on else '')
        if name != self.chosen:
            self.canvas.itemconfigure(self.fills[name][0], fill=BUTTON_HOVER if on else BUTTON)

    def mark(self, physical):
        """physical shown as chosen (or none when None)."""
        self.chosen = physical
        for name, (shape, *text) in self.fills.items():
            chosen = name == physical
            self.canvas.itemconfigure(shape, fill=ACCENT if chosen else BUTTON, width=3 if chosen else 1,
                                      outline=CHOSEN_TEXT if chosen else LINE)
            for item in text:
                self.canvas.itemconfigure(item, fill=CHOSEN_TEXT if chosen else TEXT)

    def look(self, physical):
        """(fill, outline width) of a button's shape, for tests."""
        shape = self.fills[physical][0]
        return self.canvas.itemcget(shape, 'fill'), float(self.canvas.itemcget(shape, 'width'))
