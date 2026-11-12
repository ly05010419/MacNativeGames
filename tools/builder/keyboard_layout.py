"""The keyboard layout a game's keyboard.txt describes, and the names the key test shows.

The game's runtime reads ``keyboard.txt`` beside ``controls.txt`` (docs/controls.md) and, for every
action it writes there, accepts one or two Mac keys; an action it does not list keeps the
runtime's default binding (games/totk/runtime/keyboard_map.h, keymap_default). The Controls
page's key test needs the same binding table the test result is compared with, so this module
mirrors that default table and parses the file the same way the runtime does: comments, ``none``
for an empty slot, conflicting explicit inputs rejected, and a bad line
leaving the whole file ignored with the problem reported.

Like the other models in tools/builder this module has no window and no process: MNG Launcher's Controls page
(tools/builder_jobs/tab_controls.py) and the button icons script call it, and nothing here touches a game.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# The 25 actions, in the order the runtime's header lists them (games/totk/runtime/keyboard_map.h); the last, the
# screenshot, is no Switch button (P takes a picture of the game).
ACTIONS = (
    'a', 'b', 'x', 'y', 'lstick', 'rstick', 'l', 'r', 'zl', 'zr', 'plus', 'minus',
    'left', 'up', 'right', 'down',
    'move_up', 'move_down', 'move_left', 'move_right',
    'camera_up', 'camera_down', 'camera_left', 'camera_right',
    'screenshot',
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
    'screenshot': 'Screenshot',
}

# Every input name keyboard.txt accepts, with what a person calls it. The Controls page's key test maps a key code
# into this vocabulary (tools/builder_jobs/tab_controls.key_codes, from the runtime's keyboard_map.h).
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
    'screenshot': ('p',),
}

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
    order = []
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
        if action in order:
            problems.append(f'line {number}: this action has a line already')
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
            if lowered == '1' or lowered in bound or any(lowered in bindings[a] for a in order):
                problems.append(f'line {number}: reserved or duplicate input "{name}"')
                break
            bound.append(lowered)
        else:
            bindings[action] = bound
            order.append(action)
    if problems:
        return Layout({action: DEFAULT[action] for action in ACTIONS}, tuple(problems), source)
    # Explicit bindings take their inputs away from omitted defaults. Conflicts
    # between explicit lines were rejected above, exactly as keymap_parse does.
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
