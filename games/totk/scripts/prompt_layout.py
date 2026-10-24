"""Which controller button the player presses for each Switch button: the layout that on-screen prompts must show.

Game-neutral. The game draws prompts for the Switch's buttons (A, B, X, Y, L, R, ZL, ZR, +, -, stick presses). A mod that
redraws them as Xbox or PlayStation buttons must name the button the player really presses for each one, so it follows the
same chain the runtime uses (runtime/gamepad.m, runtime/gamepad_layout.h, runtime/controller_map.h):

  controller button (Xbox A/B/X/Y) -> position (by position: A south, B east, X west, Y north; by label: A east, B south,
  X north, Y west; --face-buttons, TOTK_FACE_BUTTONS, key 6) -> controls.txt over the default layout -> Switch button.

The layout file is the same controls.txt the launcher checks. Button names are read from runtime/controller_map.h, so there is
no second name table; tests/test_prompt_layout.py compares this parser with the C one (tests/controls_dump.c). Modes
(hold, tap, toggle, aim) do not change which button a prompt names and are only checked for syntax.
"""
import re
from pathlib import Path

HEADER = Path(__file__).resolve().parents[1] / 'runtime/controller_map.h'
PRESSABLE = 16   # physical buttons 0-15; 16 and up are the touchpad and the mouse, which no prompt names
# The Switch buttons a prompt can show, in the order of the controller_map.h switch names (bit i = FullKey bit i).
SLOTS = ('a', 'b', 'x', 'y', 'lstick', 'rstick', 'l', 'r', 'zl', 'zr', 'plus', 'minus')
FACE_MODES = ('position', 'label')
# Controller face buttons (GameController buttonA, buttonB, buttonX, buttonY) -> the physical position, by position or by label
# (gamepad_face_positions in runtime/gamepad_layout.h: bits 0 south, 1 east, 2 west, 3 north).
PAD_FACES = {'position': {'a': 'south', 'b': 'east', 'x': 'west', 'y': 'north'},
             'label': {'a': 'east', 'b': 'south', 'x': 'north', 'y': 'west'}}


class LayoutError(ValueError):
    def __init__(self, line, problem):
        super().__init__(f'line {line}: {problem}')
        self.line, self.problem = line, problem


