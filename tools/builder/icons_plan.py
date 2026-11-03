"""Which button each of the game's prompt icons names, from the player's layout (E6 S6.3, C1 and B4). The model: no
Tk.

The game's button icons are glyphs of External.bfotf (S6.1); make_button_icons.py redraws 34 code points. plan(inputs)
gives, for each of them that names a button or the d-pad, what to draw: the drawn Xbox button that drives that Switch
input in the layout (controls_file.layout's inputs, drawn button -> input): 'face' with a letter (A B X Y, and L3 R3 for
the stick presses), 'bumper' (LB RB), 'trigger' (LT RT), 'menu', 'view', 'dpad' with its lit arms, or 'none' (a crossed
circle) when no button drives it. An input two buttons drive names the first in the drawing's order. The d-pad's
combined glyphs (up+down, left+right, all four) light the real arms when every input they name is on a d-pad arm, and
otherwise show those buttons' labels side by side ('labels'). The stick movement glyphs (E050-E05F, E068) are left as
they are, and so are E048, E04A, E04D-E04F, E066, E067, which main does not redraw."""
from tools.builder.controls_file import DRAWN

SINGLE = {0xE040: 'a', 0xE041: 'b', 0xE042: 'x', 0xE043: 'y', 0xE044: 'l', 0xE045: 'r', 0xE046: 'zl', 0xE047: 'zr',
          0xE04B: 'plus', 0xE04C: 'minus', 0xE056: 'lstick', 0xE057: 'rstick', 0xE060: 'up', 0xE061: 'down',
          0xE062: 'left', 0xE063: 'right'}
COMBINED = {0xE064: ('up', 'down'), 0xE065: ('left', 'right'), 0xE049: ('up', 'down', 'left', 'right')}
ARMS = ('up', 'down', 'left', 'right')
FACES = {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'l3': 'L3', 'r3': 'R3'}
SHORT = {'l1': 'LB', 'r1': 'RB', 'l2': 'LT', 'r2': 'RT', 'menu': 'Menu', 'view': 'View', 'up': '↑', 'down': '↓',
         'left': '←', 'right': '→', None: '-', **FACES}


def driver(inputs, switch_input):
    """The first drawn button (in the drawing's order) that drives switch_input, or None."""
    return next((d for d in DRAWN if inputs.get(d) == switch_input), None)


def shape(drawn):
    if drawn in FACES:
        return ['face', FACES[drawn]]
    if drawn in ('l1', 'r1'):
        return ['bumper', SHORT[drawn]]
    if drawn in ('l2', 'r2'):
        return ['trigger', SHORT[drawn]]
    if drawn in ('menu', 'view'):
        return [drawn]
    if drawn in ARMS:
        return ['dpad', [drawn]]
    return ['none']


def plan(inputs, single=SINGLE, combined=COMBINED):
    """[[code point, kind, *args]] for every glyph to redraw, in code point order."""
    out = {cp: shape(driver(inputs, s)) for cp, s in single.items()}
    for cp, names in combined.items():
        drawn = [driver(inputs, s) for s in names]
        out[cp] = ['dpad', drawn] if all(d in ARMS for d in drawn) else ['labels', [SHORT[d] for d in drawn]]
    return [[cp, *out[cp]] for cp in sorted(out)]


# Breath of the Wild 1.9.0 (make_botw_button_icons.py): External_00.bffnt has its own code points past U+E048, and the
# font already holds an icon for each button it can name, so a glyph is not drawn but shown in another code point's place:
# LB RB LT RT as L R ZL ZR, Menu and View as + and -, a cross (U+E070) for an input no button drives.
BOTW_SINGLE = {0xE040: 'a', 0xE041: 'b', 0xE042: 'x', 0xE043: 'y', 0xE044: 'l', 0xE045: 'r', 0xE046: 'zl', 0xE047: 'zr',
               0xE04B: 'plus', 0xE04C: 'minus', 0xE053: 'lstick', 0xE054: 'rstick', 0xE05C: 'up', 0xE05D: 'down',
               0xE05E: 'left', 0xE05F: 'right'}
BOTW_COMBINED = {0xE060: ('up', 'down'), 0xE061: ('left', 'right'), 0xE049: ARMS}
BOTW_SHOWN = {('face', 'A'): 0xE040, ('face', 'B'): 0xE041, ('face', 'X'): 0xE042, ('face', 'Y'): 0xE043,
              ('face', 'L3'): 0xE053, ('face', 'R3'): 0xE054, ('bumper', 'LB'): 0xE044, ('bumper', 'RB'): 0xE045,
              ('trigger', 'LT'): 0xE046, ('trigger', 'RT'): 0xE047, ('menu',): 0xE04B, ('view',): 0xE04C, ('none',): 0xE070}
BOTW_ARMS = {('up',): 0xE05C, ('down',): 0xE05D, ('left',): 0xE05E, ('right',): 0xE05F, ('down', 'up'): 0xE060,
             ('left', 'right'): 0xE061, ('down', 'left', 'right', 'up'): 0xE049}


BOTW_SLOTS = {'south': 'bottom', 'east': 'right', 'west': 'left', 'north': 'top'}


def botw_diamond(inputs):
    """{Switch face input: the place of the face button that drives it} for the action guide's diamond; an input on
    another button, or on none, is left out, and its part keeps the Switch's drawing."""
    return {s: BOTW_SLOTS[d] for s in 'abxy' if (d := driver(inputs, s)) in BOTW_SLOTS}


def botw_glyphs(inputs):
    """{code point: code point whose icon it shows} for Breath of the Wild, the code points that change only. A combined
    d-pad icon whose inputs sit on two arms the font has no icon for (up and right) shows all four arms; one whose inputs
    are not all on the d-pad keeps its own icon."""
    out = {}
    for cp, kind, *args in plan(inputs, BOTW_SINGLE, BOTW_COMBINED):
        if kind == 'dpad':
            shown = BOTW_ARMS.get(tuple(sorted(set(args[0]))), 0xE049)
        elif kind != 'labels':
            shown = BOTW_SHOWN.get((kind, *args))
        else:
            shown = None
        if shown is not None and shown != cp:
            out[cp] = shown
    return out


def shared(inputs):
    """The Switch inputs (of the redrawn glyphs) that two or more drawn buttons drive (the tab says so)."""
    names = set(SINGLE.values())
    return sorted(s for s in names if sum(1 for d in DRAWN if inputs.get(d) == s) > 1)
