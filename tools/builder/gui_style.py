"""The window's colours, fonts and sizes (S4.4), in one place so no view repeats them.

apply() fills Tk's option database before any widget is made, so every frame, label, entry and text box takes the
panel's look without each view passing colours. Buttons and selectors are gui_widgets' own (aqua's ignore the colours);
only the progress bar is drawn by aqua.

BUTTON_LOOKS: each state's fill and text together, so no state puts light text on a light fill: normal #2B313B/#F1F3F5,
hover #394352/#FFFFFF, pressed #232A34/#FFFFFF, disabled #252A32/#929BA8; the primary action, enabled, #3478F6/#FFFFFF.
The focus ring is its own colour, apart from hover and from chosen. A switch (S7.5b) is the accent when on and
TOGGLE_OFF when off, with a white knob; disabled, LINE with a grey knob."""
import tkinter as tk
import tkinter.font as tkfont

PANEL, FIELD, FIELD_OFF, LINE = '#20242B', '#2B313B', '#262B33', '#3A404B'
TEXT, MUTED, ACCENT, GOOD, BAD = '#F1F3F5', '#B4BCC8', '#3478F6', '#7BD88F', '#FF8A80'
ACCENT_HOVER, ACCENT_DOWN = '#4A88F7', '#2A62C8'
BUTTON, BUTTON_HOVER, BUTTON_DOWN, BUTTON_OFF, BUTTON_OFF_TEXT = '#2B313B', '#394352', '#232A34', '#252A32', '#929BA8'
FOCUS_RING = '#E6E9EE'                # a light outline, never the accent (the accent means chosen)
TOGGLE_OFF = '#4B5363'                # an off switch's track: apart from the panel, never the accent (S7.5b)
PICTURE_BG = '#15181D'
CHOSEN_TEXT = '#FFFFFF'
BUTTON_LOOKS = {'secondary': {'normal': (BUTTON, TEXT), 'hover': (BUTTON_HOVER, CHOSEN_TEXT),
                              'pressed': (BUTTON_DOWN, CHOSEN_TEXT)},
                'primary': {'normal': (ACCENT, CHOSEN_TEXT), 'hover': (ACCENT_HOVER, CHOSEN_TEXT),
                            'pressed': (ACCENT_DOWN, CHOSEN_TEXT)}}
WINDOW, MIN_WINDOW = (1100, 760), (900, 620)
FORM_W, PAD, ROW_GAP = 640, 28, 14
WRAP = FORM_W - 2 * PAD - 24        # long lines (check results, notes) wrap inside the form


def apply(root):
    """The option database and the fonts; returns the fonts, which must stay referenced while the window lives."""
    base = tkfont.nametofont('TkDefaultFont', root=root).actual()['family']
    fonts = {'title': tkfont.Font(root=root, family=base, size=16, weight='bold'),
             'section': tkfont.Font(root=root, family=base, size=16, weight='bold'),
             'text': tkfont.Font(root=root, family=base, size=14),
             'small': tkfont.Font(root=root, family=base, size=12),
             'tiny': tkfont.Font(root=root, family=base, size=10),
             'choice': tkfont.Font(root=root, family=base, size=15, weight='bold'),
             'action': tkfont.Font(root=root, family=base, size=15, weight='bold')}
    for pattern, value in (('*Background', PANEL), ('*Foreground', TEXT), ('*Font', str(fonts['text'])),
                           ('*Entry.Background', FIELD), ('*Entry.disabledBackground', FIELD_OFF),
                           ('*Entry.disabledForeground', MUTED), ('*Entry.insertBackground', TEXT),
                           ('*Entry.relief', 'flat'), ('*Entry.highlightThickness', 1),
                           ('*Entry.highlightBackground', LINE), ('*Entry.highlightColor', ACCENT),
                           ('*Text.Background', FIELD), ('*Text.relief', 'flat'), ('*Text.highlightThickness', 0),
                           ('*Canvas.highlightThickness', 0)):
        root.option_add(pattern, value)
    root.configure(background=PANEL)
    return fonts


def muted(widget, font=None):
    """A secondary line: readable light grey, never dark grey (S4.4)."""
    widget.configure(foreground=MUTED, **({'font': font} if font else {}))
    return widget


def section(parent, title, font):
    return tk.Label(parent, text=title, font=font, foreground=MUTED)
