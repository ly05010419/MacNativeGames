"""The controller's part for the Settings tab (S5.1): read and keep the start options, probe the GPU, and Play. It
runs the model on the daemon load_jobs (files, a slot lock, `open` and a 15-21 s probe never hold the build worker)
and emits events through the BuildController that owns it; the view calls it as controller.start.

One settings job at a time (a second call raises, like the picture's); the probe runs once per window (a later call does
nothing). Play needs the controller idle (a rebuild replaces the app in place). The game is the one chosen in the Build
tab, and its app the one that tab's paths name (the saved Output app, or build_app.py's default next to the game
file)."""
from pathlib import Path

from tools.builder import game_state, start_options
from tools.builder.build_run import collect
from tools.builder.checks import BuildPaths
from tools.builder.games import GAMES
from tools.builder.paths import effective_paths
from tools.builder.session import GpuProbed, PlayDone, ProcessError, StartStateReady


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
        self._submit('settings', start_options.read_state, spec, app, self.paths)

    def choose(self, option, choice):
        spec, app = self._context()
        self._submit('settings', start_options.choose, spec, app, self.paths, option, choice)

    def play(self):
        if not self.owner.idle:
            raise RuntimeError('a build is running or being prepared')
        spec, app = self._context()
        self._submit('play', start_options.play, self.owner.runner, spec, app, self.paths, self.gpu)

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
                self.owner._emit(ProcessError(kind, repr(error)))
            else:
                self.owner._emit(PlayDone(value) if kind == 'play' else StartStateReady(value))
        if self._probe is not None and self._probe.done():
            value, error = collect(self._probe)
            self._probe, self.gpu = None, value if error is None else 'unknown'
            self.owner._emit(GpuProbed(self.gpu))
