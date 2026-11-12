"""The small interfaces the models depend on, and their real adapters (S1.1b): a clock and a command runner.

The models receive these objects; the caller (tools/builder_jobs/tab_*.py) creates the real ones, and tests pass fakes
with the same methods."""
from dataclasses import dataclass
from enum import Enum
import errno
import os
import signal
import subprocess
import tempfile
import time
from typing import Protocol


class Clock(Protocol):
    def monotonic(self) -> float: ...   # durations
    def wall(self) -> float: ...        # timestamps only, never durations


class RealClock:
    def monotonic(self):
        return time.monotonic()

    def wall(self):
        return time.time()


class Outcome(Enum):
    OK = 'ok'
    NONZERO = 'nonzero'
    NOT_FOUND = 'not_found'
    NOT_EXECUTABLE = 'not_executable'
    TIMEOUT = 'timeout'


@dataclass(frozen=True)
class CommandResult:
    outcome: Outcome
    code: int | None     # the leader's return code (-9 when killed); None when nothing started or it was not reaped
    stdout: str
    stderr: str
    elapsed_s: float
    pid: int | None      # None when nothing started (NOT_FOUND, NOT_EXECUTABLE)
    reaped: bool         # the leader's exit status was collected; False also when nothing started


class CommandRunner:
    """Runs a short check command: output to temporary files, never pipes, so only the leader's exit is waited for.

    The process waits are bounded by timeout_s + kill_wait_s; starting the process and reading its output add to
    that, so run's total duration is not a hard guarantee. reaped=False means the leader did not exit after SIGKILL
    to its group within kill_wait_s: run starts no thread for it, and when (or whether) Python's subprocess cleanup
    collects it later is not promised."""

    def __init__(self, clock: Clock):
        self.clock = clock

    def run(self, argv, timeout_s=10.0, kill_wait_s=2.0) -> CommandResult:
        start = self.clock.monotonic()
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            try:
                proc = subprocess.Popen([str(a) for a in argv], stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                        start_new_session=True)
            except FileNotFoundError as e:
                return self._not_started(Outcome.NOT_FOUND, e, start)
            except PermissionError as e:
                return self._not_started(Outcome.NOT_EXECUTABLE, e, start)
            except OSError as e:      # ENOEXEC: a file the system cannot execute; any other error is the system's
                if e.errno != errno.ENOEXEC:
                    raise
                return self._not_started(Outcome.NOT_EXECUTABLE, e, start)
            code, timed_out, notes = self._wait(proc, timeout_s, kill_wait_s)
            stdout, stderr = _read(out), _read(err)
        if notes:
            stderr += ('' if not stderr or stderr.endswith('\n') else '\n') + '\n'.join(notes) + '\n'
        if timed_out:
            outcome = Outcome.TIMEOUT
        else:
            outcome = Outcome.OK if code == 0 else Outcome.NONZERO
        return CommandResult(outcome, code, stdout, stderr, self.clock.monotonic() - start, proc.pid, code is not None)

    @staticmethod
    def _wait(proc, timeout_s, kill_wait_s):
        """(code or None, timed out, notes for stderr)."""
        try:
            return proc.wait(timeout=timeout_s), False, []
        except subprocess.TimeoutExpired:
            pass
        notes = []
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass                    # the group ended between the timeout and the kill
        except OSError as e:
            notes.append(f'builder: killpg({proc.pid}) failed: {e}')
        try:
            return proc.wait(timeout=kill_wait_s), True, notes
        except subprocess.TimeoutExpired:
            return None, True, notes + [f'builder: could not stop process {proc.pid} within {kill_wait_s} s']

    def _not_started(self, outcome, error, start):
        return CommandResult(outcome, None, '', str(error), self.clock.monotonic() - start, None, False)


def _read(file):
    file.seek(0)
    return file.read().decode('utf-8', errors='replace')
