"""The view events, and the pure reading of one build's output into them (S1.7a).

BuildSession is fed S1.4's progress events and returns view events; finish() decides the outcome. No I/O, no clock:
every rule is testable with lists of events. Every progress event except a Heartbeat also yields a LogLine with its raw
line, so the view's log shows the build's output as it was printed.

S1.8b: Finished also says how the process ended and which signals were accepted, and, for a cancelled build, whether
that ending is compatible with those signals and what the outcome would have been without the request. Nothing claims
the cancel worked."""
from dataclasses import dataclass
from enum import Enum
import signal

from tools.builder.progress import Done, Failure, GameIdentified, Heartbeat as ProgressHeartbeat, Line, Stage, UpdateIdentified


class Outcome(Enum):
    START_FAILED = 'start_failed'
    GAME_MISMATCH = 'game_mismatch'
    CANCELLED = 'cancelled'
    INCOMPLETE = 'incomplete'       # the log could not be read to its end, so success cannot be shown
    SUCCESS = 'success'
    FAILED = 'failed'


@dataclass(frozen=True)
class SettingsLoaded:
    problems: tuple


@dataclass(frozen=True)
class SettingsNotSaved:
    error: str


@dataclass(frozen=True)
class PathsSaved:
    """The storage tab: a path the player changed was written to settings.json now, without a build."""

    game: str
    ok: bool
    error: str = ''


@dataclass(frozen=True)
class PathsIncomplete:
    missing: tuple


@dataclass(frozen=True)
class CheckResultReady:
    result: object          # tools.builder.checks.CheckResult


@dataclass(frozen=True)
class ChecksInvalidated:
    pass


@dataclass(frozen=True)
class ChecksDone:
    ready: bool


@dataclass(frozen=True)
class BuildStarted:
    argv: tuple
    log_path: str


@dataclass(frozen=True)
class StageChanged:
    index: int
    total: int
    name: str
    text: str


@dataclass(frozen=True)
class LogLine:
    text: str


@dataclass(frozen=True)
class Heartbeat:
    minutes: int


@dataclass(frozen=True)
class GameMismatch:
    expected: str
    found: str


@dataclass(frozen=True)
class ProcessError:
    where: str              # 'start', 'poll', 'cancel' or 'kill' (S1.8a), among others
    error: str


@dataclass(frozen=True)
class CancelStuck:
    pass


@dataclass(frozen=True)
class UpdateStatusReady:
    status: object          # tools.builder.updates.UpdateStatus (S4.1)


@dataclass(frozen=True)
class UpdateApplied:
    result: object          # tools.builder.updates.UpdateResult (S4.1)


@dataclass(frozen=True)
class BackgroundChanged:
    background: object      # tools.builder.background.Background (S4.3)


@dataclass(frozen=True)
class StartStateReady:
    state: object           # tools.builder.start_options.StartState (S5.1)


@dataclass(frozen=True)
class PlayDone:
    result: object          # tools.builder.start_options.PlayResult (S5.1)


@dataclass(frozen=True)
class ModsStateReady:
    state: object           # tools.builder.controller_mods.ModsState (S5.2)


@dataclass(frozen=True)
class ModsApplied:
    result: object          # tools.builder.mod_library.ApplyResult (S5.2)


@dataclass(frozen=True)
class ControlsReady:
    state: object           # tools.builder.controller_controls.ControlsState (E6 S6.2b)


@dataclass(frozen=True)
class ControlsSaved:
    result: object          # tools.builder.controller_controls.SaveResult (E6 S6.2b)


@dataclass(frozen=True)
class IconsChecked:
    info: object            # tools.builder.controller_controls.IconsInfo (E6 S6.3)


@dataclass(frozen=True)
class IconsDone:
    result: object          # tools.builder.controller_mods.IconsResult (E6 S6.3)


@dataclass(frozen=True)
class GpuProbed:
    answer: str             # 'yes', 'no' or 'unknown' (S5.1)


