"""The player's own background image (S4.3, K2 and K4): where the builder keeps its copy, the copy, the picture the
window shows, and the removal. The model: no Tk, no rule about how the picture is drawn.

The builder ships no image. The chosen file is copied next to the settings (`background<ext>`, one file), so moving or
deleting the original changes nothing; settings.json is not touched. From the copy, macOS's sips draws
`background-shown-<w>x<h>.png`: the image scaled to cover the screen (keeping its proportions, centred, the overflow
cut), in PNG because Tk reads no JPEG or HEIC. A new choice is written and drawn under temporary names first, so a file
that cannot be read or copied leaves the earlier background as it was. The focus (which part of the picture the window's
picture zone shows: left, center or right) is kept in `background-focus.txt`."""
from dataclasses import dataclass
import math
from pathlib import Path
import shutil

from tools.builder.ports import Outcome

MAX_BYTES = 50 * 1024 * 1024
SUFFIXES = ('.png', '.jpg', '.jpeg', '.heic', '.gif', '.tif', '.tiff')
STEM, PARTIAL, SHOWN, FOCUS = 'background', '.background-partial', 'background-shown-', 'background-focus.txt'
FOCI = ('left', 'center', 'right')
SIPS, SIPS_S = '/usr/bin/sips', 30.0


@dataclass(frozen=True)
class Background:
    path: Path | None                 # the saved copy, or None when there is none
    shown: Path | None = None         # the PNG the window draws, or None when there is none to draw
    refused: str = ''                 # '' or why a file was not taken or drawn: 'kind', 'too_big' or 'unreadable'
    focus: str = 'center'             # one of FOCI


def _copies(folder):
    return sorted(p for p in Path(folder).glob(STEM + '.*') if p.suffix.lower() in SUFFIXES)


def _shown(folder, size):
    return Path(folder) / f'{SHOWN}{size[0]}x{size[1]}.png'


def _dims(runner, path):
    r = runner.run([SIPS, '-g', 'pixelWidth', '-g', 'pixelHeight', str(path)], timeout_s=SIPS_S)
    values = [line.split(':')[1].strip() for line in r.stdout.splitlines() if line.strip().startswith('pixel')]
    if r.outcome is not Outcome.OK or len(values) != 2 or not all(v.isdigit() and int(v) > 0 for v in values):
        return None
    return int(values[0]), int(values[1])


def _draw(runner, source, target, size):
    """sips: source scaled to cover size and cut to it, as a PNG at target. False when sips cannot read it."""
    dims = _dims(runner, source)
    if dims is None:
        return False
    scale = max(size[0] / dims[0], size[1] / dims[1])
    wide, high = max(size[0], math.ceil(dims[0] * scale)), max(size[1], math.ceil(dims[1] * scale))
    r = runner.run([SIPS, '-s', 'format', 'png', '--resampleHeightWidth', str(high), str(wide),
                    '--cropToHeightWidth', str(size[1]), str(size[0]), str(source), '--out', str(target)],
                   timeout_s=SIPS_S)
    return r.outcome is Outcome.OK and Path(target).is_file()


def _drop_shown(folder, keep=None):
    for old in Path(folder).glob(SHOWN + '*.png'):
        if old != keep:
            old.unlink()


def _focus(folder):
    try:
        value = (Path(folder) / FOCUS).read_text().strip()
    except OSError:
        return 'center'
    return value if value in FOCI else 'center'


def _current(folder, size):
    found = _copies(folder)
    shown = _shown(folder, size)
    return Background(found[0] if found else None, shown if found and shown.is_file() else None,
                      focus=_focus(folder))


def saved(runner, folder, size):
    """The saved copy and its picture for this screen size, drawn now if it is missing (a new screen, a first run)."""
    current = _current(folder, size)
    if current.path is None or current.shown is not None:
        return current
    partial = Path(folder) / (PARTIAL + '-shown.png')
    try:
        if not _draw(runner, current.path, partial, size):
            return Background(current.path, None, 'unreadable', current.focus)
        _drop_shown(folder)
        partial.replace(_shown(folder, size))
    finally:
        partial.unlink(missing_ok=True)
    return Background(current.path, _shown(folder, size), focus=current.focus)


def save(runner, folder, source, size):
    """Copy source in as the background and draw it, replacing an earlier one. Another kind, a file over MAX_BYTES
    or one sips cannot read is refused and the earlier background stays. OSError (a missing source, a full disk) is
    the caller's to report."""
    folder, source = Path(folder), Path(source)
    if source.suffix.lower() not in SUFFIXES:
        return _refuse(folder, size, 'kind')
    if source.stat().st_size > MAX_BYTES:
        return _refuse(folder, size, 'too_big')
    folder.mkdir(parents=True, exist_ok=True)
    partial, picture = folder / (PARTIAL + source.suffix.lower()), folder / (PARTIAL + '-shown.png')
    try:
        shutil.copyfile(source, partial)
        if not _draw(runner, partial, picture, size):
            return _refuse(folder, size, 'unreadable')
        target, shown = folder / (STEM + source.suffix.lower()), _shown(folder, size)
        for old in _copies(folder):
            if old != target:
                old.unlink()
        _drop_shown(folder)
        partial.replace(target)
        picture.replace(shown)
    finally:
        partial.unlink(missing_ok=True)
        picture.unlink(missing_ok=True)
    return Background(target, shown, focus=_focus(folder))


def _refuse(folder, size, why):
    current = _current(folder, size)
    return Background(current.path, current.shown, why, current.focus)


def focus(runner, folder, size, value):
    """Keep which part of the picture is shown; ValueError for a value outside FOCI."""
    if value not in FOCI:
        raise ValueError(f'focus must be one of {FOCI}, not {value!r}')
    Path(folder).mkdir(parents=True, exist_ok=True)
    (Path(folder) / FOCUS).write_text(value + '\n')
    return _current(folder, size)


def remove(runner, folder):
    for old in _copies(folder):
        old.unlink()
    _drop_shown(folder)
    (Path(folder) / FOCUS).unlink(missing_ok=True)
    return Background(None)
