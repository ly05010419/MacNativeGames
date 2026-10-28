"""The controller the two views share (S1.7b, C1-C3): selection, checks, start, cancel, close; events in a queue.

The view calls tick() from its timer; tick() never waits. Blocking work runs as jobs on the worker (jobs.py): settings
load, each check, preparing a build request, starting and saving. A check run and a build each work on a frozen snapshot
of the selection."""
from dataclasses import dataclass, replace
from pathlib import Path
import threading

from tools.builder import background, updates
from tools.builder.build_request import LOG_NAMES, prepare_request, request_argv, reserve_log  # noqa: F401
from tools.builder.build_run import BuildRun, collect
from tools.builder.controller_controls import ControlsController
from tools.builder.controller_mods import ModsController
from tools.builder.controller_start import StartController
from tools.builder.checks import BuildPaths, Status, iter_checks, ready
from tools.builder.config import PATH_FIELDS
from tools.builder.games import GAMES
from tools.builder.session import (BuildSession, CheckResultReady, ChecksDone, ChecksInvalidated, Finished, Outcome,
                                   PathsIncomplete, ProcessError, SettingsLoaded, UpdateApplied, UpdateStatusReady,
                                   BackgroundChanged)

_END = object()


def _next_check(results):
    return next(results, _END)


@dataclass
class _CheckRun:
    snapshot: tuple
    results: object             # the iter_checks generator
    job: object
    done: list