@dataclass(frozen=True)
class Finished:
    outcome: Outcome
    exit_code: int | None
    app: str | None
    error: str | None
    log_path: str | None
    termination: str | None = None              # S1.8b: 'exit <n>' or 'signal <NAME>'; None when nothing ran
    signals_sent: tuple = ()                    # the signal numbers killpg accepted (S1.8a)
    cancel_compatible: bool | None = None       # CANCELLED only: the ending matches a signal in signals_sent
    observed: Outcome | None = None             # CANCELLED only: the outcome the run would have had without the request


SIGNAL_NAMES = {s.value: s.name for s in signal.Signals}


def termination(exit_code):
    if exit_code is None:
        return None
    if exit_code >= 0:
        return f'exit {exit_code}'
    return f'signal {SIGNAL_NAMES.get(-exit_code, -exit_code)}'


def cancel_compatible(ending, signals_sent):
    """True only when the ending is one a signal in signals_sent can cause; 'compatible' is the strongest claim."""
    if ending in ('signal SIGINT', 'exit 130'):
        return signal.SIGINT in signals_sent
    if ending == 'signal SIGKILL':
        return signal.SIGKILL in signals_sent
    return False


class BuildSession:
    def __init__(self, expected_game):
        self.expected_game = expected_game
        self.game = None            # the game build_app.py announced
        self._botw_version = None
        self._update_version = None
        self._app = None
        self._mismatch = None       # the other game, once announced
        self._error_lines = None    # None until an ERROR: line

    @property
    def version(self):
        if self.game == 'botw':
            return self._botw_version
        if self.game == 'totk':
            return self._update_version or '1.0.0'
        return None

    def feed(self, event):
        if isinstance(event, ProgressHeartbeat):
            return [Heartbeat(event.minutes)]
        out = [LogLine(event.raw)]
        if isinstance(event, Stage):
            out.insert(0, StageChanged(event.index, event.total, event.name, event.text))
        elif isinstance(event, GameIdentified):
            out += self._identified(event)
        elif isinstance(event, UpdateIdentified):
            self._update_version = event.version
        elif isinstance(event, Done):
            self._app = event.app
        elif isinstance(event, Failure):
            self._error_lines = [event.text]
        elif isinstance(event, Line) and self._error_lines is not None:
            self._error_lines.append(event.text)
        return out

    def _identified(self, event):
        if event.game == self.expected_game:
            self.game = event.game
            if event.game == 'botw':
                self._botw_version = event.version
            return []
        if self._mismatch is None:
            self._mismatch = event.game
            return [GameMismatch(self.expected_game, event.game)]
        return []

    def finish(self, exit_code, cancel_requested, start_error=None, output_complete=True, read_error=None,
               log_path=None, signals_sent=()) -> Finished:
        """cancel_requested is the player's request only; a GameMismatch auto-cancel keeps its own outcome."""
        ending, sent = termination(exit_code), tuple(signals_sent)
        def done(outcome, error=None, app=None, compatible=None, observed=None):
            return Finished(outcome, exit_code, app, error, log_path, ending, sent, compatible, observed)
        if start_error is not None:
            return done(Outcome.START_FAILED, str(start_error))
        if self._mismatch is not None:
            return done(Outcome.GAME_MISMATCH, f'expected {self.expected_game}, found {self._mismatch}')
        if cancel_requested:
            if ending is None:                      # cancelled before anything ran: nothing to judge
                return done(Outcome.CANCELLED)
            observed = self._ending(exit_code, output_complete, read_error)[0]
            return done(Outcome.CANCELLED, compatible=cancel_compatible(ending, sent), observed=observed)
        return done(*self._ending(exit_code, output_complete, read_error))

    def _ending(self, exit_code, output_complete, read_error):
        """(outcome, error, app) without a cancel request: INCOMPLETE, SUCCESS or FAILED."""
        if not output_complete:
            return Outcome.INCOMPLETE, None if read_error is None else str(read_error), None
        if exit_code == 0 and self._app is not None:
            return Outcome.SUCCESS, None, self._app
        if self._error_lines is not None:
            return Outcome.FAILED, '\n'.join(self._error_lines), None
        return Outcome.FAILED, f'exit code {exit_code}', None
