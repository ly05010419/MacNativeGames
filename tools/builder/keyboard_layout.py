"""The keyboard layout a game's keyboard.txt describes, and the key names its tests show.

The game's runtime reads ``keyboard.txt`` beside ``controls.txt`` (docs/controls.md) and, for every
action it writes there, accepts one or two Mac keys; an action it does not list keeps the
runtime's default binding (games/totk/runtime/keyboard_map.h, keymap_default). The builder's
keyboard test needs the same binding table the test result is compared with, so this module
mirrors that default table and parses the file the same way the runtime does: comments, ``none``
for an empty slot, one input driving at most one action (the later line wins), and a bad line
leaving the whole file ignored with the problem reported.

Like the other pure builder models (settings.py, paths.py, checks.py) this module has no Tk and no
process: the GUI calls it, the tests call it, and nothing here touches a game.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# The 24 actions, in the order the runtime's header lists them (games/totk/runtime/keyboard_map.h).
ACTIONS = (
    'a', 'b', 'x', 'y', 'lstick', 'rstick', 'l', 'r', 'zl', 'zr', 'plus', 'minus',
    'left', 'up', 'right', 'down',
    'move_up', 'move_down', 'move_left', 'move_right',
    'camera_up', 'camera_down', 'camera_left', 'camera_right',
)

# What the pad calls each action, for the "this key is bound to ..." line. The names are the ones
# the game itself uses for its buttons, so they need no explanation popover.
TITLES = {
    'a': 'A (confirm)', 'b': 'B (cancel)', 'x': 'X (jump)', 'y': 'Y (attack)',
    'lstick': 'Left stick', 'rstick': 'Right stick', 'l': 'L', 'r': 'R',
    'zl': 'ZL', 'zr': 'ZR', 'plus': 'Plus (menu)', 'minus': 'Minus (map)',
    'left': 'D-pad left', 'up': 'D-pad up', 'right': 'D-pad right', 'down': 'D-pad down',
    'move_up': 'Move up', 'move_down': 'Move down', 'move_left': 'Move left',
    'move_right': 'Move right', 'camera_up': 'Camera up', 'camera_down': 'Camera down',
    'camera_left': 'Camera left', 'camera_right': 'Camera right',
}

# Every input name keyboard.txt accepts, with what a person calls it. A key event maps into this
# vocabulary through key_name() before it is looked up, so the two sides agree on spelling.
INPUTS = {
    **{c: c.upper() for c in 'abcdefghijklmnopqrstuvwxyz'},
    **{c: c for c in '0123456789'},
    'space': 'Space', 'return': 'Return', 'escape': 'Escape', 'tab': 'Tab',
    'backspace': 'Backspace', 'forward_delete': 'Delete',
    'shift': 'Shift', 'right_shift': 'Right Shift',
    'control': 'Control', 'right_control': 'Right Control',
    'option': 'Option', 'right_option': 'Right Option',
    'arrow_up': 'Up', 'arrow_down': 'Down', 'arrow_left': 'Left', 'arrow_right': 'Right',
    'home': 'Home', 'end': 'End', 'page_up': 'Page Up', 'page_down': 'Page Down',
    'grave': 'Grave', 'minus': 'Minus', 'equal': 'Equal',
    'left_bracket': 'Left Bracket', 'right_bracket': 'Right Bracket',
    'backslash': 'Backslash', 'semicolon': 'Semicolon', 'quote': 'Quote',
    'comma': 'Comma', 'period': 'Period', 'slash': 'Slash',
    **{f'f{n}': f'F{n}' for n in range(1, 13)},
    **{f'keypad_{n}': f'Keypad {n}' for n in range(10)},
    'keypad_divide': 'Keypad /', 'keypad_multiply': 'Keypad *',
    'keypad_minus': 'Keypad -', 'keypad_plus': 'Keypad +',
    'keypad_period': 'Keypad .', 'keypad_enter': 'Keypad Enter',
    'mouse_left': 'Left click', 'mouse_right': 'Right click', 'mouse_middle': 'Wheel click',
    'mouse4': 'Mouse 4', 'mouse5': 'Mouse 5',
}

# The runtime's default table (keymap_default in keyboard_map.h), one tuple per action. Inputs the
# header spells as numbers are spelled as names here; that is the vocabulary keyboard.txt uses.
DEFAULT = {
    'a': ('e', 'return'), 'b': ('shift', 'backspace'), 'x': ('space',), 'y': ('mouse_left', 'g'),
    'lstick': ('control', 'c'), 'rstick': ('v',), 'l': ('f',), 'r': ('r',),
    'zl': ('q', 'mouse_middle'), 'zr': ('mouse_right', 'option'),
    'plus': ('escape', 'i'), 'minus': ('tab', 'm'),
    'left': ('2',), 'up': ('3',), 'right': ('4',), 'down': ('5',),
    'move_up': ('w',), 'move_down': ('s',), 'move_left': ('a',), 'move_right': ('d',),
    'camera_up': ('arrow_up',), 'camera_down': ('arrow_down',),
    'camera_left': ('arrow_left',), 'camera_right': ('arrow_right',),
}

# Tk reports X11 keysyms; these are the ones keyboard.txt names. A key Tk spells differently under
# Shift (Shift+1 is 'exclam') is folded back to the key the layout knows.
_KEYSYMS = {
    **{c: c for c in 'abcdefghijklmnopqrstuvwxyz'},
    **{c.upper(): c for c in 'abcdefghijklmnopqrstuvwxyz'},
    **{c: c for c in '0123456789'},
    'space': 'space', 'Return': 'return', 'KP_Enter': 'keypad_enter', 'Escape': 'escape',
    'Tab': 'tab', 'ISO_Left_Tab': 'tab', 'BackSpace': 'backspace', 'Delete': 'forward_delete',
    'Shift_L': 'shift', 'Shift_R': 'right_shift',
    'Control_L': 'control', 'Control_R': 'right_control',
    'Option_L': 'option', 'Option_R': 'right_option',
    'Alt_L': 'option', 'Alt_R': 'right_option',
    'Up': 'arrow_up', 'Down': 'arrow_down', 'Left': 'arrow_left', 'Right': 'arrow_right',
    'Home': 'home', 'End': 'end', 'Prior': 'page_up', 'Next': 'page_down',
    'grave': 'grave', 'minus': 'minus', 'equal': 'equal',
    'bracketleft': 'left_bracket', 'bracketright': 'right_bracket', 'backslash': 'backslash',
    'semicolon': 'semicolon', 'apostrophe': 'quote', 'comma': 'comma', 'period': 'period',
    'slash': 'slash',
    'KP_0': 'keypad_0', 'KP_1': 'keypad_1', 'KP_2': 'keypad_2', 'KP_3': 'keypad_3',
    'KP_4': 'keypad_4', 'KP_5': 'keypad_5', 'KP_6': 'keypad_6', 'KP_7': 'keypad_7',
    'KP_8': 'keypad_8', 'KP_9': 'keypad_9',
    'KP_Decimal': 'keypad_period', 'KP_Add': 'keypad_plus', 'KP_Subtract': 'keypad_minus',
    'KP_Multiply': 'keypad_multiply', 'KP_Divide': 'keypad_divide',
    **{f'F{n}': f'f{n}' for n in range(1, 13)},
}

# Shifted punctuation arrives as its own keysym on X11; the layout names the key itself.
_SHIFTED = {
    'exclam': '1', 'at': '2', 'numbersign': '3', 'dollar': '4', 'percent': '5',
    'asciicircum': '6', 'ampersand': '7', 'asterisk': '8', 'parenleft': '9', 'parenright': '0',
    'underscore': 'minus', 'plus': 'equal', 'braceleft': 'left_bracket',
    'braceright': 'right_bracket', 'bar': 'backslash', 'colon': 'semicolon',
    'quotedbl': 'quote', 'less': 'comma', 'greater': 'period', 'question': 'slash',
    'asciitilde': 'grave',
}

# Modifier keys the layout does not bind, but the test still names. The system's own shortcuts use
# them, so the test shows them without ever claiming to act on them.
UNBOUND_MODIFIERS = {'Command_L': 'Command', 'Command_R': 'Command', 'Meta_L': 'Meta',
                     'Meta_R': 'Meta', 'Super_L': 'Super', 'Super_R': 'Super',
                     'Caps_Lock': 'Caps Lock', 'fn': 'fn'}

_LABELS = {**INPUTS, 'KP_Enter': 'Keypad Enter', 'ISO_Left_Tab': 'Tab'}
_LABELS.update(UNBOUND_MODIFIERS)


@dataclass(frozen=True)
class Layout:
    """One game's bindings: action -> the inputs that drive it, plus what was wrong with the file."""

    bindings: dict
    problems: tuple = ()
    source: Path | None = None

    @property
    def valid(self) -> bool:
        return not self.problems

    def inputs(self) -> tuple:
        seen = []
        for action in ACTIONS:
            for name in self.bindings.get(action, ()):
                if name not in seen:
                    seen.append(name)
        return tuple(seen)