class BuildController:
    def __init__(self, *, base_context, system_checks, input_checks, store, process_factory, jobs, runner, clock,
                 repo_root, logs_dir, load_jobs=None):
        self.base_context, self.checks = base_context, tuple(system_checks) + tuple(input_checks)
        self.store, self.process_factory, self.jobs, self.runner = store, process_factory, jobs, runner
        self.load_jobs = jobs if load_jobs is None else load_jobs       # S3.4 f4: a stuck load must not hold the worker
        self.clock, self.repo_root, self.logs_dir = clock, Path(repo_root), Path(logs_dir)
        self._events, self._game, self._fields, self._saved = [], None, {}, {}
        self._load_job, self._check, self._ready, self._preparing, self._run = None, None, None, None, None
        self._update_check = self._update_apply = self._background = None          # S4.1, S4.3
        self._closed = False
        self.start = StartController(self)                     # S5.1: the Settings tab's part
        self.mods = ModsController(self)                       # S5.2: the Mods tab's part
        self.controls = ControlsController(self)               # E6: the Controls tab's part

    # selection
    def select_game(self, game_id):
        if game_id not in GAMES:
            raise ValueError(f'unknown game {game_id!r}')
        self._game = game_id
        saved = self._saved.get(game_id)
        self._fields = {f: getattr(saved, f) for f in PATH_FIELDS if saved and getattr(saved, f)} if saved else {}
        self._invalidate()

    def set_paths(self, **fields):
        unknown = set(fields) - set(PATH_FIELDS)
        if unknown:
            raise ValueError(f'unknown path fields {sorted(unknown)}')
        self._fields.update({k: (str(v) if v else None) for k, v in fields.items()})
        self._invalidate()

    def selection(self):
        return self._game, dict(self._fields)

    def games(self):
        """S2.2 W5: (id, display name) of every game, for the window's choice; read-only."""
        return tuple((spec.id, spec.display_name) for spec in GAMES.values())

    def path_fields(self):
        """S2.2 W5: the path fields, in order; read-only."""
        return PATH_FIELDS

    def _snapshot(self):
        return self._game, tuple((f, self._fields.get(f)) for f in PATH_FIELDS)

    def _invalidate(self):
        if self._check is not None or self._ready is not None:      # S2.3: ready checks are dropped visibly too
            self._check = None                          # the running job's result is never read
            self._emit(ChecksInvalidated())
        self._ready = None

    # commands
    def load_settings(self):
        self._open()
        self._load_job = self.load_jobs.submit(self.store.load)

    def check_updates(self):
        """S4.1 U1: on load_jobs, like the settings load: a fetch can be slow or hang. UpdateStatusReady when done."""
        self._open()
        if self._update_check is None:
            self._update_check = self.load_jobs.submit(updates.check_updates, self.runner, self.repo_root)

    def apply_update(self):
        """S4.1 U2: on the worker, so it never runs beside a build start; only when idle. UpdateApplied when done."""
        self._open()
        if not self.idle:
            raise RuntimeError('a build is running or being prepared')
        self._update_apply = self.jobs.submit(updates.apply_update, self.runner, self.repo_root)

    def background(self, action, *args):
        """S4.3: 'saved' (screen size), 'save' (chosen file, screen size), 'focus' (screen size, left|center|right)
        or 'remove' the background image next to the settings, on load_jobs (the disk and sips may be slow);
        BackgroundChanged when done. One at a time: two would race on the files."""
        self._open()
        if self._background is not None:
            raise RuntimeError('a background change is still running')
        fn = {'saved': background.saved, 'save': background.save, 'remove': background.remove,
              'focus': background.focus}[action]
        self._background = self.load_jobs.submit(fn, self.runner, self.store.path.parent, *args)

    @property
    def background_busy(self):
        return self._background is not None

    def run_checks(self):
        self._open()
        if self._game is None:
            raise RuntimeError('select a game first')
        missing = tuple(f for f in ('game', 'keys') if not self._fields.get(f))
        if missing:
            self._emit(PathsIncomplete(missing))
            return
        snapshot = self._snapshot()
        paths = BuildPaths(**{f: Path(v) if v else None for f, v in snapshot[1]})
        ctx = replace(self.base_context, spec=GAMES[self._game], paths=paths)
        results = iter_checks(self.checks, ctx)
        self._ready = None
        self._check = _CheckRun(snapshot, results, self.jobs.submit(_next_check, results), [])

    def start_build(self):
        self._open()
        if not self.idle:
            raise RuntimeError('a build is running or being prepared')
        snapshot, python = self._fresh_ready()
        cancel = threading.Event()
        compose = self.mods.code_for_build(snapshot)    # S5.4: None, or the code mods composed at the build's start
        job = self.jobs.submit(prepare_request, GAMES[snapshot[0]], snapshot[1], self.repo_root, python,
                               self.logs_dir, self.clock.wall(), self.runner, cancel, compose)
        self._preparing = (job, cancel)

    def planned_command(self):
        """S2.1 T6: the argv start_build() would run now, for the text mode's --dry-run. It touches the disk
        (effective_paths resolves the paths); RuntimeError when the checks are not ready for this selection."""
        self._open()                                # S2.1b, H4
        snapshot, python = self._fresh_ready()
        code = self.mods.code_folder(snapshot)          # S5.4: the path --mod would get; nothing is written
        return request_argv(GAMES[snapshot[0]], snapshot[1], self.repo_root, python, code)[1]

    def cancel(self):
        if self._preparing is not None:
            self._preparing[1].set()
        elif self._run is not None and self._run.active:
            self._run.cancel()
        self.mods.cancel()                              # S5.5 B2: an archive being extracted
        self.controls.cancel()                          # E6 S6.3: the icons being made

    @property
    def cancel_failed(self):
        """The last cancel request was refused by the system; the build runs on and cancel() may be tried again."""
        return self._run is not None and self._run.active and self._run.cancel_failed

    @property
    def idle(self):
        """S2.1 T8: no start is being prepared, no run is active, no update is being applied (S4.1) and no mods are
        being applied (S5.2) nor added (S5.5), no controls saved (E6), so close() and start_build() are allowed."""
        return (self._preparing is None and self._update_apply is None and not self.mods.holds
                and not self.controls.holds and not (self._run is not None and self._run.active))

    def close(self):
        if not self.idle:
            raise RuntimeError('a build is running, being prepared or saving; cancel it and wait for it to end')
        self._check = None
        self.jobs.shutdown()
        if self.load_jobs is not self.jobs:
            self.load_jobs.shutdown()
        self._closed = True

    def take_events(self):
        events, self._events = self._events, []
        return events

    def tick(self):
        if self._closed:
            return
        self._tick_load()
        self._tick_updates()
        self.start.tick()
        self.mods.tick()
        self.controls.tick()
        self._tick_checks()
        self._tick_preparing()
        if self._run is not None and self._run.active:
            self._run.tick()

    # internals
    def _open(self):
        if self._closed:
            raise RuntimeError('the controller is closed')

    def _fresh_ready(self):
        """The ready snapshot and its build Python, or RuntimeError if the checks are missing or stale."""
        if self._ready is None or self._ready[0] != self._snapshot():
            raise RuntimeError('run the checks for the current selection first')
        snapshot, results = self._ready
        return snapshot, next(r.detail for r in results if r.id == 'build_python' and r.status is Status.OK)

    def _emit(self, event):
        self._events.append(event)

    def _tick_load(self):
        if self._load_job is None or not self._load_job.done():
            return
        job, self._load_job = self._load_job, None
        result, error = collect(job)
        if error is not None:
            self._emit(ProcessError('load', repr(error)))
            return
        self._saved = dict(result.settings.paths)      # only what select_game fills from; the selection is untouched
        self._emit(SettingsLoaded(result.problems))

    def _tick_updates(self):
        for attr, event, where in (('_update_check', UpdateStatusReady, 'update'),
                                   ('_update_apply', UpdateApplied, 'update'),
                                   ('_background', BackgroundChanged, 'background')):
            job = getattr(self, attr)
            if job is None or not job.done():
                continue
            setattr(self, attr, None)                   # removed before its result is read (R5)
            value, error = collect(job)
            self._emit(ProcessError(where, repr(error)) if error is not None else event(value))

    def _tick_checks(self):
        run = self._check
        if run is None or not run.job.done():
            return
        result, error = collect(run.job)
        if error is not None:
            self._check = None
            self._emit(ProcessError('checks', repr(error)))
            self._emit(ChecksDone(False))
        elif result is _END:
            self._check = None
            ok = ready(run.done)
            self._ready = (run.snapshot, tuple(run.done)) if ok else None
            self._emit(ChecksDone(ok))
        else:
            run.done.append(result)
            self._emit(CheckResultReady(result))
            run.job = self.jobs.submit(_next_check, run.results)

    def _tick_preparing(self):
        if self._preparing is None or not self._preparing[0].done():
            return
        (job, cancel), self._preparing = self._preparing, None      # removed before its result is read (R5)
        request, error = collect(job)
        if error is not None:
            if not isinstance(error, OSError):
                self._emit(ProcessError('prepare', repr(error)))
            self._emit(Finished(Outcome.START_FAILED, None, None, str(error), None))
        elif request is None or cancel.is_set():
            self._emit(Finished(Outcome.CANCELLED, None, None, None, None if request is None else str(request.log_path)))
        else:
            self._run = BuildRun(request, self.process_factory(), BuildSession(request.game), self.jobs, self.store,
                                 self.clock, cancel, self._emit)
