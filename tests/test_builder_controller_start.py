"""tools/builder/controller_start.py (S5.1): the Settings tab's part of a real BuildController, with InlineJobs: jobs
on load_jobs, one at a time, the GPU probe once, Play only when idle, events and errors. Base comes from the S1.7b
controller tests; the model's own behaviour is in test_builder_start_options.py."""
import unittest
from unittest.mock import patch

from tools.builder import game_state, start_options
from tools.builder.jobs import InlineJobs
from tools.builder.session import GpuProbed, PlayDone, ProcessError, StartStateReady
from tools.builder.start_options import StartPaths
from test_builder_controller import Base


class Start(Base):
    def setUp(self):
        super().setUp()
        self.load_jobs = self.ctl.load_jobs = InlineJobs()
        self.ctl.start.paths = StartPaths(self.tmp / 'Builder/start-options.json', self.tmp / 'data')
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'))
        self.events()
        self.app = (self.tmp / 'Tears of the Kingdom.app').resolve()       # effective_paths resolves
        (self.app / 'Contents/Resources/totk/extracted/v143').mkdir(parents=True)

    def run_load(self):
        self.load_jobs.run_next()
        self.ctl.tick()
        return self.events()

    def test_the_state_runs_off_the_worker_for_the_chosen_games_app(self):
        self.ctl.start.state()
        self.assertEqual((self.jobs.pending(), self.load_jobs.pending(), self.ctl.start.busy), (0, 1, True))
        [event] = self.run_load()
        self.assertIsInstance(event, StartStateReady)
        self.assertEqual((event.state.app, event.state.version, self.ctl.start.busy), (self.app, 'v143', False))

    def test_the_output_app_wins_over_the_default_name(self):
        out = self.tmp / 'Mine.app'
        self.ctl.set_paths(out=str(out))
        self.events()
        self.ctl.start.state()
        self.assertEqual(self.run_load()[0].state.app, out.resolve())

    def test_one_job_at_a_time(self):
        self.ctl.start.state()
        self.assertRaises(RuntimeError, self.ctl.start.choose, 'interpolation', 'interp')

    def test_a_choice_comes_back_as_the_new_state(self):
        self.ctl.start.choose('interpolation', 'interp')
        self.assertEqual(self.run_load()[0].state.choices['interpolation'], 'interp')

    def test_play_needs_an_idle_controller(self):
        self.started()
        self.assertRaises(RuntimeError, self.ctl.start.play)

    def test_play_comes_back_as_play_done(self):
        with patch.object(start_options, 'play', return_value=start_options.PlayResult(True)) as play:
            self.ctl.start.play()
            self.assertEqual(self.run_load(), [PlayDone(start_options.PlayResult(True))])
        self.assertEqual(play.call_args.args[2:4], (self.app, self.ctl.start.paths))

    def test_errors_are_reported_by_kind(self):
        with patch.object(start_options, 'choose', side_effect=OSError('disk')):
            self.ctl.start.choose('ground', '16')
            self.assertEqual(self.run_load(), [ProcessError('settings', "OSError('disk')")])

    def test_the_gpu_is_probed_once(self):
        with patch.object(game_state, 'probe_metal4fx', return_value='no') as probe:
            self.ctl.start.probe()
            self.ctl.start.probe()
            self.assertEqual(self.ctl.start.gpu, 'checking')
            self.assertEqual(self.run_load(), [GpuProbed('no')])
            self.ctl.start.probe()
        self.assertEqual((probe.call_count, self.ctl.start.gpu, self.load_jobs.pending()), (1, 'no', 0))

    def test_no_game_file_gives_no_app(self):
        self.ctl.set_paths(game=None)
        self.events()
        self.ctl.start.state()
        self.assertEqual(self.run_load()[0].state.reason, 'no_game_file')


if __name__ == '__main__':
    unittest.main()
