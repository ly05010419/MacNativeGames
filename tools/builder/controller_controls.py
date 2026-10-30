"""The controller's part for the Controls tab (E6 S6.2b): read controls.txt and controller-type.txt, keep which
controller the player uses when the type is auto, save an edited layout and move the file aside for the defaults. It
runs the model on load_jobs and emits events through the BuildController that owns it; the view calls it as
controller.controls.

Every game in GAMES, each in its own data folder: <data>/<data_folder>/controls.txt and controller-type.txt
(TOTK_CONTROLS set by hand moves them; the tab looks at the default place). With 'auto' the face buttons follow the
controller, so the tab asks (xbox: by position, nintendo: by label) and keeps the answer per game in
Builder/controls-tab.json, never in the game's files; TOTK_FACE_BUTTONS set by hand wins over it. C6: every state
carries the game it was read for, and Save and Restore refuse a state of another game than the one chosen now
('other_game', nothing written). Save and Restore run only while the game is closed, read both files and the answer
again and do nothing when any differs from what the preview was made from (reason 'changed'), back the old file up first
under a name never used before (controls.txt.before-<date-time>[-n], created with O_EXCL), write through a symbolic link
to its target, and write atomically. Save and Restore hold the controller (not idle); one job at a time. S6.3:
icons_check and update_icons run controller_icons' jobs; the made icons mod is handed to the Mods part (its
install_icons, the only writer of library.json) as soon as that part is free, and the controller stays held until then;
cancel stops pip or the generator."""
from dataclasses import dataclass, field
from datetime import datetime
import json
import os
from pathlib import Path
import threading

from tools.builder import controls_file as cf, game_state
from tools.builder import controller_icons
from tools.builder.build_run import collect
from tools.builder.controller_mods import IconsResult
from tools.builder.games import GAMES
from tools.builder.session import ControlsReady, ControlsSaved, IconsChecked, IconsDone, ProcessError

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
    if game not in GAMES:
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


class ControlsController:
    def __init__(self, owner):
        self.owner = owner
        self._job = self._kind = self._handoff = None
        self._cancel = threading.Event()

    @property
    def busy(self):
        return self._job is not None

    @property
    def holds(self):
        """Save, Restore or making the icons runs (or waits to hand them to the Mods part): not idle."""
        return self._kind in ('save', 'restore', 'icons') or self._handoff is not None

    def cancel(self):
        """Stops making the icons (pip or the generator is killed); nothing else is cancellable."""
        if self._kind == 'icons':
            self._cancel.set()

    def _icons_args(self):
        """(game dir, store, repo, spec, app, data dir, Builder dir) for controller_icons' jobs."""
        spec, app = self.owner.start._context()
        game_dir, store, _ = self._paths(spec.id)
        return game_dir, store, self.owner.repo_root, spec, app, self.owner.start.paths.data, store.parent

    def icons_check(self):
        self._submit('read', controller_icons.icons_info, *self._icons_args())

    def update_icons(self, install_env=False, switch_off=()):
        if not self.owner.idle:
            raise RuntimeError('a build, an update or Play is running')
        cancel = threading.Event()
        self._submit('icons', controller_icons.make_icons, *self._icons_args(), bool(install_env), cancel)
        self._cancel, self._switch_off = cancel, tuple(switch_off)

    def _paths(self, game):
        """(the game's data folder, Builder/controls-tab.json, the game): the leading arguments of every job."""
        paths = self.owner.start.paths
        return paths.data / GAMES[game].data_folder, paths.store.parent / 'controls-tab.json', game

    def _submit(self, kind, fn, *args):
        self.owner._open()
        if self._job is not None:
            raise RuntimeError('a controls change is still running')
        self._job = self.owner.load_jobs.submit(fn, *args)
        self._kind = kind

    @staticmethod
    def swap(inputs, switch_input, drawn):
        """The view's preview of a pick (controls_file.swap)."""
        return cf.swap(inputs, switch_input, drawn)

    @staticmethod
    def xbox_letters(inputs):
        """The view's preview of the 'Xbox letters' preset (controls_file.xbox_letters)."""
        return cf.xbox_letters(inputs)

    def _chosen(self):
        return self._paths(self.owner.selection()[0])

    def read(self):
        self._submit('read', read_state, *self._chosen())

    def answer(self, controller):
        self._submit('read', set_answer, *self._chosen(), controller)

    def save(self, state, desired, converted=()):
        if not self.owner.idle:
            raise RuntimeError('a build, an update or Play is running')
        self._submit('save', save, *self._chosen(), state, dict(desired), tuple(converted))

    def restore(self, state):
        if not self.owner.idle:
            raise RuntimeError('a build, an update or Play is running')
        self._submit('restore', restore, *self._chosen(), state)

    def tick(self):
        if self._handoff is not None and not self.owner.mods.busy:
            work, made_from, switch_off = self._handoff
            self.owner.mods.install_icons(work, made_from, switch_off)
            self._handoff = None
        if self._job is None or not self._job.done():
            return
        job, kind, self._job, self._kind = self._job, self._kind, None, None
        value, error = collect(job)
        if error is not None:
            self.owner._emit(ProcessError('controls', repr(error)))
        elif kind == 'icons':
            if value.ok:
                self._handoff = (value.work, value.made_from, self._switch_off)   # the Mods part installs it
            else:
                self.owner._emit(IconsDone(IconsResult(False, value.reason, value.detail)))
        elif isinstance(value, controller_icons.IconsInfo):
            self.owner._emit(IconsChecked(value))
        else:
            self.owner._emit(ControlsSaved(value) if kind in ('save', 'restore') else ControlsReady(value))
