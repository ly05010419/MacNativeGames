"""One build's life, from its start job to the last save (S1.7b, R1-R5).

Everything here runs on the view's thread except the job functions (_start, save_paths, save_last_build), which run on
the worker. The BuildProcess is touched on the view's thread only after its start job is done; the cancel Event is the
one object both threads use."""
from collections import deque
from dataclasses import dataclass, replace
from datetime import datetime
from enum import Enum
from pathlib import Path
import signal

from tools.builder.config import LastBuild
from tools.builder.process import State as ProcessState
from tools.builder.progress import parse_line
from tools.builder.session import BuildStarted, CancelStuck, GameMismatch, LogLine, Outcome, ProcessError, SettingsNotSaved

FEED_ATTEMPTS = 3
FEED_PER_TICK = 500


@dataclass(frozen=True)
class BuildRequest:
    game: str
    paths: object               # effective BuildPaths
    saved: object               # the raw SavedPaths to remember
    argv: tuple
    log_path: Path
    commit: str
    python: str
    code: tuple = ()                # S5.4: the code mods composed for this build (mod id, ((file, sha256), ...))
    on_success: object = None       # S5.4: called with the app path as a save job, on this run's success only


class RunState(Enum):
    STARTING = 'starting'
    RUNNING = 'running'
    CANCELLING = 'cancelling'
    STUCK = 'stuck'
    SAVING = 'saving'           # Finished published; an own save job is not collected yet
    ENDED = 'ended'


def collect(job):
    """(value, None) or (None, error) for a done job: the job-collection boundary of R5."""
    try:
        return job.result(), None
    except Exception as e:      # R5: every job error is mapped by its kind and published, never re-raised each tick
        return None, e


def save_paths(store, game, saved):
    """Worker job: remembers one game's paths, keeping everything else in the settings file."""
    settings = store.load().settings
    store.save(replace(settings, paths={**settings.paths, game: saved}))


def save_last_build(store, last_build):
    store.save(replace(store.load().settings, last_build=last_build))


