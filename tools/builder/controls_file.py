"""controls.txt as the game reads it, and the Controls tab's edits to it (E6 S6.2b). The model: no Tk.

parse() mirrors runtime/controller_map.h's controller_map_parse on the raw bytes, as runtime/gamepad.m reads them: it
stops at the first NUL, splits lines at '\n' only, cuts a comment at '#', trims ' ', '\t' and '\r', splits words at ' '
and '\t' only, matches names in ASCII only (a non-ASCII byte never matches) and strips no BOM; the last line for a
button wins and the first bad line refuses the whole file with the runtime's own words. app/launcher.m refuses 16 KB or
more and text that is not UTF-8 (line 1), and gamepad.m 16383 bytes: parse refuses both.
tests/test_builder_controls_file.py compares parse() with the compiled header. The tab draws 16 buttons
(controls_table.PHYSICAL): their file names are the positions, except that by label (controller-type.txt 'switch', or
'auto' with a Nintendo controller) the face buttons' names move: the bottom one is 'east', the right 'south', the left
'north', the top 'west' (runtime/gamepad_layout.h gamepad_face_positions). A drawn button is custom when its line has a
mode other than hold, is 'none', is either end of an 'aim', or drives an input another of the 16 also drives; render()
never writes a line for a custom button the player did not convert."""
from dataclasses import dataclass
import json
from pathlib import Path

PHYSICAL = ('south', 'east', 'west', 'north', 'l3', 'r3', 'l1', 'r1', 'l2', 'r2', 'menu', 'view', 'left', 'up', 'right',
            'down', 'touchpad', 'mouse_left', 'mouse_right', 'mouse_middle', 'mouse4', 'mouse5')
SWITCH = ('a', 'b', 'x', 'y', 'lstick', 'rstick', 'l', 'r', 'zl', 'zr', 'plus', 'minus', 'left', 'up', 'right', 'down')
DRAWN = PHYSICAL[:16]
NONE = len(SWITCH)
HOLD, TAP, TOGGLE, AIM = 0, 1, 2, 3
MODES = {b'hold': HOLD, b'tap': TAP, b'toggle': TOGGLE, b'aim': AIM}
MAX_BYTES = 16383                   # the launcher refuses 16384 or more; gamepad.m's fread of 16383 into its 16384-byte
                                    # buffer does not reach EOF on a 16383-byte file and stops the game: 16382 at most
LABEL_NAMES = {'south': 'east', 'east': 'south', 'west': 'north', 'north': 'west'}
TYPES = ('auto', 'xbox', 'playstation', 'switch')
HEADER, END = b'# Controls tab (MacNativeGames builder)', b'# end of the Controls tab block'
ERRORS = {
    'syntax': 'expected "button = Switch button [hold|tap|toggle]"',
    'physical': 'unknown controller button (south east west north l1 r1 l2 r2 l3 r3 menu view up down left right '
                'touchpad mouse_left mouse_right mouse_middle mouse4 mouse5)',
    'switch': 'unknown Switch button (a b x y l r zl zr plus minus lstick rstick up down left right none)',
    'mode': 'unknown mode (hold tap toggle aim)', 'aim': 'aim needs a fire button, e.g. "l2 = zr aim r2"',
    'fire': 'the fire button must be another button', 'words': 'too many words after the Switch button',
    'none': 'none takes no mode'}
SPACE = b' \t'


@dataclass(frozen=True)
class Parsed:
    map: tuple                      # per PHYSICAL: the Switch index (NONE for none) its last line or the default gives
    modes: tuple                    # per PHYSICAL: HOLD, TAP, TOGGLE, or AIM | fire << 3 (the runtime's mode byte)
    line: int = 0                   # the first bad line (1-based), 0 when the file is good
    error: str = ''


def default():
    targets = list(range(16)) + [NONE] * 6
    targets[0:4] = [1, 0, 3, 2]                                     # south B, east A, west Y, north X
    # the mouse's buttons drive nothing here: since PR #1210 they belong to the keyboard and mouse layout
    # (runtime/keyboard_map.h, keyboard.txt), which attacks with the left and draws the bow with the right
    modes = [HOLD] * 22
    return targets, modes


