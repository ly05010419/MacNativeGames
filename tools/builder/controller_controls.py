"""The Controls page's model calls (E6 S6.2b): read controls.txt and controller-type.txt, keep which controller the
player uses when the type is auto, save an edited layout and move the file aside for the defaults. MNG Launcher's
Controls page (tools/builder_jobs/tab_controls.py) calls these.

Every game in PAGE_GAMES, each in its own data folder: <data>/<data_folder>/controls.txt and controller-type.txt
(TOTK_CONTROLS set by hand moves them; the page looks at the default place). With 'auto' the face buttons follow the
controller, so the page asks (xbox: by position, nintendo: by label) and keeps the answer per game in
Builder/controls-tab.json, never in the game's files; TOTK_FACE_BUTTONS set by hand wins over it. C6: every state
carries the game it was read for, and Save and Restore refuse a state of another game than the one given ('other_game',
nothing written). Save and Restore run only while the game is closed, read both files and the answer again and do
nothing when any differs from what the preview was made from (reason 'changed'), back the old file up first under a
name never used before (controls.txt.before-<date-time>[-n], created with O_EXCL), write through a symbolic link to its
target, and write atomically. The button icons are controller_icons'."""
from dataclasses import dataclass, field
from datetime import datetime
import json
import os
from pathlib import Path

from tools.builder import controls_file as cf, game_state
from tools.builder.games import PAGE_GAMES

ANSWERS = cf.ANSWERS


@dataclass(frozen=True)
class ControlsState:
    game: str = ''                  # S9.1 C6: the game it was read for
    reason: str = ''                # '' or not_offered
    type_word: str = 'auto'
    answer: str | None = None       # with auto: 'xbox' or 'nintendo', None until the player says
    by_label: bool | None = None    # None: ask first
    raw: bytes | None = None        # controls.txt as read; None when there is none
    type_raw: bytes | None = None
    error: tuple = ()               # (line, text) of a file the game refuses
    inputs: dict = field(default_factory=dict)
    custom: dict = field(default_factory=dict)
    link: bool = False


@dataclass(frozen=True)
class SaveResult:
    ok: bool
    reason: str = ''                # '' or: other_game, running, changed, refused
    backup: str = ''
    detail: str = ''


def _read(path):
    try:
        return Path(path).read_bytes()
    except FileNotFoundError:
        return None


def read_state(data_dir, store, game):
    if game not in PAGE_GAMES:
        return ControlsState(game=game, reason='not_offered')
    controls, kind = Path(data_dir) / 'controls.txt', Path(data_dir) / 'controller-type.txt'
    raw, type_raw, answer = _read(controls), _read(kind), cf.read_answer(store, game)
    word = cf.read_type(type_raw)
    by_label = {'switch': True, 'xbox': False, 'playstation': False}.get(word)
    if by_label is None and answer:
        by_label = answer == 'nintendo'
    base = dict(game=game, type_word=word, answer=answer, by_label=by_label, raw=raw, type_raw=type_raw,
                link=controls.is_symlink())
    parsed = cf.parse(raw or b'')
    if parsed.line:
        return ControlsState(**base, error=(parsed.line, parsed.error))
    if by_label is None:
        return ControlsState(**base)
    inputs, custom = cf.layout(parsed, by_label)
    return ControlsState(**base, inputs=inputs, custom=custom)


def set_answer(data_dir, store, game, answer):
    """C3: this game's answer, the other games' kept (a format-1 file is written as format 2)."""
    if answer not in ANSWERS:
        raise ValueError(f'unknown controller {answer!r}')
    answers = {**cf.read_answers(store), game: answer}
    _atomic(Path(store), json.dumps({'format': 2, 'controllers': answers}, indent=2).encode() + b'\n')
    return read_state(data_dir, store, game)


def _atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.partial')
    partial.write_bytes(data)
    os.replace(partial, path)


def _backup_name(folder):
    """A new controls.txt.before-<date-time>[-n] in folder, created empty with O_EXCL."""
    base = f'controls.txt.before-{datetime.now().strftime("%Y%m%d-%H%M%S")}'
    for n in range(1, 1000):
        path = Path(folder) / (base if n == 1 else f'{base}-{n}')
        try:
            os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644))
            return path
        except FileExistsError:
            continue
    raise FileExistsError(f'no free backup name for {base}')


def _unchanged(data_dir, store, state):
    now = read_state(data_dir, store, state.game)
    return (now.raw, now.type_raw, now.by_label) == (state.raw, state.type_raw, state.by_label)


def save(data_dir, store, game, state, desired, converted=()):
    data_dir = Path(data_dir)
    if state.game != game:
        return SaveResult(False, 'other_game')
    if game_state.game_running(data_dir):
        return SaveResult(False, 'running')
    if not _unchanged(data_dir, store, state):
        return SaveResult(False, 'changed')
    if state.by_label is None or state.error:
        return SaveResult(False, 'refused', detail='no controller mode, or a file the game refuses')
    owned = {d for d in cf.DRAWN if d not in state.custom} | set(converted)
    try:
        new = cf.render(state.raw, desired, state.by_label, owned)
    except ValueError as e:
        return SaveResult(False, 'refused', detail=str(e))
    controls = data_dir / 'controls.txt'
    target = controls.resolve() if controls.is_symlink() else controls
    backup = ''
    if state.raw is not None:
        path = _backup_name(controls.parent)
        path.write_bytes(state.raw)
        backup = path.name
    _atomic(target, new)
    return SaveResult(True, backup=backup)


def restore(data_dir, store, game, state):
    """The defaults: controls.txt moved aside (a link moves as a link); nothing is deleted."""
    data_dir = Path(data_dir)
    if state.game != game:
        return SaveResult(False, 'other_game')
    if game_state.game_running(data_dir):
        return SaveResult(False, 'running')
    if not _unchanged(data_dir, store, state):
        return SaveResult(False, 'changed')
    controls = data_dir / 'controls.txt'
    if state.raw is None and not controls.is_symlink():
        return SaveResult(True)
    path = _backup_name(data_dir)
    os.replace(controls, path)
    return SaveResult(True, backup=path.name)