def _names(array):
    text = HEADER.read_text()
    block = re.search(array + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    if not block:
        raise RuntimeError(f'{HEADER} has no {array}')
    return re.findall(r'"([^"]*)"', block.group(1))


PHYSICAL = _names('controller_physical_names')   # south east west north l3 r3 l1 r1 l2 r2 menu view left up right down touchpad mouse_*
SWITCH = _names('controller_switch_names')        # a b x y lstick rstick l r zl zr plus minus left up right down
MODES = ('hold', 'tap', 'toggle', 'aim')


def default_layout():
    """controller_map_default: the Switch's layout by position, the mouse buttons attack / draw the bow / target.
    Returns (map, modes): per physical button the Switch bit mask and the mode name; a mode 'aim' carries (fire index,)."""
    out = [1 << i if i < len(SWITCH) else 0 for i in range(len(PHYSICAL))]
    out[PHYSICAL.index('south')], out[PHYSICAL.index('east')] = 1 << SWITCH.index('b'), 1 << SWITCH.index('a')
    out[PHYSICAL.index('west')], out[PHYSICAL.index('north')] = 1 << SWITCH.index('y'), 1 << SWITCH.index('x')
    out[PHYSICAL.index('mouse_left')] = 1 << SWITCH.index('y')
    out[PHYSICAL.index('mouse_right')] = 1 << SWITCH.index('zr')
    out[PHYSICAL.index('mouse_middle')] = 1 << SWITCH.index('zl')
    modes = [('hold', None)] * len(PHYSICAL)
    modes[PHYSICAL.index('mouse_right')] = ('aim', PHYSICAL.index('mouse_left'))
    return out, modes


def parse_controls(text):
    """The layout a controls.txt text gives: (map, modes, written) like default_layout, plus the set of physical buttons the
    text names. Raises LayoutError(line, problem) for the first bad line, as controller_map_parse does."""
    mapping, modes = default_layout()
    written = set()
    for number, raw in enumerate(text.split('\n'), 1):
        line = raw.split('#', 1)[0].strip(' \t\r')
        if not line:
            continue
        if '=' not in line:
            raise LayoutError(number, 'expected "button = Switch button [hold|tap|toggle]"')
        key, value = line.split('=', 1)
        words = [w for w in re.split(r'[ \t]+', value.strip(' \t')) if w]
        name = key.strip(' \t').lower()
        if name not in PHYSICAL:
            raise LayoutError(number, 'unknown controller button')
        physical = PHYSICAL.index(name)
        target = words[0].lower() if words else ''
        if target != 'none' and target not in SWITCH:
            raise LayoutError(number, 'unknown Switch button')
        how = ('hold', None)
        rest = [w.lower() for w in words[1:]]
        if rest:
            if rest[0] not in MODES:
                raise LayoutError(number, 'unknown mode (hold tap toggle aim)')
            how = (rest[0], None)
            extra = rest[1:]
            if rest[0] == 'aim':
                fire = PHYSICAL.index(extra[0]) if extra and extra[0] in PHYSICAL else -1
                if fire < 0:
                    raise LayoutError(number, 'aim needs a fire button')
                if fire == physical:
                    raise LayoutError(number, 'the fire button must be another button')
                how = ('aim', fire)
                extra = extra[1:]
            if extra:
                raise LayoutError(number, 'too many words after the Switch button')
            if target == 'none' and how[0] != 'hold':
                raise LayoutError(number, 'none takes no mode')
        mapping[physical] = 0 if target == 'none' else 1 << SWITCH.index(target)
        modes[physical] = how
        written.add(physical)
    return mapping, modes, written


def pad_positions(face_buttons):
    """Controller face button ('a' 'b' 'x' 'y') -> physical button index, for 'position' or 'label' face mapping."""
    if face_buttons not in FACE_MODES:
        raise ValueError(f'face buttons: {FACE_MODES}, not {face_buttons!r}')
    return {pad: PHYSICAL.index(place) for pad, place in PAD_FACES[face_buttons].items()}


def pad_switch_masks(controls_text, face_buttons):
    """The Switch buttons (bit mask) each controller face button drives: what tests/controls_dump.c prints as `pad`."""
    mapping, _, _ = parse_controls(controls_text or '')
    return {pad: mapping[index] for pad, index in pad_positions(face_buttons).items()}


def prompt_controls(controls_text=None, face_buttons='position'):
    """For each Switch button in SLOTS, the controller button the player presses for it, or None when nothing drives it (the
    prompt then keeps the Switch icon).

    A controller button is 'a' 'b' 'x' 'y' (Xbox names; PlayStation Cross Circle Square Triangle sit in the same places) or a
    controller_map.h physical name ('l1' 'r1' 'l2' 'r2' 'l3' 'r3' 'menu' 'view' 'left' ...). Several buttons may drive one
    Switch button: the touchpad and the mouse are skipped, a button the file names beats one that keeps its default, and the
    lowest button in controller_map.h order wins."""
    mapping, _, written = parse_controls(controls_text or '')
    by_physical = {index: pad for pad, index in pad_positions(face_buttons).items()}
    result = {}
    for slot in SLOTS:
        bit = 1 << SWITCH.index(slot)
        drivers = [p for p in range(PRESSABLE) if mapping[p] & bit]
        drivers.sort(key=lambda p: (p not in written, p))
        result[slot] = (by_physical.get(drivers[0], PHYSICAL[drivers[0]]) if drivers else None)
    return result


SWITCH_LABEL = {'lstick': 'left stick press', 'rstick': 'right stick press', 'plus': '+', 'minus': '-'}


def describe(controls_text, face_buttons, style='xbox', slots=None):
    """Lines like `Switch A <- Xbox X` for the user to check before a mod is built (only the Switch buttons in `slots`, if given)."""
    faces = {'xbox': {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y'},
             'playstation': {'a': 'Cross', 'b': 'Circle', 'x': 'Square', 'y': 'Triangle'},
             'switch': {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y'}}[style]
    other = {'l1': 'LB', 'r1': 'RB', 'l2': 'LT', 'r2': 'RT', 'l3': 'LS', 'r3': 'RS', 'menu': 'Menu', 'view': 'View'}
    if style == 'playstation':
        other = {'l1': 'L1', 'r1': 'R1', 'l2': 'L2', 'r2': 'R2', 'l3': 'L3', 'r3': 'R3', 'menu': 'Options', 'view': 'Create'}
    if style == 'switch':
        other = {'l1': 'L', 'r1': 'R', 'l2': 'ZL', 'r2': 'ZR', 'l3': 'L stick press', 'r3': 'R stick press', 'menu': '+', 'view': '-'}
    brand = {'xbox': 'Xbox', 'playstation': 'PlayStation', 'switch': 'Switch'}[style]
    lines = []
    for slot, pad in prompt_controls(controls_text, face_buttons).items():
        if slots is not None and slot not in slots:
            continue
        shown = 'unchanged (nothing drives it)' if pad is None else faces.get(pad, other.get(pad, pad))
        lines.append(f'Switch {SWITCH_LABEL.get(slot, slot.upper())} <- ' + (shown if pad is None else f'{brand} {shown}'))
    return lines