def _index(names, word):
    low = word.lower()                                              # bytes.lower(): ASCII letters only, as strncasecmp
    return next((i for i, name in enumerate(names) if name.encode() == low), -1)


def _word(seg, at):
    """(the word starting at or after at, its end): words end at ' ' or '\t' only."""
    while at < len(seg) and seg[at] in SPACE:
        at += 1
    end = at
    while end < len(seg) and seg[end] not in SPACE:
        end += 1
    return at, end


def _line(seg):
    """(physical, target, mode) of one line, None for a blank or comment line, or the error key."""
    seg = seg.split(b'#', 1)[0].strip(b' \t\r')
    if not seg:
        return None
    eq = seg.find(b'=')
    if eq < 0:
        return 'syntax'
    physical = _index(PHYSICAL, seg[:eq].rstrip(SPACE))
    if physical < 0:
        return 'physical'
    start, end = _word(seg, eq + 1)
    value = seg[start:end]
    target = NONE if value.lower() == b'none' else _index(SWITCH, value)
    if target < 0:
        return 'switch'
    how = HOLD
    mode_start, mode_end = _word(seg, end)
    if mode_start < len(seg):
        how = MODES.get(seg[mode_start:mode_end].lower())
        if how is None:
            return 'mode'
        extra = _word(seg, mode_end)[0]
        if how == AIM:
            fire_start, fire_end = _word(seg, mode_end)
            fire = _index(PHYSICAL, seg[fire_start:fire_end]) if fire_start < len(seg) else -1
            if fire < 0:
                return 'aim'
            if fire == physical:
                return 'fire'
            how, extra = AIM | fire << 3, _word(seg, fire_end)[0]
        if extra < len(seg):
            return 'words'
        if target == NONE and how != HOLD:
            return 'none'
    return physical, target, how


def lines(data):
    """The lines controller_map_parse reads: before the first NUL, split at '\n', no last empty one."""
    segs = data.split(b'\0', 1)[0].split(b'\n')
    return segs[:-1] if segs[-1] == b'' else segs


def parse(data):
    targets, modes = default()
    if len(data) >= MAX_BYTES:
        return Parsed(tuple(targets), tuple(modes), 1, 'larger than 16 KB')
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return Parsed(tuple(targets), tuple(modes), 1, 'not UTF-8 text')
    new_targets, new_modes = list(targets), list(modes)
    for number, seg in enumerate(lines(data), 1):
        found = _line(seg)
        if isinstance(found, str):
            return Parsed(tuple(targets), tuple(modes), number, ERRORS[found])
        if found:
            physical, target, how = found
            new_targets[physical], new_modes[physical] = target, how
    return Parsed(tuple(new_targets), tuple(new_modes))


def read_type(data):
    """controller-type.txt's word as gamepad_type_parse reads it: C isspace trimmed, exact case; unknown = 'auto'."""
    word = (data or b'').strip(b' \t\n\v\f\r')
    return next((t for t in TYPES if word == t.encode()), 'auto')


ANSWERS = ('xbox', 'nintendo')


def read_answers(store):
    """E9 S9.1 C3: {game id: the controller the player said they use with 'auto'} from Builder/controls-tab.json.
    Format 2 keeps one answer per game; a format-1 file's single answer is Tears of the Kingdom's (the only game the
    tab had then)."""
    try:
        data = json.loads(Path(store).read_text())
        found = data['controllers'] if data.get('format') == 2 else {'totk': data.get('controller')}
        return {game: value for game, value in found.items() if value in ANSWERS}
    except (OSError, ValueError, AttributeError, KeyError, TypeError):
        return {}


def read_answer(store, game):
    return read_answers(store).get(game)


def file_name(drawn, by_label):
    return LABEL_NAMES.get(drawn, drawn) if by_label else drawn


