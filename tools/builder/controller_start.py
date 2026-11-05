"""The controller's part for the Settings tab (S5.1): read and keep the start options, probe the GPU, and Play. It
runs the model on the daemon load_jobs (files, a slot lock, `open` and a 15-21 s probe never hold the build worker)
and emits events through the BuildController that owns it; the view calls it as controller.start.

One settings job at a time (a second call raises, like the picture's); the probe runs once per window (a later call does
nothing). Play needs the controller idle (a rebuild replaces the app in place). The game is the one chosen in the Build
tab, and its app the one that tab's paths name (the saved Output app, or build_app.py's default next to the game
file). E12: a choice being saved and a Play being started hold the controller (holds: not idle), so closing the window
asks and waits instead of ending the save with the process; a read and the GPU probe do not. Errors still name
'settings' (a read or a choice) or 'play', where the window sends them."""
from functools import partial
from pathlib import Path

from tools.builder import controller_textures, game_state, mod_code, start_options
from tools.builder.build_run import collect
from tools.builder.checks import BuildPaths
from tools.builder.games import GAMES
from tools.builder.paths import effective_paths
from tools.builder.session import GpuProbed, PlayDone, ProcessError, StartStateReady


def _read(spec, app, paths, repo, *choice):
    """The state (or a choice and the new state), knowing whether the app has the texture pack's heaps (E17)."""
    heaps = bool(controller_textures.play_env(repo, spec, app, paths.data))
    return start_options.choose(spec, app, paths, *choice, heaps) if choice else \
        start_options.read_state(spec, app, paths, heaps)


def _native_4k(spec, fields, paths, code_mod):
    """E17: for a build, once its code-mod folder is composed (a texture pack's heap table rides in it)."""
    heaps = code_mod is not None and (Path(code_mod) / 'romfs' / mod_code.HEAPDEF).is_file()
    return start_options.build_native_4k(spec, fields, paths, heaps)


def _play(runner, spec, app, paths, gpu, repo):
    """Play, with the texture pack's 8 GB mode when the app was built with its heaps (S9.2 C6)."""
    return start_options.play(runner, spec, app, paths, gpu, controller_textures.play_env(repo, spec, app, paths.data))


class StartController:
    def __init__(self, owner):
        self.owner = owner
        self._job = self._kind = self._probe = None
        self.gpu = None                 # None until probed; then 'checking', 'yes', 'no' or 'unknown'
        builder = Path(owner.store.path).parent
        self.paths = start_options.StartPaths(builder / 'start-options.json', builder.parent)

    @property
    def busy(self):
        return self._job is not None

    @property
    def holds(self):
        """A choice is being written or Play is starting the game (E12)."""
        return self._kind in ('choose', 'play')

    def _context(self):
        game, fields = self.owner.selection()
        if game is None:
            raise RuntimeError('no game chosen')
        spec = GAMES[game]
        if not fields.get('game'):
            return spec, None
        raw = BuildPaths(**{f: Path(fields[f]) if fields.get(f) else None for f in BuildPaths.__dataclass_fields__})
        return spec, effective_paths(spec, raw).out

    def _submit(self, kind, fn, *args):
        self.owner._open()
        if self._job is not None:
            raise RuntimeError('a settings change is still running')
        self._job, self._kind = self.owner.load_jobs.submit(fn, *args), kind

    def state(self):
        spec, app = self._context()
        self._submit('read', _read, spec, app, self.paths, self.owner.repo_root)

    def choose(self, option, choice):
        spec, app = self._context()
        self._submit('choose', _read, spec, app, self.paths, self.owner.repo_root, option, choice)

    def native_4k_for(self, snapshot):
        """E17 S17.1: native_4k(code_mod) for a build of snapshot: what the Settings tab's picture asks of it."""
        return partial(_native_4k, GAMES[snapshot[0]], dict(snapshot[1]), self.paths)

    def play(self):
        if not self.owner.idle:
            raise RuntimeError('a build is running or being prepared')
        spec, app = self._context()
        self._submit('play', _play, self.owner.runner, spec, app, self.paths, self.gpu, self.owner.repo_root)

    def probe(self):
        self.owner._open()
        if self.gpu is None:
            self.gpu = 'checking'
            self._probe = self.owner.load_jobs.submit(game_state.probe_metal4fx, self.owner.runner)

    def tick(self):
        if self._job is not None and self._job.done():
            job, kind, self._job, self._kind = self._job, self._kind, None, None
            value, error = collect(job)
            if error is not None:
                self.owner._emit(ProcessError('play' if kind == 'play' else 'settings', repr(error)))
            else:
                self.owner._emit(PlayDone(value) if kind == 'play' else StartStateReady(value))
        if self._probe is not None and self._probe.done():
            value, error = collect(self._probe)
            self._probe, self.gpu = None, value if error is None else 'unknown'
            self.owner._emit(GpuProbed(self.gpu))
