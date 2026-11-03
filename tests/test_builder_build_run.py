"""tools/builder/build_run.py (S1.7b): one build's life with a scripted process, InlineJobs and a real settings
store in a temporary folder."""
from concurrent.futures import CancelledError
import errno
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from builder_fake_process import Clock, FakeProcess, T
from tools.builder.build_run import BuildRequest, BuildRun, RunState
from tools.builder.config import SavedPaths, SettingsStore
from tools.builder.jobs import InlineJobs, Jobs
from tools.builder.process import State
from tools.builder.session import (BuildSession, BuildStarted, CancelStuck, Finished, GameMismatch, LogLine, Outcome,
                                   ProcessError, SettingsNotSaved, StageChanged)

WORK = T + 'work folder /w/TOTK_build (400 GB free on its volume)'
DONE = T + 'done in 9 min: /A/Tears of the Kingdom.app'
BOTW = T + 'Breath of the Wild 1.6.0; work folder /w'


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.store = SettingsStore(self.tmp / 'settings.json', Clock())
        self.jobs, self.events, self.proc = InlineJobs(), [], FakeProcess()
        self.cancel = threading.Event()
        self.request = BuildRequest('totk', None, SavedPaths(game='/g.xci', keys='/k'), ('py', 'build_app.py'),
                                    self.tmp / 'b.log', 'abc123', 'py')
        self.run = BuildRun(self.request, self.proc, BuildSession('totk'), self.jobs, self.store, Clock(), self.cancel,
                            self.events.append)

    def started(self):
        self.jobs.run_next()
        self.run.tick()
        self.assertEqual(self.run.state, RunState.RUNNING)

    def finish_jobs(self):
        while self.jobs.run_next():
            self.run.tick()
        self.run.tick()

    def of(self, kind):
        return [e for e in self.events if isinstance(e, kind)]


class Lifecycle(Base):
    def test_success_saves_paths_and_last_build_then_ends(self):
        self.started()
        self.assertEqual(self.of(BuildStarted), [BuildStarted(('py', 'build_app.py'), str(self.tmp / 'b.log'))])
        self.proc.emit(WORK, T + '1/4 code: x', DONE)
        self.proc.end(0)
        self.run.tick()
        self.assertEqual(self.of(Finished)[0].outcome, Outcome.SUCCESS)
        self.assertEqual((self.run.state, self.run.active, self.proc.closes), (RunState.SAVING, True, 1))
        self.finish_jobs()
        self.assertEqual(self.run.state, RunState.ENDED)
        saved = self.store.load().settings
        self.assertEqual(saved.paths['totk'], SavedPaths(game='/g.xci', keys='/k'))
        last = saved.last_build
        self.assertEqual((last.version, last.app, last.commit), ('1.0.0', '/A/Tears of the Kingdom.app', 'abc123'))
        self.assertEqual(self.of(Finished)[0].log_path, str(self.tmp / 'b.log'))

    def test_failure_saves_no_last_build(self):
        self.started()
        self.proc.emit('ERROR: boom')
        self.proc.end(1)
        self.run.tick()
        self.finish_jobs()
        self.assertEqual((self.of(Finished)[0].outcome, self.store.load().settings.last_build), (Outcome.FAILED, None))

    def test_player_cancel_and_mismatch_cancel_once(self):
        self.started()
        self.run.cancel()
        self.run.cancel()
        self.proc.emit(BOTW, BOTW)
        self.run.tick()
        self.assertEqual((self.proc.cancels, self.run.state), (2, RunState.CANCELLING))  # player once, mismatch once
        self.proc.end(130)
        self.run.tick()
        self.assertEqual(self.of(Finished)[0].outcome, Outcome.GAME_MISMATCH)
        self.assertEqual(len(self.of(GameMismatch)), 1)

    def test_stuck_is_tracked_until_the_process_ends(self):
        self.started()
        self.run.cancel()
        self.proc.state = State.UNSTOPPABLE
        for _ in range(5):
            self.run.tick()
        self.assertEqual((self.run.state, len(self.of(CancelStuck)), self.of(Finished)), (RunState.STUCK, 1, []))
        self.assertTrue(self.run.active)
        self.proc.end(-9)
        self.run.tick()
        self.assertEqual((self.of(Finished)[0].outcome, self.proc.closes), (Outcome.CANCELLED, 1))


