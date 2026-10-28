"""The window's frame (S4.4): the form zone on the left and the picture zone on the right, the tab bar, a scrolling
area, and the native file dialogs. A view: no rule.

The form keeps FORM_W when the window grows; only the picture zone stretches. The tabs are Build, Settings and Mods; the
last two only say what comes there later (E5)."""
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, ttk

from tools.builder.gui_style import ACCENT, FORM_W, LINE, MIN_WINDOW, MUTED, TEXT, WINDOW, WRAP
from tools.builder.strings import text

IMAGE_TYPES = ('.png', '.jpg', '.jpeg', '.heic', '.gif', '.tif', '.tiff')     # background.SUFFIXES (a test)
TABS = ('build', 'settings', 'mods', 'controls')


def zones(root):
    """The window's size and its two zones: (form, the column the picture zone goes in)."""
    root.geometry('{}x{}'.format(*WINDOW))
    root.minsize(*MIN_WINDOW)
    root.columnconfigure(1, weight=1)
    root.rowconfigure(0, weight=1)
    form = tk.Frame(root, width=FORM_W)
    form.grid(row=0, column=0, sticky='ns')
    form.pack_propagate(False)
    return form, 1


class TabBar:
    """Build | Settings | Mods | Controls, clicked or reached with Tab and Space; the chosen one underlined."""

    def __init__(self, parent, font, on_change):
        self.on_change, self.current, self.tabs = on_change, None, {}
        self.frame = tk.Frame(parent)
        for name in TABS:
            cell = tk.Frame(self.frame)
            cell.pack(side='left', padx=(0, 22))
            label = tk.Label(cell, text=text(f'gui.tab.{name}'), font=font, takefocus=1, cursor='hand2')
            label.pack()
            line = tk.Frame(cell, height=2, background=LINE)
            line.pack(fill='x', pady=(4, 0))
            for sequence in ('<Button-1>', '<space>', '<Return>'):
                label.bind(sequence, lambda e, n=name: self.select(n))
            self.tabs[name] = (label, line)
        tk.Frame(self.frame, height=1, background=LINE).pack(side='bottom', fill='x')

    def select(self, name):
        self.current = name
        for n, (label, line) in self.tabs.items():
            label.configure(foreground=TEXT if n == name else MUTED)
            line.configure(background=ACCENT if n == name else LINE)
        self.on_change(name)


def soon(parent, key):
    """A tab's placeholder: one line saying what comes there later."""
    frame = tk.Frame(parent)
    tk.Label(frame, text=text(key), foreground=MUTED, wraplength=WRAP, justify='left').pack(anchor='w', pady=(18, 0))
    return frame


TOUCHPAD = """if {%# %% 5 == 0} {
    lassign [tk::PreciseScrollDeltas %D] tk::Priv(deltaX) tk::Priv(deltaY)
    if {$tk::Priv(deltaY) != 0} {CANVAS yview scroll [expr {-$tk::Priv(deltaY)}] units}
}"""


