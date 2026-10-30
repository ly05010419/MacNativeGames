"""The Controls tab's table (E6 S6.2): the game's inputs with their names, and the labels of an Xbox controller's
buttons. A view's helper (texts and order); the layout itself is controls_file's.

The layout: actions on the left (one row per Switch input, named after its main use in the game; an input whose use is
not confirmed yet is shown as "<button> - to confirm"), a drawn Xbox controller on the right whose every button can be
clicked. The names are runtime/controller_map.h's (controller_switch_names, controller_physical_names;
tests/test_builder_gui_controls.py reads the header). The drawn buttons are named by where they sit on an Xbox
controller (south is the bottom one). E9 S9.1 C2: one table of names per game, every one in the same order of Switch
inputs; Breath of the Wild's names are provisional until they are confirmed in play."""

# The Switch inputs in the order the tab lists them.
ORDER = ('a', 'b', 'x', 'y', 'zl', 'zr', 'l', 'r', 'minus', 'plus', 'lstick', 'rstick', 'up', 'down', 'left', 'right')
# Per game: each input's name for its main use, or None until confirmed (the D-pad's four stay separate rows).
NAMES = {
    'totk': dict(zip(ORDER, ('Action', 'Put Away', 'Jump', 'Attack', 'Focus', None, None, 'Throw Weapon',
                             'Purah Pad', 'Pause Menu', 'Crouch', None, None, None, None, None))),
    'botw': dict(zip(ORDER, ('Action', 'Cancel / Sprint', 'Jump', 'Attack', 'Target', 'Bow', 'Rune',
                             'Throw Weapon', 'Map', 'Pause Menu', 'Crouch', 'Look', None, None, None, None))),
}

SWITCH_LABELS = {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y', 'zl': 'ZL', 'zr': 'ZR', 'l': 'L', 'r': 'R', 'minus': '−',
                 'plus': '+', 'lstick': 'Left stick press', 'rstick': 'Right stick press', 'up': 'D-pad up',
                 'down': 'D-pad down', 'left': 'D-pad left', 'right': 'D-pad right'}

# The physical buttons (controller_physical_names, without the touchpad and the mouse) and their Xbox labels.
PHYSICAL = (('south', 'A'), ('east', 'B'), ('west', 'X'), ('north', 'Y'), ('l1', 'LB'), ('r1', 'RB'), ('l2', 'LT'),
            ('r2', 'RT'), ('l3', 'L3'), ('r3', 'R3'), ('menu', 'Menu'), ('view', 'View'), ('up', 'D-pad up'),
            ('down', 'D-pad down'), ('left', 'D-pad left'), ('right', 'D-pad right'))
XBOX = dict(PHYSICAL)

def button_for(mapping, switch_input):
    """The physical button that drives switch_input in mapping (physical -> input), or None."""
    return next((p for p, s in mapping.items() if s == switch_input), None)


def input_name(game, switch_input):
    """(the row's name, confirmed): 'Jump', or 'ZR' for an input whose use is not confirmed."""
    name = NAMES[game][switch_input]
    return (name, True) if name else (SWITCH_LABELS[switch_input], False)