def layout(parsed, by_label):
    """(drawn button -> Switch input or None, drawn button -> why it is custom) for the 16 drawn buttons."""
    inputs, custom, users = {}, {}, {}
    for drawn in DRAWN:
        i = PHYSICAL.index(file_name(drawn, by_label))
        target, mode = parsed.map[i], parsed.modes[i]
        inputs[drawn] = None if target == NONE else SWITCH[target]
        if target == NONE:
            custom[drawn] = 'none'
        elif mode & 7 != HOLD:
            custom[drawn] = ('tap', 'toggle', 'aim')[(mode & 7) - 1]
            fire = PHYSICAL[mode >> 3] if mode & 7 == AIM else None
            if fire in DRAWN:
                custom[next(d for d in DRAWN if file_name(d, by_label) == fire)] = 'aim'
        if target != NONE:
            users.setdefault(target, []).append(drawn)
    for shared in (ds for ds in users.values() if len(ds) > 1):
        for drawn in shared:
            custom.setdefault(drawn, 'shared')
    return inputs, custom


def swap(inputs, switch_input, drawn):
    """switch_input on drawn; the input drawn had goes to the button switch_input was on (each keeps one button)."""
    new = dict(inputs)
    before = next((d for d, s in inputs.items() if s == switch_input), None)
    new[drawn] = switch_input
    if before is not None and before != drawn:
        new[before] = inputs[drawn]
    return new


XBOX_LETTERS = (('a', 'south'), ('b', 'east'), ('x', 'west'), ('y', 'north'))


def xbox_letters(inputs):
    """The 'Xbox letters' preset: Switch A, B, X, Y on the drawn buttons labelled A (bottom), B, X, Y, by swaps, so
    each input keeps one button and the inputs the face buttons had move to where those letters were."""
    for switch_input, drawn in XBOX_LETTERS:
        inputs = swap(inputs, switch_input, drawn)
    return inputs


def _key(seg):
    found = _line(seg)
    return PHYSICAL[found[0]] if isinstance(found, tuple) else None


def render(original, desired, by_label, owned):
    """The new controls.txt: the old block and every line of a changed owned button removed, other lines in place, then
    a block with the owned buttons whose input is not what the rest gives. ValueError when the result is not exactly
    the intended layout (owned buttons as desired, every other button as before)."""
    before = parse(original or b'')
    if before.line:
        raise ValueError(f'the file has a bad line {before.line}: {before.error}')
    old, _ = layout(before, by_label)
    changed = {file_name(d, by_label) for d in owned if desired[d] != old[d]} | \
              {file_name(d, by_label) for d in owned if layout(before, by_label)[1].get(d)}
    segs = (original or b'').split(b'\n')
    marks = [seg.strip(b' \t\r') for seg in segs]
    kept, skipping = [], False
    for at, seg in enumerate(segs):
        if marks[at] == HEADER and END in marks[at:]:
            skipping = True                                         # an old block: its header to its end
        elif marks[at] == HEADER:
            continue                                                # a header alone (edited by hand): only it goes
        if not skipping and _key(seg) not in changed:
            kept.append(seg)
        if skipping and seg.strip(b' \t\r') == END:
            skipping = False
    text = b'\n'.join(kept).rstrip(b'\n')
    now = layout(parse(text), by_label)[0]
    block = [f'{file_name(d, by_label)} = {desired[d] or "none"}'.encode()
             for d in DRAWN if d in owned and now[d] != desired[d]]
    new = (text + b'\n' if text else b'') + (b'\n'.join([HEADER] + block + [END]) + b'\n' if block else b'')
    after = parse(new)
    if after.line:
        raise ValueError(f'the new text has a bad line {after.line}: {after.error}')
    result, _ = layout(after, by_label)
    for drawn in DRAWN:
        i = PHYSICAL.index(file_name(drawn, by_label))
        keep = (after.map[i], after.modes[i]) == (before.map[i], before.modes[i])
        if (drawn in owned and (result[drawn] != desired[drawn] or after.modes[i] != HOLD)) or \
                (drawn not in owned and not keep):
            raise ValueError(f'{drawn} would not be as intended')
    if after.map[16:] != before.map[16:] or after.modes[16:] != before.modes[16:]:
        raise ValueError('the touchpad or mouse lines would change')
    return new
