"""The window's buttons and selectors, in one shared style (S5.1 polish). Aqua's tk.Button ignores its background, so
with the panel's light text it drew near-white text on a white button; and a Radiobutton drawn as a button shows neither
which one is chosen nor a clean outline. Both are labels with their own colours here, and no view uses aqua's buttons. A
view's helpers: no rule.

FlatButton: every state has its fill and its text together (gui_style.BUTTON_LOOKS): normal, hover, pressed and
disabled; primary (the accent, white bold text) is kept for Build, Play and Apply mods, and is disabled like any other.
A button is never smaller than MIN_SIZE (or the min_size it is given), and grows with its text. Segmented: one row of
segments of the same height with 14 px of padding, no double borders; the chosen one is the accent with white text
and stays so when focus leaves or the selector is disabled. Both take keyboard focus (Tab) and act on Space or
Return; the focus ring is an outline in its own colour, a state apart from hover and from being chosen.

Toggle (S7.5b: an on/off switch instead of a tick): a pill with its knob right and the accent when on, left and
TOGGLE_OFF when off, its label beside it. It behaves as the Checkbutton it replaces: invoke() flips the variable, then
calls the command, which may put it back; disabled, it does nothing and is drawn dimmed (the line colour, a grey knob).
The variable drives the drawing. Tab reaches it; Space or Return switch it; a click on the pill or the label switches
it."""
import tkinter as tk

from tools.builder.gui_style import (ACCENT, BUTTON, BUTTON_LOOKS, BUTTON_OFF, BUTTON_OFF_TEXT, CHOSEN_TEXT, FOCUS_RING,
                                     LINE, MUTED, PANEL, TEXT, TOGGLE_OFF)

MIN_SIZE = (0, 36)
RING = 2                            # the focus outline's width, inside the button's size
TRACK = (40, 22)                    # a switch's pill
KNOB_GAP = 3                        # between the knob and the pill's edge
LABEL_GAP = 10
INDENT = RING + TRACK[0] + LABEL_GAP + RING     # where a switch's label starts: the lines under it line up there


def _focusable(frame, act):
    frame.configure(takefocus=1, highlightthickness=RING, highlightbackground=PANEL, highlightcolor=FOCUS_RING)
    for sequence in ('<space>', '<Return>'):
        frame.bind(sequence, lambda e: act())


class FlatButton:
    def __init__(self, parent, text, command, kind='secondary', font=None, size=None, state='normal', min_size=None):
        self.command, self.kind, self._state, self._hover, self._down = command, kind, state, False, False
        self._size, self._min = size, min_size or MIN_SIZE
        self.frame = tk.Frame(parent, class_='FlatButton')
        self.frame.button = self                        # tests and views find the button from its frame
        _focusable(self.frame, self.invoke)
        self.frame.pack_propagate(False)
        self.label = tk.Label(self.frame, text=text, padx=14, pady=6, cursor='hand2',
                              **({'font': font} if font else {}))
        self.label.pack(fill='both', expand=True)
        for widget in (self.frame, self.label):
            widget.bind('<Enter>', lambda e: self._set(hover=True))
            widget.bind('<Leave>', lambda e: self._set(hover=False, down=False))
            widget.bind('<ButtonPress-1>', lambda e: self._set(down=True))
            widget.bind('<ButtonRelease-1>', self._release)
        self._fit()
        self._paint()

    def __getattr__(self, name):
        """Geometry and winfo calls (pack, grid, grid_remove, winfo_*, master...) are the frame's."""
        if name == 'frame':
            raise AttributeError(name)
        return getattr(self.frame, name)

    def cget(self, key):
        if key == 'state':
            return self._state
        if key == 'kind':
            return self.kind
        return self.label.cget(key)

    def configure(self, state=None, text=None, kind=None):
        if text is not None:
            self.label.configure(text=text)
            self._fit()
        if kind is not None:
            self.kind = kind
        if state is not None:
            self._state = state
        self._paint()

    def invoke(self):
        if self._state == 'normal':
            self.command()

    def _fit(self):
        """A fixed size, or the text's own size and never less than the minimum (the ring included)."""
        width, height = self._size or (max(self.label.winfo_reqwidth() + 2 * RING, self._min[0]),
                                       max(self.label.winfo_reqheight() + 2 * RING, self._min[1]))
        self.frame.configure(width=width, height=height)

    def _set(self, hover=None, down=None):
        self._hover = self._hover if hover is None else hover
        self._down = self._down if down is None else down
        self._paint()

    def _release(self, event):
        inside = 0 <= event.x < event.widget.winfo_width() and 0 <= event.y < event.widget.winfo_height()
        self._set(down=False)
        if inside:
            self.invoke()

    def _paint(self):
        looks = BUTTON_LOOKS[self.kind]
        if self._state != 'normal':
            fill, text = BUTTON_OFF, BUTTON_OFF_TEXT
        else:
            fill, text = looks['pressed' if self._down else 'hover' if self._hover else 'normal']
        self.label.configure(background=fill, foreground=text, cursor='hand2' if self._state == 'normal' else '')
        self.frame.configure(background=fill)


