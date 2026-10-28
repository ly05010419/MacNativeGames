"""The build as a running process: started once, read line by line, cancelled cleanly (S1.5).

The output goes to the run's log file, never to a pipe: poll() reads new lines from that file and learns that the build
ended from its exit status, not from EOF. Nothing here waits for the process and there are no threads: the controller
calls poll() repeatedly, and a requested cancel is escalated by those calls (SIGINT to the process group, then SIGKILL
once the grace period is over).

A refused signal never raises (S1.8a): it is kept pending, and the next poll() either drops it (the target was a zombie,
now reaped and gone) or shows it as signal_error."""
import codecs
from dataclasses import dataclass
from enum import Enum
import os
from pathlib import Path
import signal
import subprocess

READ_LIMIT = 1 << 20        # bytes read per poll()
LINE_LIMIT = 64 * 1024      # characters kept for an unfinished line


class State(Enum):
    RUNNING = 'running'
    CANCELLING = 'cancelling'
    EXITED = 'exited'
    UNSTOPPABLE = 'unstoppable'


@dataclass(frozen=True, eq=False)        # eq=False: two refusals with the same text are still two refusals
class SignalError:
    signal: int
    error: OSError


class BuildProcess:
    def __init__(self, clock):
        self.clock = clock
        self.log_path = None
        self._started = False
        self._proc = None
        self._reader = None
        self._decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        self._pending = ''
        self._drained = False
        self._state = None
        self._group_gone = False       # latched: once seen gone, no signal is sent to this group id again
        self._phase = None             # 'int' or 'kill' while cancelling
        self._deadline = None
        self._kill_wait_s = None
        self._refused = None           # a refused signal not settled by a poll() yet
        self._signal_error = None      # the last settled refusal; cleared by a retry, an accepted signal or the end
        self._signals_sent = []        # the signals killpg accepted, in order

    def start(self, argv, log_path, env=None):
        """Opens the writer and a separate reader, then starts the build. On any error nothing is left running
        and none of our files is open."""
        if self._started:
            raise RuntimeError('a BuildProcess runs one build; make a new one')
        self._started = True
        log_path = Path(log_path)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        writer = open(log_path, 'wb')
        try:
            reader = open(log_path, 'rb')
        except BaseException:
            writer.close()
            raise
        try:
            self._proc = subprocess.Popen([str(a) for a in argv], stdin=subprocess.DEVNULL, stdout=writer,
                                          stderr=subprocess.STDOUT, start_new_session=True,
                                          env={**os.environ, **(env or {}), 'PYTHONUNBUFFERED': '1'})
        except BaseException:
            reader.close()
            writer.close()
            raise
        writer.close()                 # the child keeps its own copy
        self._reader, self.log_path, self._state = reader, log_path, State.RUNNING

    @property
    def pid(self):
        return self._proc.pid if self._proc else None

    @property
    def state(self):
        return self._state

    @property
    def exit_code(self):
        return self._proc.returncode if self._proc else None

    @property
    def finished(self):
        """EXITED, and the output is final: the leader is reaped, the group is gone and the log is read."""
        return self._state is State.EXITED and self._drained

    @property
    def signal_error(self):
        return self._signal_error

    @property
    def signals_sent(self):
        return tuple(self._signals_sent)

    @property
    def group_alive(self):
        return self._proc is not None and not self._probe_group_gone()

    def poll(self):
        """New complete lines; reaps the leader if it exited; advances a requested cancel. Never waits."""
        if not self._started or self._proc is None:
            raise RuntimeError('poll() before a successful start()')
        # The output is final only when nobody in the group can still write: the leader is reaped and the group is
        # gone. Checked before reading, so that a short read afterwards proves the file is drained.
        no_writers = self._proc.poll() is not None and self._probe_group_gone()
        if self._refused is not None:
            if not no_writers:             # leader reaped and group gone: the refused target was a zombie (S1.8c)
                self._signal_error = self._refused
            self._refused = None
        lines = []
        if self._reader is not None and not self._reader.closed:
            data = self._reader.read(READ_LIMIT)
            self._pending += self._decoder.decode(data)
            lines += self._take_lines()
            if no_writers and len(data) < READ_LIMIT and not self._drained:
                self._pending += self._decoder.decode(b'', final=True)
                if self._pending:
                    lines.append(self._pending)
                    self._pending = ''
                self._drained = True
        self._advance(self._proc.returncode)
        return lines

    def _take_lines(self):
        *complete, self._pending = self._pending.split('\n')
        while len(self._pending) > LINE_LIMIT:
            complete.append(self._pending[:LINE_LIMIT])
            self._pending = self._pending[LINE_LIMIT:]
        return complete

    def request_cancel(self, grace_s=10.0, kill_wait_s=5.0):
        """Sends SIGINT to the group and returns; poll() escalates. Also works after a natural end while the
        group is still alive. After a settled refusal it re-sends the current phase's signal once, keeping the
        deadlines. Otherwise it does nothing."""
        if self._refused is not None:
            return
        if self._signal_error is not None and self._state in (State.CANCELLING, State.UNSTOPPABLE):
            self._signal_error = None
            self._signal(signal.SIGINT if self._phase == 'int' else signal.SIGKILL)
            return
        if self._state is State.RUNNING:
            pass
        elif self._state is State.EXITED and not self._probe_group_gone():
            pass
        else:
            return
        self._state, self._phase = State.CANCELLING, 'int'
        self._deadline, self._kill_wait_s = self.clock.monotonic() + grace_s, kill_wait_s
        self._signal(signal.SIGINT)

    def close(self):
        """Closes the reader. Idempotent; does not signal. poll() afterwards still reaps the leader."""
        if self._reader is not None:
            self._reader.close()

    def _advance(self, code):
        if self._state is State.RUNNING:
            if code is not None:
                self._state = State.EXITED
            return
        if self._state not in (State.CANCELLING, State.UNSTOPPABLE):
            return
        gone = self._probe_group_gone()
        if code is not None and gone:
            self._state, self._phase = State.EXITED, None
            self._signal_error = None               # S1.8d: the target is gone, nothing is left to retry
            return
        now = self.clock.monotonic()
        if self._phase == 'int' and now >= self._deadline and not gone:
            self._signal(signal.SIGKILL)
            self._phase, self._deadline = 'kill', now + self._kill_wait_s
        elif self._phase == 'kill' and now >= self._deadline:
            self._state = State.UNSTOPPABLE

    def _probe_group_gone(self):
        if not self._group_gone:
            try:
                os.killpg(self._proc.pid, 0)
            except ProcessLookupError:
                self._group_gone = True
            except PermissionError:
                pass        # S1.8c: EPERM means the group exists; on macOS its only member is a zombie not reaped yet
        return self._group_gone

    def _signal(self, sig):
        if self._group_gone:
            return
        try:
            os.killpg(self._proc.pid, sig)
        except ProcessLookupError:
            self._group_gone = True
        except OSError as e:
            self._refused = SignalError(int(sig), e)
        else:
            self._signals_sent.append(int(sig))
            self._signal_error = None