class Errors(Base):
    def test_start_errors(self):
        for error, extra in ((OSError(errno.ENOENT, 'no python'), []), (RuntimeError('twice'), ['start'])):
            with self.subTest(error=error):
                self.setUp()
                self.proc.start_error = error
                self.jobs.run_next()
                self.run.tick()
                self.assertEqual(self.of(Finished)[0].outcome, Outcome.START_FAILED)
                self.assertEqual([e.where for e in self.of(ProcessError)], extra)
                self.assertFalse(self.run.active)

    def test_read_error_after_done_is_incomplete(self):
        self.started()
        self.proc.emit(DONE)
        self.run.tick()
        self.proc.poll_error = OSError(errno.EIO, 'I/O error')
        self.run.tick()
        self.assertEqual((self.of(ProcessError)[0].where, self.proc.closes), ('poll', 1))
        self.proc.end(0, finished=False)                    # exited, group gone, but the log was not read to its end
        self.run.tick()
        self.finish_jobs()
        finished = self.of(Finished)[0]
        self.assertEqual(finished.outcome, Outcome.INCOMPLETE)
        self.assertIn('could not be read', finished.error)
        self.assertIsNone(self.store.load().settings.last_build)

    def test_refused_cancel_is_visible_and_can_be_tried_again(self):          # S1.7b round 2, P1; rewritten in S1.8a
        self.started()
        self.proc.cancel_error = PermissionError(errno.EPERM, 'Operation not permitted')
        self.run.cancel()
        self.assertEqual((self.of(ProcessError), self.run.state, self.run.cancel_failed),
                         ([], RunState.CANCELLING, False))                      # intent recorded; the refusal is pending
        self.run.tick()
        self.assertEqual((self.of(ProcessError)[0].where, self.run.cancel_failed), ('cancel', True))
        self.proc.cancel_error = None
        self.run.cancel()                                                       # the retry is accepted
        self.assertEqual((self.proc.cancels, self.run.state, self.run.cancel_failed), (2, RunState.CANCELLING, False))
        self.proc.end(130)
        self.run.tick()
        self.assertEqual(self.of(Finished)[0].outcome, Outcome.CANCELLED)

    def test_lines_per_tick_are_bounded(self):                                  # S1.7b round 2, follow-up
        self.started()
        self.proc.emit(*(f'line {i}' for i in range(1200)))
        counts = []
        for _ in range(3):
            self.run.tick()
            counts.append(len(self.of(LogLine)))
        self.assertEqual(counts, [500, 1000, 1200])
        self.assertEqual([e.text for e in self.of(LogLine)], [f'line {i}' for i in range(1200)])

    def test_save_errors_end_the_run(self):
        for error, where in ((OSError('disk'), []), (RuntimeError('bug'), ['save']), (CancelledError(), ['save'])):
            with self.subTest(error=error):
                self.setUp()
                self.started()
                with patch.object(self.store, 'save', side_effect=error):
                    self.proc.emit(WORK, DONE)
                    self.proc.end(0)
                    self.run.tick()
                    self.finish_jobs()
                self.assertEqual(len(self.of(SettingsNotSaved)), 2)                  # paths and last_build
                self.assertEqual([e.where for e in self.of(ProcessError)], where * 2)
                self.assertEqual(self.run.state, RunState.ENDED)
                for _ in range(10):
                    self.run.tick()