class BuildRun:
    def __init__(self, request, process, session, jobs, store, clock, cancel_event, emit):
        self.request, self.process, self.session = request, process, session
        self.jobs, self.store, self.clock, self.cancel_event, self.emit = jobs, store, clock, cancel_event, emit
        self.state = RunState.STARTING
        self._start_job = jobs.submit(self._start)
        self._save_jobs = []
        self._lines = deque()
        self._failures = 0
        self._output_complete, self._read_error, self._reader_closed = True, None, False
        self._player_cancel = cancel_event.is_set()
        self._mismatch_cancelled = False
        self.cancel_failed = False      # the last signal was refused (S1.8a); cancel() may be called again
        self._published_refusal = None

    @property
    def active(self):
        return self.state is not RunState.ENDED

    def _start(self):
        """Worker job: starts the process unless a cancel came first."""
        if self.cancel_event.is_set():
            return 'skipped'
        self.process.start(list(self.request.argv), self.request.log_path)
        return 'started'

    def cancel(self):
        self.cancel_event.set()
        self._player_cancel = True
        if self.state is RunState.RUNNING or (self.cancel_failed and self.state in (RunState.CANCELLING, RunState.STUCK)):
            self._request_cancel()

    def tick(self):
        self._collect_saves()
        if self.state is RunState.STARTING:
            self._collect_start()
        elif self.state in (RunState.RUNNING, RunState.CANCELLING, RunState.STUCK):
            self._drive()
        if self.state is RunState.SAVING and not self._save_jobs:
            self.state = RunState.ENDED

    def _collect_start(self):
        if not self._start_job.done():
            return
        value, error = collect(self._start_job)
        if error is not None:
            if not isinstance(error, OSError):
                self.emit(ProcessError('start', repr(error)))
            self._finish(self.session.finish(None, False, start_error=error, log_path=str(self.request.log_path)))
        elif value == 'skipped':
            self._finish(self.session.finish(None, True, log_path=str(self.request.log_path)))
        else:
            self.emit(BuildStarted(tuple(self.request.argv), str(self.request.log_path)))
            self._save_jobs.append(self.jobs.submit(save_paths, self.store, self.request.game, self.request.saved))
            self.state = RunState.RUNNING
            if self._player_cancel:
                self._request_cancel()

    def _request_cancel(self):
        """S1.8a: the intent is recorded at once (CANCELLING; STUCK stays). A refused signal does not raise: the
        process keeps it, and the next poll() shows it as signal_error, which _publish_refusal reports."""
        self.process.request_cancel()
        self.cancel_failed = self.process.signal_error is not None
        if self.state is not RunState.STUCK:
            self.state = RunState.CANCELLING

    def _publish_refusal(self):
        """Read after every poll(): a settled refusal is published once, by object identity, not by its text."""
        refusal = self.process.signal_error
        self.cancel_failed = refusal is not None
        if refusal is not None and refusal is not self._published_refusal:
            self._published_refusal = refusal
            self.emit(ProcessError('cancel' if refusal.signal == signal.SIGINT else 'kill', str(refusal.error)))

    def _drive(self):
        if not self._lines and not self._reader_closed:
            try:
                self._lines.extend(self.process.poll())
            except OSError as e:
                self.emit(ProcessError('poll', str(e)))
                self._incomplete(f'the log could not be read: {e}')
                self._reader_closed = True
                self.process.close()
            self._publish_refusal()
        elif not self._lines:
            self.process.poll()                     # the reader is closed: this still reaps and escalates
            self._publish_refusal()
        self._feed_lines()
        if self.process.state is ProcessState.UNSTOPPABLE and self.state is not RunState.STUCK:
            self.state = RunState.STUCK
            self.emit(CancelStuck())
        if not self._lines and self._ended():
            self._finish(self.session.finish(self.process.exit_code, self._player_cancel,
                                             output_complete=self._output_complete, read_error=self._read_error,
                                             log_path=str(self.request.log_path),
                                             signals_sent=self.process.signals_sent))
            self.process.close()

    def _ended(self):
        if self._reader_closed:
            return self.process.state is ProcessState.EXITED and not self.process.group_alive
        return self.process.finished

    def _feed_lines(self):
        """R4: a line leaves the queue only after its events are queued; a failing line is retried next tick. At most
        FEED_PER_TICK lines per call, so a burst cannot hold the window; the rest waits for the next tick."""
        for _ in range(FEED_PER_TICK):
            if not self._lines:
                break
            event = parse_line(self._lines[0])
            try:
                out = self.session.feed(event)
            except Exception as e:              # R4: re-raised twice, then the line is published plainly
                self._failures += 1
                if self._failures < FEED_ATTEMPTS:
                    raise
                out = [LogLine(event.raw), ProcessError('feed', repr(e))]
                self._incomplete(f'a line could not be processed: {e}')
            self._failures = 0
            self._lines.popleft()
            for view_event in out:
                self.emit(view_event)
                if isinstance(view_event, GameMismatch) and not self._mismatch_cancelled:
                    self._mismatch_cancelled = True
                    self._request_cancel()

    def _incomplete(self, reason):
        self._output_complete = False
        self._read_error = self._read_error or reason

    def _finish(self, finished):
        self.emit(finished)
        if finished.outcome is Outcome.SUCCESS:
            wall = datetime.fromtimestamp(self.clock.wall()).astimezone().isoformat(timespec='seconds')
            last = LastBuild(self.request.game, self.session.version or 'unknown', finished.app, self.request.commit, wall)
            self._save_jobs.append(self.jobs.submit(save_last_build, self.store, last))
            if self.request.on_success is not None:
                self._save_jobs.append(self.jobs.submit(self.request.on_success, finished.app))
        self.state = RunState.SAVING if self._save_jobs else RunState.ENDED

    def _collect_saves(self):
        for job in [j for j in self._save_jobs if j.done()]:
            self._save_jobs.remove(job)                 # removed before its result is read (R5)
            _, error = collect(job)
            if error is not None:
                self.emit(SettingsNotSaved(str(error)))
                if not isinstance(error, OSError):
                    self.emit(ProcessError('save', repr(error)))
