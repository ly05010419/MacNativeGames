"""The key list of a game (games/<game>/assets/ui_pack_keys*.tsv): which textures get a pack picture, at what screen scale, with
what options. Format (TSV, # starts a comment line):
  # reference-height: <int>     the picture height the scales were measured at
  # default-scale: <float>      the scale of a key whose scale cell is empty
  key<TAB>scale<TAB>options<TAB>name
scale: screen px per texel at the reference height, or empty. options: a JSON object ({} if none). name: free text (where it is drawn)."""
import json
import re
from pathlib import Path
from typing import NamedTuple

from tools.ui_pack.errors import UiPackError

CHECKS = ('ink', 'levels', 'rt', 'gv')    # the gate checks an `accept` option can name (gate.py)
OPTIONS = {'mode', 'T', 'P', 'tau', 'zdil', 'S0', 'ea_ch', 'hard', 'K', 'it', 'reg', 'reg0', 'sig', 'trace_tol', 'turd', 'poly',
           'ctol', 'cmin', 'noround', 'nopix', 'prim', 'corner', 'param', 'pix', 'sym', 'patch', 'patch_sig', 'F', 'skip', 'accept'}
LEVELS = re.compile(r'levels\d+$')          # levels0, levels1, ...: the plateau values of a channel


class Entry(NamedTuple):
    key: str
    scale: float | None
    options: dict
    name: str


class KeyList(NamedTuple):
    reference_height: int
    default_scale: float
    entries: dict           # key -> Entry


def check_options(options, where='options'):
    """An unknown option name is an error; so is an `accept` that is not {check: reason}."""
    if not isinstance(options, dict):
        raise UiPackError(f'{where}: the options must be a JSON object')
    for name in options:
        if name not in OPTIONS and not LEVELS.match(name):
            raise UiPackError(f'{where}: unknown option "{name}"')
    accept = options.get('accept', {})
    if not isinstance(accept, dict) or any(k not in CHECKS or not isinstance(v, str) or not v for k, v in accept.items()):
        raise UiPackError(f'{where}: accept must map a check name ({", ".join(CHECKS)}) to a reason text')
    if 'F' in options and (type(options['F']) is not int or not 1 <= options['F'] <= 8):
        raise UiPackError(f'{where}: F must be an integer from 1 to 8 (the game takes a pack picture of at most 8x)')
    if 'skip' in options and (not isinstance(options['skip'], str) or not options['skip']):
        raise UiPackError(f'{where}: skip must be the evidence text')
    return options


def read_keylist(path):
    path = Path(path)
    if not path.is_file():
        raise UiPackError(f'{path} not found (the key list of this game is not written yet; use --keys <tsv>)')
    ref = default = None
    entries = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        where = f'{path}:{number}'
        if not line.strip():
            continue
        if line.lstrip().startswith('#'):
            m = re.match(r'\s*#\s*(reference-height|default-scale):\s*(\S+)\s*$', line)
            if m:
                try:
                    if m.group(1) == 'reference-height':
                        ref = int(m.group(2))
                    else:
                        default = float(m.group(2))
                except ValueError:
                    raise UiPackError(f'{where}: bad {m.group(1)} "{m.group(2)}"') from None
            continue
        cells = line.split('\t')
        if len(cells) != 4 or not cells[0].strip():
            raise UiPackError(f'{where}: expected key<TAB>scale<TAB>options<TAB>name')
        key, scale, options, name = (c.strip() for c in cells)
        try:
            scale = float(scale) if scale else None
        except ValueError:
            raise UiPackError(f'{where}: bad scale "{scale}"') from None
        if scale is not None and scale <= 0:
            raise UiPackError(f'{where}: the scale must be positive')
        try:
            options = json.loads(options)
        except ValueError as error:
            raise UiPackError(f'{where}: the options are not JSON ({error})') from None
        if key in entries:
            raise UiPackError(f'{where}: {key} is listed twice')
        entries[key] = Entry(key, scale, check_options(options, where), name)
    if ref is None or ref <= 0:
        raise UiPackError(f'{path}: the header needs "# reference-height: <int>"')
    if default is None or default <= 0:
        raise UiPackError(f'{path}: the header needs "# default-scale: <float>"')
    return KeyList(ref, default, entries)


def draw_scale(entry, keylist, screen_height):
    """s_d: screen px per texel at the screen height."""
    return (entry.scale if entry.scale is not None else keylist.default_scale) * screen_height / keylist.reference_height
