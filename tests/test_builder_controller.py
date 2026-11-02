"""tools/builder/controller.py (S1.7b): selection, checks one per job, preparation, start, cancel, close. Fakes for
the checks, the process and git; InlineJobs, except for the test that the view's thread never blocks."""
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from builder_fake_process import Clock, FakeProcess, T, drain
from builder_fakes import FakeRunner, fail, ok
from tools.builder.checks import CheckContext, CheckResult, Status, iter_checks
from tools.builder.process import State
from tools.builder.config import SavedPaths, Settings, SettingsStore
from tools.builder.controller import BuildController, reserve_log
from tools.builder.jobs import InlineJobs, Jobs
from tools.builder.ports import Outcome as CommandOutcome
from tools.builder.session import (BuildSession, CheckResultReady, ChecksDone, ChecksInvalidated, Finished, Outcome,
                                   PathsIncomplete, ProcessError, SettingsLoaded, StageChanged)


class Check:
    def __init__(self, id, status=Status.OK, detail='', sleep=0.0):
        self.id, self.requires, self.status, self.detail, self.sleep, self.runs = id, (), status, detail, sleep, 0

    def run(self, ctx):
        self.runs += 1
        time.sleep(self.sleep)
        return CheckResult(self.id, self.status, 'ok', self.detail)


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.jobs, self.procs = InlineJobs(), []
        self.checks = [Check('build_python', detail='/opt/py'), Check('a'), Check('b')]
        self.git = FakeRunner({lambda argv: argv[0] == 'git': ok('cafe123\n')})
        self.store = SettingsStore(self.tmp / 'settings.json', Clock())
        self.ctl = self.controller(self.jobs)

    def controller(self, jobs, checks=None):
        def factory():
            self.procs.append(FakeProcess())
            return self.procs[-1]
        return BuildController(base_context=CheckContext(runner=None), system_checks=checks or self.checks,
                               input_checks=(), store=self.store, process_factory=factory, jobs=jobs, runner=self.git,
                               clock=Clock(), repo_root=self.tmp, logs_dir=self.tmp / 'logs')

    def events(self):
        return self.ctl.take_events()

    def ready(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'))
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        self.assertIn(ChecksDone(True), self.events())

    def started(self):
        self.ready()
        self.ctl.start_build()
        drain(self.jobs, self.ctl.tick)
        return self.procs[-1]


class Checks(Base):
    def test_keys_missing(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game='/g.xci')
        self.ctl.run_checks()
        self.assertEqual((self.events(), self.jobs.pending()), ([PathsIncomplete(('keys',))], 0))

    def test_one_check_per_job(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game='/g.xci', keys='/k')
        self.ctl.run_checks()
        for n in range(3):
            self.jobs.run_next()
            self.ctl.tick()
            self.assertEqual([c.runs for c in self.checks], [1] * (n + 1) + [0] * (2 - n))
        drain(self.jobs, self.ctl.tick)
        events = self.events()
        self.assertEqual((sum(isinstance(e, CheckResultReady) for e in events), events[-1]), (3, ChecksDone(True)))

    def test_a_change_mid_run_invalidates_it(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game='/g.xci', keys='/k')
        self.ctl.run_checks()
        self.jobs.run_next()
        self.ctl.tick()
        self.ctl.set_paths(keys='/other')                    # while check 2 is queued
        self.jobs.run_next()
        self.ctl.tick()
        events = self.events()
        self.assertIn(ChecksInvalidated(), events)
        self.assertEqual((sum(isinstance(e, CheckResultReady) for e in events), self.jobs.pending()), (1, 0))
        self.assertRaises(RuntimeError, self.ctl.start_build)


class StartRefusals(Base):
    def test_refusals(self):
        self.ctl.select_game('totk')
        self.assertRaises(RuntimeError, self.ctl.start_build)                 # no checks
        self.checks[1].status = Status.BLOCK
        self.ctl.set_paths(game='/g.xci', keys='/k')
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        self.assertRaises(RuntimeError, self.ctl.start_build)                 # a check blocked
        self.checks[1].status = Status.OK
        self.ready()
        self.ctl.set_paths(out=str(self.tmp / 'Other.app'))
        self.assertRaises(RuntimeError, self.ctl.start_build)                 # selection changed after the checks

    def test_preparing_blocks_a_second_start_and_a_failure_frees_it(self):
        self.ready()
        self.ctl.start_build()
        self.assertRaises(RuntimeError, self.ctl.start_build)                 # the request job has not run yet
        (self.tmp / 'logs').mkdir()
        (self.tmp / 'logs').chmod(0o500)                                       # the log cannot be reserved
        self.addCleanup((self.tmp / 'logs').chmod, 0o700)
        drain(self.jobs, self.ctl.tick)
        self.assertEqual([e.outcome for e in self.events() if isinstance(e, Finished)], [Outcome.START_FAILED])
        (self.tmp / 'logs').chmod(0o700)
        self.ctl.start_build()                                                 # allowed again


class Builds(Base):
    def test_success_with_commit_and_the_request_paths(self):
        self.ready()
        self.ctl.start_build()
        self.jobs.run_next()                                                  # the request job
        self.ctl.tick()                                                       # a BuildRun takes over
        self.ctl.set_paths(keys='/changed')                                  # before any save: next build only
        drain(self.jobs, self.ctl.tick)
        proc = self.procs[-1]
        self.assertEqual(proc.started[0][0], '/opt/py')
        self.assertEqual(proc.started[1].name, 'builder-totk-20261023-120000.log')
        proc.emit(T + 'work folder /w (1 GB free on its volume)', T + 'done in 1 min: /A.app')
        proc.end(0)
        drain(self.jobs, self.ctl.tick)
        finished = [e for e in self.events() if isinstance(e, Finished)][0]
        self.assertEqual((finished.outcome, finished.log_path), (Outcome.SUCCESS, str(proc.started[1])))
        saved = self.store.load().settings
        self.assertEqual((saved.last_build.commit, saved.paths['totk'].keys), ('cafe123', str(self.tmp / 'k')))
        self.ctl.close()

    def test_git_failing_gives_unknown(self):
        self.git.script = {lambda argv: argv[0] == 'git': fail(CommandOutcome.NONZERO, 'not a repo')}
        proc = self.started()
        proc.emit(T + 'work folder /w (1 GB free on its volume)', T + 'done in 1 min: /A.app')
        proc.end(0)
        drain(self.jobs, self.ctl.tick)
        self.assertEqual(self.store.load().settings.last_build.commit, 'unknown')

    def test_two_starts_with_the_same_clock_keep_both_logs(self):
        first = reserve_log(self.tmp / 'logs', 'totk', Clock().wall())
        first.write_text('evidence of the first run')
        second = reserve_log(self.tmp / 'logs', 'totk', Clock().wall())
        self.assertEqual((first.name, second.name), ('builder-totk-20261023-120000.log', 'builder-totk-20261023-120000-2.log'))
        self.assertEqual(first.read_text(), 'evidence of the first run')

    def test_cancel_while_preparing_creates_nothing(self):
        self.ready()
        self.ctl.start_build()
        self.ctl.cancel()
        drain(self.jobs, self.ctl.tick)
        finished = [e for e in self.events() if isinstance(e, Finished)]
        self.assertEqual((finished[0].outcome, finished[0].log_path, self.procs), (Outcome.CANCELLED, None, []))
        self.assertFalse((self.tmp / 'logs').exists() and any((self.tmp / 'logs').iterdir()))


class Plan(Base):
    """The remaining cases of the plan: iter_checks step by step, a non-OSError preparation, refusals while a run is
    stuck or a tick raised."""

    def test_iter_checks_runs_one_check_per_next_and_validates_first(self):
        results = iter_checks(self.checks, CheckContext(runner=None))
        self.assertEqual([c.runs for c in self.checks], [0, 0, 0])
        next(results)
        self.assertEqual([c.runs for c in self.checks], [1, 0, 0])
        dup = iter_checks([Check('a'), Check('a')], CheckContext(runner=None))
        self.assertRaises(ValueError, next, dup)

    def test_preparation_error_that_is_not_oserror(self):
        self.ready()
        self.ctl.set_paths(firmware='/fw', shared_fonts='/sf')               # build_argv refuses: ValueError
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        self.ctl.start_build()
        drain(self.jobs, self.ctl.tick)
        events = self.events()
        self.assertEqual([e.outcome for e in events if isinstance(e, Finished)], [Outcome.START_FAILED])
        self.assertEqual([e.where for e in events if isinstance(e, ProcessError)], ['prepare'])
        self.ctl.start_build()                                                 # allowed again

    def test_cancel_failed_is_visible_on_the_controller(self):                  # rewritten in S1.8a
        proc = self.started()
        proc.cancel_error = PermissionError(1, 'Operation not permitted')
        self.ctl.cancel()
        self.assertFalse(self.ctl.cancel_failed)                               # pending until the next poll()
        self.ctl.tick()
        self.assertTrue(self.ctl.cancel_failed)
        proc.cancel_error = None
        self.ctl.cancel()
        self.assertEqual((self.ctl.cancel_failed, proc.cancels), (False, 2))

    def test_refused_while_stuck(self):
        proc = self.started()
        self.ctl.cancel()
        proc.state = State.UNSTOPPABLE
        self.ctl.tick()
        self.assertRaises(RuntimeError, self.ctl.start_build)
        self.assertRaises(RuntimeError, self.ctl.close)

    def test_a_tick_that_raises_keeps_the_build_tracked(self):
        proc = self.started()
        proc.emit(T + '1/4 code: x')
        real, failed = BuildSession.feed, []
        def once(session, event):
            if not failed:
                failed.append(event)
                raise ValueError('once')
            return real(session, event)
        with patch.object(BuildSession, 'feed', once):
            self.assertRaises(ValueError, self.ctl.tick)
            self.assertRaises(RuntimeError, self.ctl.start_build)            # the build is still tracked
            self.ctl.tick()
        self.assertEqual([e.text for e in self.events() if isinstance(e, StageChanged)], ['x'])
        self.assertRaises(RuntimeError, self.ctl.close)


class Close(Base):
    def test_close_refused_while_preparing_running_or_saving(self):
        self.ready()
        self.ctl.start_build()
        self.assertRaises(RuntimeError, self.ctl.close)                       # preparing
        drain(self.jobs, self.ctl.tick)
        self.assertRaises(RuntimeError, self.ctl.close)                       # running
        proc = self.procs[-1]
        proc.emit(T + 'work folder /w (1 GB free on its volume)', T + 'done in 1 min: /A.app')
        proc.end(0)
        self.ctl.tick()
        self.assertRaises(RuntimeError, self.ctl.close)                       # saving: last_build not collected yet
        drain(self.jobs, self.ctl.tick)
        self.ctl.close()
        self.assertIsNotNone(self.store.load().settings.last_build)

    def test_close_idle_mid_check_run(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(game='/g.xci', keys='/k')
        self.ctl.run_checks()
        self.ctl.close()
        self.ctl.tick()
        self.assertNotIn(ChecksDone(True), self.events())
        for call in (self.ctl.run_checks, self.ctl.start_build, self.ctl.load_settings):
            self.assertRaises(RuntimeError, call)


class Settings_(Base):
    def test_load_fills_the_selection(self):
        self.store.save(Settings(paths={'botw': SavedPaths(game='/b.xci', keys='/k')}))
        self.ctl.load_settings()
        drain(self.jobs, self.ctl.tick)
        self.assertEqual(self.events(), [SettingsLoaded(())])
        self.ctl.select_game('botw')
        self.assertEqual(self.ctl.selection(), ('botw', {'game': '/b.xci', 'keys': '/k'}))
        self.assertEqual(self.ctl.take_events(), [])


class NeverBlocks(Base):
    def test_tick_returns_quickly_while_a_check_sleeps(self):
        jobs = Jobs()
        self.addCleanup(jobs.shutdown)
        ctl = self.controller(jobs, checks=[Check('build_python', detail='/opt/py', sleep=1.0)])
        ctl.select_game('totk')
        ctl.set_paths(game='/g.xci', keys='/k')
        ctl.run_checks()
        slowest, deadline = 0.0, time.monotonic() + 10
        while ChecksDone(True) not in ctl.take_events() and time.monotonic() < deadline:
            start = time.monotonic()
            ctl.tick()
            slowest = max(slowest, time.monotonic() - start)
            time.sleep(0.01)
        self.assertLess(slowest, 0.05)


if __name__ == '__main__':
    unittest.main()