def default_layout(source: Path | None = None) -> Layout:
    """The table the runtime starts from when keyboard.txt is missing or unreadable."""
    return Layout({action: DEFAULT[action] for action in ACTIONS}, (), source)


def parse_layout(text: str, source: Path | None = None) -> Layout:
    """Read the file the way the runtime does: a bad line ignores the whole file."""
    bindings = {action: list(DEFAULT[action]) for action in ACTIONS}
    order = []          # the actions a line bound, in file order: the later line wins an input
    problems = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        action, sep, rest = line.partition('=')
        action = action.strip().lower()
        if not sep:
            problems.append(f'line {number}: expected "action = input"')
            continue
        if action not in ACTIONS:
            problems.append(f'line {number}: unknown action "{action}"')
            continue
        names = rest.split()
        if not names:
            problems.append(f'line {number}: "{action}" lists no input')
            continue
        if len(names) > 2:
            problems.append(f'line {number}: "{action}" lists more than two inputs')
            continue
        if 'none' in (name.lower() for name in names) and len(names) > 1:
            problems.append(f'line {number}: none cannot be combined with an input')
            continue
        bound = []
        for name in names:
            lowered = name.lower()
            if lowered == 'none':
                continue
            if lowered not in INPUTS:
                problems.append(f'line {number}: unknown input "{name}"')
                break
            bound.append(lowered)
        else:
            bindings[action] = bound
            order.append(action)
    if problems:
        return Layout({action: DEFAULT[action] for action in ACTIONS}, tuple(problems), source)
    # An input drives at most one action: the file's later binding takes it from the earlier ones and
    # from the defaults the file did not mention.
    taken = {}
    for action in order:
        for name in bindings[action]:
            taken[name] = action
    for action in ACTIONS:
        if action in order:
            bindings[action] = [name for name in bindings[action] if taken.get(name, action) == action]
        else:
            bindings[action] = [name for name in bindings[action] if name not in taken]
    return Layout({action: tuple(names) for action, names in bindings.items()}, (), source)