class Lines(Base):
    def test_a_failing_line_is_retried_without_loss_or_duplicates(self):
        self.started()
        self.proc.emit(T + '1/4 code: a', T + '1/4 code: b', T + '2/4 data: c', T + '3/4 shaders: d')
        real, calls = self.run.session.feed, []
        def flaky(event):
            calls.append(event.raw)
            if event.raw.endswith('b') and calls.count(event.raw) == 1:
                raise ValueError('once')
            return real(event)
        with patch.object(self.run.session, 'feed', side_effect=flaky):
            self.assertRaises(ValueError, self.run.tick)
            self.assertEqual([e.text for e in self.of(StageChanged)], ['a'])
            polls = self.proc.polls
            self.run.tick()
            self.assertEqual(self.proc.polls, polls)                    # no poll while lines were pending
        self.assertEqual([e.text for e in self.of(StageChanged)], ['a', 'b', 'c', 'd'])

    def test_a_line_failing_three_times_is_dropped_and_blocks_success(self):     # revision 6, P1
        self.started()
        self.proc.emit(T + 'bad line', DONE)
        real = self.run.session.feed
        def broken(event):
            if event.raw.endswith('bad line'):
                raise ValueError('bug')
            return real(event)
        with patch.object(self.run.session, 'feed', side_effect=broken):
            self.assertRaises(ValueError, self.run.tick)
            self.assertRaises(ValueError, self.run.tick)
            self.run.tick()
        self.assertIn(LogLine(T + 'bad line'), self.events)
        self.assertEqual(self.of(ProcessError)[0].where, 'feed')
        self.proc.end(0)
        self.run.tick()
        self.finish_jobs()
        self.assertEqual(self.of(Finished)[0].outcome, Outcome.INCOMPLETE)
        self.assertIn('a line could not be processed', self.of(Finished)[0].error)
        self.assertIsNone(self.store.load().settings.last_build)


class CancelBeforeRunning(Base):
    def test_cancel_while_the_start_job_is_queued(self):
        self.run.cancel()
        self.jobs.run_next()
        self.run.tick()
        self.assertEqual((self.proc.started, self.of(Finished)[0].outcome), (None, Outcome.CANCELLED))

    def test_cancel_during_start_is_applied_on_the_next_tick(self):
        self.proc.start_hook = self.run.cancel
        self.jobs.run_next()
        self.assertEqual(self.proc.cancels, 0)
        self.run.tick()
        self.assertEqual((self.proc.cancels, self.run.state), (1, RunState.CANCELLING))

    def test_failed_start_with_a_cancel_pending_is_start_failed(self):
        self.proc.start_hook = self.run.cancel
        self.proc.start_error = OSError('no python')
        self.jobs.run_next()
        self.run.tick()
        self.assertEqual(self.of(Finished)[0].outcome, Outcome.START_FAILED)

    def test_real_thread_cancel_during_start(self):                             # revision 6
        jobs, entered, release = Jobs(), threading.Event(), threading.Event()
        self.addCleanup(jobs.shutdown)
        self.addCleanup(release.set)
        proc = FakeProcess()
        proc.start_hook = lambda: entered.set() or release.wait(10)
        run = BuildRun(self.request, proc, BuildSession('totk'), jobs, self.store, Clock(), threading.Event(),
                       self.events.append)
        self.assertTrue(entered.wait(10))
        run.cancel()
        self.assertEqual(proc.cancels, 0)                   # nothing touched the process while start() ran
        release.set()
        deadline = time.monotonic() + 10
        while run.state is RunState.STARTING and time.monotonic() < deadline:
            run.tick()
            time.sleep(0.005)
        proc.end(130)
        run.tick()
        self.assertEqual((proc.cancels, self.of(Finished)[0].outcome), (1, Outcome.CANCELLED))
        self.assertNotEqual(proc.threads['start'], threading.get_ident())
        self.assertEqual(proc.threads['cancel'], threading.get_ident())


if __name__ == '__main__':
    unittest.main()
