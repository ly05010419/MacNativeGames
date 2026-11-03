"""Test helpers for S1.7b: a BuildProcess with scripted output and states, a clock, and job draining."""
from collections import deque
import signal
import threading
import time

from tools.builder.process import SignalError, State

T = '[12:00:00] '
NOON = time.mktime((2026, 10, 23, 12, 0, 0, 0, 0, -1))     # local time, as the controller formats log names


class FakeProcess:
    """Same methods as BuildProcess. The test pushes output with emit() and ends it with end()."""

    def __init__(self):
        self.batches = deque()
        self.state, self.exit_code, self.finished, self.group_alive = State.RUNNING, None, False, False
        self.started = None
        self.start_hook = None          # called inside start(), on the start job's thread
        self.start_error = self.poll_error = None
        self.cancel_error = None        # S1.8a: while set, each signal is refused (pending until the next poll())
        self.signal_error, self.signals_sent, self._refused = None, (), None
        self.polls = self.cancels = self.closes = 0
        self.threads = {}

    def start(self, argv, log_path):
        self.threads['start'] = threading.get_ident()
        if self.start_hook:
            self.start_hook()
        if self.start_error:
            raise self.start_error
        self.started = (argv, log_path)

    def emit(self, *lines):
        self.batches.append(list(lines))

    def end(self, code, finished=True):
        """Leader reaped and group gone: like BuildProcess, no refusal is left (S1.8d)."""
        self.state, self.exit_code, self.finished = State.EXITED, code, finished
        self.signal_error = self._refused = None

    def poll(self):
        self.polls += 1
        if self._refused is not None:
            self.signal_error, self._refused = self._refused, None
        if self.poll_error:
            error, self.poll_error = self.poll_error, None
            raise error
        return self.batches.popleft() if self.batches else []

    def request_cancel(self):
        """Never raises (S1.8a): the first request or a retry of a settled refusal sends a signal; nothing else does."""
        self.cancels += 1
        self.threads['cancel'] = threading.get_ident()
        if self._refused is not None:
            return
        if self.signal_error is not None and self.state in (State.CANCELLING, State.UNSTOPPABLE):
            sig, self.signal_error = self.signal_error.signal, None
        elif self.state is State.RUNNING:
            sig = signal.SIGINT
        else:
            return
        if self.cancel_error:
            self._refused = SignalError(int(sig), self.cancel_error)
        else:
            self.signals_sent += (int(sig),)          # an int, as BuildProcess stores it (S1.8d)
        if self.state is State.RUNNING:
            self.state = State.CANCELLING

    def refuse_kill(self, error):
        """As the escalation's SIGKILL refused inside poll(): pending until the next poll()."""
        self._refused = SignalError(int(signal.SIGKILL), error)

    def close(self):
        self.closes += 1


class Clock:
    def __init__(self, wall=NOON):
        self.now = wall

    def monotonic(self):
        return self.now

    def wall(self):
        return self.now


def drain(jobs, tick, rounds=20):
    """Run every queued job and tick after each, until nothing is left (bounded)."""
    for _ in range(rounds):
        tick()
        if not jobs.run_next():
            tick()
            return
    raise AssertionError('jobs kept coming')