def read_layout(path: Path) -> Layout:
    """The layout beside controls.txt; a missing file is the default table, as at runtime."""
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError as error:
        if not Path(path).exists():
            return default_layout(Path(path))
        return Layout({action: DEFAULT[action] for action in ACTIONS},
                      (f'could not read {path}: {error.strerror or error}',), Path(path))
    return parse_layout(text, Path(path))


def owner(layout: Layout, name: str) -> tuple:
    """The actions ``name`` is bound to. A right-hand modifier falls back to its left twin, because
    the runtime's defaults name ``shift``/``control``/``option`` and a Mac's right-hand key still
    is that modifier."""
    for candidate in (name, {'right_shift': 'shift', 'right_control': 'control',
                             'right_option': 'option'}.get(name)):
        if not candidate:
            continue
        found = tuple(action for action in ACTIONS if candidate in layout.bindings.get(action, ()))
        if found:
            return found
    return ()


def key_name(keysym: str) -> str | None:
    """The keyboard.txt input a Tk keysym means, or None for a key the layout cannot bind."""
    keysym = keysym or ''
    if keysym in _KEYSYMS:
        return _KEYSYMS[keysym]
    if keysym in _SHIFTED:
        return _SHIFTED[keysym]
    return None


def label(keysym: str) -> str:
    """What the test shows for a key: the layout's name when it has one, else the sensor's."""
    name = key_name(keysym)
    if name is not None:
        return INPUTS.get(name, name)
    if keysym in _LABELS:
        return _LABELS[keysym]
    return keysym or '?'


def modifiers(state: int, pressed: frozenset) -> tuple:
    """The modifiers a press carries, as labels. ``state`` is Tk's bit mask (Shift 0x1, Control
    0x4, Option 0x8, Command 0x10 on macOS), ``pressed`` the modifier keys still held; either can
    report a modifier the other misses, and naming it is better than hiding it."""
    found = []
    if state & 0x1 or 'Shift_L' in pressed or 'Shift_R' in pressed:
        found.append('Shift')
    if state & 0x4 or 'Control_L' in pressed or 'Control_R' in pressed:
        found.append('Control')
    if state & 0x8 and 'Option_L' not in pressed and 'Option_R' not in pressed \
            and 'Alt_L' not in pressed and 'Alt_R' not in pressed:
        found.append('Command')         # macOS reports Option as 0x10 too; a bare 0x8 is Command
    elif state & 0x10 or {'Option_L', 'Option_R', 'Alt_L', 'Alt_R'} & pressed:
        found.append('Option')
    return tuple(found)


def is_modifier(keysym: str) -> bool:
    return keysym in ('Shift_L', 'Shift_R', 'Control_L', 'Control_R', 'Option_L', 'Option_R',
                      'Alt_L', 'Alt_R', 'Command_L', 'Command_R', 'Meta_L', 'Meta_R',
                      'Super_L', 'Super_R', 'Caps_Lock', 'fn')