class ScrollArea:
    """A frame (inner) that scrolls when its content is taller than the space it gets (S4.4b, B1).

    The wheel and the trackpad use Tk 9.1's own handlers, in units of yscrollincrement, through a bind tag put in front
    of the class tag of every widget inside (the build log's Text keeps its own scrolling). The plain wheel scrolls the
    body and stops there (an Entry's own vertical wheel does nothing useful); Shift+wheel and the trackpad's sideways
    part go on to an Entry's class binding, so long paths still scroll sideways (C1). sync() brings the scroll region
    and the tags up to date; the view calls it after adding or removing rows and after a panel switch (C2). A focused
    field that is not fully visible is brought into view by the least scroll (C4)."""

    def __init__(self, parent):
        self.frame = tk.Frame(parent)
        self.canvas = tk.Canvas(self.frame, height=40, yscrollincrement=20)
        self.bar = ttk.Scrollbar(self.frame, orient='vertical', command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.bar.pack(side='right', fill='y')
        self.canvas.pack(side='left', fill='both', expand=True)
        self.inner = tk.Frame(self.canvas)
        self._window = self.canvas.create_window(0, 0, window=self.inner, anchor='nw')
        self.tag, canvas = f'BuilderWheel{id(self)}', str(self.canvas)
        self.canvas.bind_class(self.tag, '<MouseWheel>', f'tk::MouseWheel {canvas} y %D -40.0 units; break')
        for sequence in ('<Shift-MouseWheel>', '<Shift-Mod2-MouseWheel>'):
            self.canvas.bind_class(self.tag, sequence, 'list')         # nothing here, and no break: the class runs
        self.canvas.bind_class(self.tag, '<TouchpadScroll>', TOUCHPAD.replace('CANVAS', canvas))
        self.canvas.bind_class(self.tag, '<FocusIn>', lambda e: self.reveal(e.widget))
        self.inner.bind('<Configure>', lambda e: self._region())       # a backup; sync() is the one relied on
        self.canvas.bind('<Configure>', lambda e: self.canvas.itemconfigure(self._window, width=e.width))
        self._tag_all()

    def region_height(self):
        return max(1, self.inner.winfo_reqheight())

    def _region(self):
        self.canvas.configure(scrollregion=(0, 0, self.canvas.winfo_width(), self.region_height()))
        self._tag_all()

    def _tag_all(self):
        pending = [self.canvas]
        while pending:
            widget = pending.pop()
            pending += widget.winfo_children()
            tags = widget.bindtags()
            if widget.winfo_class() != 'Text' and self.tag not in tags:
                widget.bindtags((tags[0], self.tag) + tags[1:])

    def sync(self):
        self.canvas.update_idletasks()
        self._region()

    def inside(self, widget):
        while widget is not None and widget is not self.inner:
            widget = widget.master
        return widget is self.inner

    def offset(self, widget):
        """The widget's top inside the scrolled frame."""
        y = 0
        while widget is not self.inner:
            y, widget = y + widget.winfo_y(), widget.master
        return y

    def see(self, widget):
        """Scroll so the widget's top is at the view's top (as far as the region allows)."""
        self.sync()
        self.canvas.yview_moveto(self.offset(widget) / self.region_height())

    def reveal(self, widget):
        """C4: only when the widget is not fully visible, and by the least scroll."""
        if not self.inside(widget):
            return
        self.sync()
        height = self.region_height()
        top, bottom = (f * height for f in self.canvas.yview())
        y = self.offset(widget)
        end = y + widget.winfo_height()
        if y < top:
            self.canvas.yview_moveto(y / height)
        elif end > bottom:                              # its bottom into view, but never past its top
            self.canvas.yview_moveto(min(y, end - (bottom - top)) / height)

    def top(self):
        self.canvas.yview_moveto(0)


def dialog_chooser(root):
    """choose(kind, current) with the native dialogs; a cancelled dialog gives None (c3: per-kind start)."""
    def choose(kind, current):
        if kind == 'folder':
            chosen = filedialog.askdirectory(parent=root, **({'initialdir': current} if current else {}))
        elif kind == 'image':                   # S4.3
            chosen = filedialog.askopenfilename(parent=root, filetypes=[(text('gui.background.kinds'),
                                                                         ' '.join('*' + s for s in IMAGE_TYPES))])
        elif kind == 'archive':                 # S5.4 C2, S5.5 C2
            chosen = filedialog.askopenfilename(parent=root, filetypes=[(text('gui.mods.zip_kind'),
                                                                         '*.zip *.7z *.rar *.tkcl *.bnp')])
        elif kind == 'save':
            start = {'initialdir': str(Path(current).parent), 'initialfile': Path(current).name} if current else {}
            chosen = filedialog.asksaveasfilename(parent=root, defaultextension='.app', **start)
        else:
            chosen = filedialog.askopenfilename(parent=root, **({'initialdir': str(Path(current).parent)}
                                                                 if current else {}))
        return chosen or None
    return choose
