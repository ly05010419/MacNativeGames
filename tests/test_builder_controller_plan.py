"""tools/builder/controller.py, S2.1's declared additions: idle (T8) and planned_command() (T6), on a real
BuildController with InlineJobs and FakeProcess. Base comes from the S1.7b controller tests."""
import unittest
from unittest.mock import patch

from builder_fake_process import drain
from tools.builder import background
from tools.builder.background import Background
from tools.builder.jobs import InlineJobs
from test_builder_controller import Base
from tools.builder.session import BackgroundChanged, ChecksDone, ChecksInvalidated, Finished, ProcessError


class Idle(Base):
    def test_idle_through_a_build(self):
        self.assertTrue(self.ctl.idle)
        self.ready()
        self.ctl.start_build()
        self.assertFalse(self.ctl.idle)                         # preparing
        self.jobs.run_next()
        self.ctl.tick()
        self.assertFalse(self.ctl.idle)                         # the run is STARTING
        self.jobs.run_next()
        self.ctl.tick()
        proc = self.procs[-1]
        self.assertFalse(self.ctl.idle)                         # RUNNING
        proc.end(0)
        self.ctl.tick()
        self.assertTrue(any(isinstance(e, Finished) for e in self.events()))
        self.assertFalse(self.ctl.idle)                         # SAVING: the paths save job is not collected yet
        self.assertRaises(RuntimeError, self.ctl.close)
        drain(self.jobs, self.ctl.tick)
        self.assertTrue(self.ctl.idle)


class Planned(Base):
    def test_planned_command_is_what_start_build_runs(self):
        self.ready()
        planned = self.ctl.planned_command()
        self.ctl.start_build()
        drain(self.jobs, self.ctl.tick)
        self.assertEqual(self.procs[-1].started[0], list(planned))
        self.assertEqual(planned[0], '/opt/py')                 # the build Python from the build_python check

    def test_planned_command_needs_fresh_checks(self):
        self.assertRaises(RuntimeError, self.ctl.planned_command)
        self.ready()
        self.ctl.set_paths(keys=str(self.tmp / 'other'))       # the selection changed: the checks are stale
        self.assertRaises(RuntimeError, self.ctl.planned_command)

    def test_planned_command_after_close(self):                                 # S2.1b, H4
        self.ready()
        self.ctl.close()
        self.assertRaises(RuntimeError, self.ctl.planned_command)


class Invalidation(Base):                                                       # S2.3, revision 2 point 1
    def test_ready_checks_are_dropped_with_an_event(self):
        self.ready()
        self.ctl.set_paths(keys=str(self.tmp / 'other'))
        self.assertEqual(self.events(), [ChecksInvalidated()])
        self.ctl.set_paths(keys=str(self.tmp / 'again'))       # nothing ready any more: no second event
        self.assertEqual(self.events(), [])

    def test_no_event_before_any_checks(self):
        self.ctl.select_game('totk')
        self.ctl.set_paths(keys='/k')
        self.assertEqual(self.events(), [])


class StuckLoad(Base):                                                          # S3.4, f4
    def test_checks_run_while_the_load_waits_and_a_late_load_changes_no_choice(self):
        load_jobs = InlineJobs()
        ctl = self.controller(self.jobs)
        ctl.load_jobs = load_jobs                               # the load waits on its own queue, never run
        ctl.load_settings()
        ctl.select_game('totk')
        ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'))
        ctl.run_checks()
        drain(self.jobs, ctl.tick)
        self.assertIn(ChecksDone(True), ctl.take_events())     # the worker was free
        chosen = ctl.selection()
        load_jobs.run_next()
        ctl.tick()
        self.assertEqual(ctl.selection(), chosen)
        self.assertEqual(ctl.planned_command()[0], '/opt/py')    # still ready: the late load invalidated nothing


class Backgrounds(Base):                                                        # S4.3
    def change(self, *args):
        self.ctl.background(*args)
        self.assertTrue(self.ctl.background_busy)
        self.assertEqual((self.jobs.pending(), self.load_jobs.pending()), (0, 1))     # off the worker
        self.load_jobs.run_next()
        self.ctl.tick()
        self.assertFalse(self.ctl.background_busy)
        return self.events()

    def setUp(self):
        super().setUp()
        self.load_jobs = self.ctl.load_jobs = InlineJobs()

    def test_each_action_reaches_the_model_with_the_runner_and_the_settings_folder(self):
        for action, args in (('saved', ((64, 36),)), ('save', ('/mine.png', (64, 36))), ('remove', ()),
                             ('focus', ((64, 36), 'left'))):
            with self.subTest(action=action), patch.object(background, action, return_value=Background(None)) as fn:
                self.assertEqual(self.change(action, *args), [BackgroundChanged(Background(None))])
                fn.assert_called_once_with(self.ctl.runner, self.tmp, *args)

    def test_one_at_a_time(self):
        self.ctl.background('saved', (64, 36))
        self.assertRaises(RuntimeError, self.ctl.background, 'remove')

    def test_an_error_is_reported(self):
        with patch.object(background, 'save', side_effect=OSError('disk')):
            self.assertEqual(self.change('save', '/x.png', (64, 36)), [ProcessError('background', "OSError('disk')")])


if __name__ == '__main__':
    unittest.main()