class Segment:
    """One segment: its value, invoke(), its state (the selector's), and its label's colours (cget)."""

    def __init__(self, owner, value, cell, label):
        self.owner, self.value, self.cell, self.label = owner, value, cell, label

    def invoke(self):
        self.owner.pick(self.value)

    def configure(self, state=None):
        self.owner.configure(state=state)

    def cget(self, key):
        if key == 'state':
            return self.owner.state
        if key == 'value':
            return self.value
        return self.label.cget(key)


class Segmented:
    def __init__(self, parent, choices, variable, command, font=None, size=None, state='normal', gap=2):
        self.var, self.command, self.state, self.segments = variable, command, state, []
        self.frame = tk.Frame(parent)
        for value, text in choices:
            cell = tk.Frame(self.frame)
            _focusable(cell, lambda v=value: self.pick(v))
            label = tk.Label(cell, text=text, padx=14, pady=7, cursor='hand2', **({'font': font} if font else {}))
            if size:                                    # every segment the same fixed size (the game buttons)
                cell.configure(width=size[0], height=size[1])
                cell.pack_propagate(False)
                label.pack(fill='both', expand=True)
            else:
                label.pack()
            label.bind('<ButtonRelease-1>', lambda e, v=value: self.pick(v))
            cell.pack(side='left', padx=(0, gap))
            self.segments.append(Segment(self, value, cell, label))
        variable.trace_add('write', lambda *_: self._paint())
        self._paint()

    def pick(self, value):
        if self.state == 'normal':
            self.command(value)

    def configure(self, state=None):
        if state is not None:
            self.state = state
            self._paint()

    def _paint(self):
        if not self.frame.winfo_exists():
            return
        for segment in self.segments:
            chosen = segment.value == self.var.get()
            if chosen:
                fill, text = ACCENT, CHOSEN_TEXT
            else:
                fill, text = (BUTTON, TEXT) if self.state == 'normal' else (BUTTON_OFF, BUTTON_OFF_TEXT)
            segment.label.configure(background=fill, foreground=text, cursor='hand2' if self.state == 'normal' else '')
            segment.cell.configure(background=fill)


class Toggle:
    def __init__(self, parent, text, variable, command, font=None, state='normal'):
        self.var, self.command, self._state = variable, command, state
        self.frame = tk.Frame(parent, class_='Toggle')
        self.frame.toggle = self                        # tests and views find the switch from its frame
        _focusable(self.frame, self.invoke)
        self.track = tk.Canvas(self.frame, width=TRACK[0], height=TRACK[1], background=PANEL, highlightthickness=0)
        self.track.pack(side='left')
        self.label = tk.Label(self.frame, text=text, anchor='w', **({'font': font} if font else {}))
        self.label.pack(side='left', padx=(LABEL_GAP, 0))
        for widget in (self.frame, self.track, self.label):
            widget.bind('<ButtonRelease-1>', self._release)
        variable.trace_add('write', lambda *_: self._paint())
        self._paint()

    def __getattr__(self, name):
        """Geometry and winfo calls are the frame's."""
        if name == 'frame':
            raise AttributeError(name)
        return getattr(self.frame, name)

    def cget(self, key):
        return self._state if key == 'state' else self.label.cget(key)

    def configure(self, state=None):
        if state is not None:
            self._state = state
            self._paint()

    def invoke(self):
        if self._state == 'normal':
            self.var.set(not self.var.get())
            self.command()

    def _release(self, event):
        if 0 <= event.x < event.widget.winfo_width() and 0 <= event.y < event.widget.winfo_height():
            self.invoke()

    def _paint(self):
        if not self.frame.winfo_exists():
            return
        on, normal = bool(self.var.get()), self._state == 'normal'
        fill = (ACCENT if on else TOGGLE_OFF) if normal else LINE        # LINE: still seen on the panel
        knob = CHOSEN_TEXT if normal else BUTTON_OFF_TEXT
        w, h = TRACK
        d = h - 2 * KNOB_GAP
        x = w - KNOB_GAP - d if on else KNOB_GAP
        self.track.delete('all')
        self.track.create_oval(0, 0, h, h, fill=fill, outline=fill, tags='pill')
        self.track.create_oval(w - h, 0, w, h, fill=fill, outline=fill, tags='pill')
        self.track.create_rectangle(h / 2, 0, w - h / 2, h, fill=fill, outline=fill, tags='pill')
        self.track.create_oval(x, KNOB_GAP, x + d, KNOB_GAP + d, fill=knob, outline=knob, tags='knob')
        self.label.configure(foreground=TEXT if normal else MUTED, cursor='hand2' if normal else '')
        self.track.configure(cursor='hand2' if normal else '')
