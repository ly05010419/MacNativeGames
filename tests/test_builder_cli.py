"""tools/builder/cli.py (S2.1, T5 and T7): the text mode against a scripted controller (builder_fake_controller.py).
Loop turns: 1 is the settings load, 2.. the checks, then one interrupted() before the prompt (S2.1b, g1) and one
before start_build() (S2.2, from S2.1b P3-1); a build's turns follow."""
import signal
import tempfile
import unittest

from collections import deque
from pathlib import Path

from builder_fake_controller import CtrlC, FakeController, cli_args
from tools.builder.checks import CheckResult, Status
from tools.builder.cli import CliView
from tools.builder.session import (BuildStarted, CancelStuck, CheckResultReady, ChecksDone, Finished, Heartbeat,
                                   LogLine, Outcome, PathsIncomplete, ProcessError, SettingsLoaded, SettingsNotSaved,
                                   StageChanged)

READY = [[CheckResultReady(CheckResult('git', Status.OK, 'ok', '/usr/bin/git'))], [ChecksDone(True)]]
LOG = '/logs/builder-totk.log'


def finished(outcome, code, **kw):
    return Finished(outcome, code, kw.pop('app', None), kw.pop('error', None), LOG, **kw)


class Base(unittest.TestCase):
    def view(self, ctl, answer='y', interrupted=None):
        self.ctl, self.lines, self.asked = ctl, [], []
        def ask(prompt):
            self.asked.append(prompt)
            return answer
        return CliView(ctl, self.lines.append, ask, lambda s: None, interrupted or CtrlC())

    def run_cli(self, ctl, answer='y', interrupted=None, **args):
        code = self.view(ctl, answer, interrupted).run(cli_args(**args))
        self.assertEqual(self.ctl.closes, 1)                            # one close(), always, once idle
        return code

    def build(self, *batches, **kw):
        return FakeController(checks=READY, build=[[BuildStarted(('py',), LOG)], *batches], **kw)


class Checks(Base):
    def test_all_ok_and_check_only(self):
        code = self.run_cli(FakeController(checks=READY), check_only=True)
        self.assertEqual((code, self.lines), (0, ['  OK    git', 'Checks: ready.']))
        self.assertNotIn('start_build', self.ctl.calls)

    def test_check_only_prints_the_logs(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / 'builder-totk-20260101-000000.log'
            log.write_text('x')
            ctl = FakeController(checks=READY)
            ctl.logs_dir = Path(folder)
            code = self.run_cli(ctl, check_only=True)
        self.assertEqual((code, self.lines),
                         (0, ['  OK    git', 'Checks: ready.', f'  logs: builder: {log}']))

    def test_a_block_with_a_fix_and_a_skip(self):
        block = CheckResult('homebrew', Status.BLOCK, 'homebrew_missing', '/opt/homebrew/bin/brew', 'install-brew')
        skip = CheckResult('brew_packages', Status.SKIP, 'skipped', 'requires homebrew')
        ctl = FakeController(checks=[[CheckResultReady(block), CheckResultReady(skip)], [ChecksDone(False)]])
        self.assertEqual(self.run_cli(ctl), 2)
        self.assertEqual(self.lines, ['  BLOCK Homebrew: Homebrew is not installed '
                                      '(/opt/homebrew/bin/brew is missing).',
                                      '        fix: install-brew',
                                      '  SKIP  Homebrew packages (opus, libvpx): Not checked (requires homebrew).',
                                      'Checks: blocked. Nothing was started.'])

    def test_paths_incomplete(self):
        self.assertEqual(self.run_cli(FakeController(checks=[[PathsIncomplete(('game', 'keys'))]])), 2)
        self.assertEqual(self.lines, ['Missing paths: --game-file, --keys.'])                  # S2.1b: the flags

    def test_only_the_flags_given_are_set(self):
        self.run_cli(FakeController(checks=READY), check_only=True, paths={'keys': '/k'})       # S2.1b: args.paths
        self.assertEqual(self.ctl.calls[:3], ['load_settings', ('select_game', 'totk'), ('set_paths', {'keys': '/k'})])

    def test_settings_problems_and_a_load_error_end_the_load(self):
        self.run_cli(FakeController(load=[[SettingsLoaded(('format_newer',))]], checks=READY), check_only=True)
        self.assertEqual(self.lines[0], '  ! saved settings: format_newer')
        self.run_cli(FakeController(load=[[ProcessError('load', 'boom')]], checks=READY), check_only=True)
        self.assertEqual(self.lines[0], '  ! load: boom')

    def test_dry_run(self):
        self.assertEqual(self.run_cli(FakeController(checks=READY), dry_run=True), 0)
        self.assertEqual(self.lines[-1], 'Command: /py build_app.py totk')
        self.assertNotIn('start_build', self.ctl.calls)
        self.assertEqual(self.run_cli(FakeController(checks=[[ChecksDone(False)]]), dry_run=True), 2)
        self.assertNotIn('planned_command', self.ctl.calls)

    def test_the_prompt(self):
        self.assertEqual(self.run_cli(FakeController(checks=READY), answer='n'), 0)
        self.assertEqual((self.asked, self.lines[-1]), (['Build now? [y/N] '], 'Not started.'))
        self.assertNotIn('start_build', self.ctl.calls)
        self.assertEqual(self.run_cli(self.build([finished(Outcome.SUCCESS, 0, app='/A.app')]), yes=True), 0)
        self.assertEqual(self.asked, [])

    def test_ctrl_c_during_the_checks(self):
        self.assertEqual(self.run_cli(FakeController(checks=READY), interrupted=CtrlC(2)), 130)
        self.assertEqual((self.lines, 'start_build' in self.ctl.calls), (['Interrupted. Nothing was started.'], False))

    def test_ctrl_c_at_the_prompt(self):
        ctl = FakeController(checks=READY)
        view = self.view(ctl)
        def ask(prompt):
            raise KeyboardInterrupt
        view.ask = ask
        self.assertEqual((view.run(cli_args()), ctl.closes, 'start_build' in ctl.calls), (130, 1, False))


class Build(Base):
    def test_success(self):
        code = self.run_cli(self.build([StageChanged(2, 4, 'data', 'extracting'), Heartbeat(3)],
                                       [LogLine('x'), finished(Outcome.SUCCESS, 0, app='/A.app')]), yes=True)
        self.assertEqual(code, 0)
        self.assertEqual(self.lines[2:], [f'Started. Log: {LOG}', '[2/4 data] extracting',
                                          '  ... still running (3 min)',
                                          'The build finished.', 'App: /A.app', f'Log: {LOG}'])

    def test_failed_shows_the_error_and_the_last_lines(self):
        logs = [LogLine(f'l{i}') for i in range(25)]
        code = self.run_cli(self.build(logs, [finished(Outcome.FAILED, 1, error='boom')]), yes=True)
        self.assertEqual(code, 1)
        tail = self.lines[self.lines.index('Last 20 log lines:') + 1:]
        self.assertEqual(tail, [f'l{i}' for i in range(5, 25)] + [f'Log: {LOG}'])
        self.assertIn('Error: boom', self.lines)

    def test_exit_codes(self):
        for outcome, expected in ((Outcome.START_FAILED, 3), (Outcome.INCOMPLETE, 1), (Outcome.GAME_MISMATCH, 1)):
            with self.subTest(outcome=outcome):
                self.assertEqual(self.run_cli(self.build([finished(outcome, None)]), yes=True), expected)

    def test_stuck_keeps_waiting_for_finished(self):
        end = finished(Outcome.CANCELLED, -9, termination='signal SIGKILL', signals_sent=(9,), cancel_compatible=True)
        self.assertEqual(self.run_cli(self.build([CancelStuck()], [], [], [end]), yes=True), 130)
        stuck = 'The build did not stop even after SIGKILL. It is still tracked; waiting for it to end.'
        self.assertIn(stuck, self.lines)

    def test_waits_for_idle_before_closing(self):
        ctl = self.build([finished(Outcome.SUCCESS, 0, app='/A.app')], saving_ticks=3)
        self.assertEqual(self.run_cli(ctl, yes=True), 0)                  # the fake close() raises while busy

    def test_idle_without_finished(self):
        self.assertEqual(self.run_cli(self.build([]), yes=True), 1)
        self.assertEqual(self.lines[-1], '  ! the build ended without a final report')


class CtrlCDuringBuild(Base):
    END = finished(Outcome.CANCELLED, 130, termination='exit 130', signals_sent=(2,), cancel_compatible=True)

    def test_one_ctrl_c_cancels_once_and_waits(self):
        code = self.run_cli(self.build([], [], [self.END]), yes=True, interrupted=CtrlC(6))
        self.assertEqual((code, self.ctl.calls.count('cancel')), (130, 1))
        self.assertIn('Cancelling; waiting for the build to stop.', self.lines)

    def test_a_later_ctrl_c_without_a_refusal(self):
        self.run_cli(self.build([], [], [], [self.END]), yes=True, interrupted=CtrlC(6, 7))
        self.assertEqual(self.ctl.calls.count('cancel'), 1)
        self.assertIn('The build is still stopping; waiting.', self.lines)

    def test_a_later_ctrl_c_after_a_refusal_retries(self):
        ctl = self.build([ProcessError('kill', 'Operation not permitted')], [], [], [self.END])
        hooks = {7: lambda: setattr(ctl, 'cancel_failed', True)}
        self.run_cli(ctl, yes=True, interrupted=CtrlC(6, 7, hooks=hooks))
        self.assertEqual(ctl.calls.count('cancel'), 2)
        self.assertIn('Retrying the cancel.', self.lines)
        hint = self.lines.index('  ! kill: Operation not permitted')
        self.assertEqual(self.lines[hint + 1], '    press Ctrl-C again to retry')


class Hardening(Base):                                                          # S2.1b
    def test_a_ctrl_c_in_the_last_check_turn_starts_nothing(self):             # g1: turn 4 follows the checks
        self.assertEqual(self.run_cli(FakeController(checks=READY), yes=True, interrupted=CtrlC(4)), 130)
        self.assertEqual(self.lines[-1], 'Interrupted. Nothing was started.')
        self.assertNotIn('start_build', self.ctl.calls)

    def test_a_ctrl_c_around_the_prompt_starts_nothing(self):    # S2.2, S2.1b P3-1: the flag is set at the turn after
        # the prompt (turn 5), which is what a Ctrl-C during ask() or the print before it leaves behind
        self.assertEqual(self.run_cli(FakeController(checks=READY), answer='y', interrupted=CtrlC(5)), 130)
        self.assertEqual((self.asked, 'start_build' in self.ctl.calls), (['Build now? [y/N] '], False))

    def test_a_stuck_load_goes_on_after_10_s(self):                            # S3.4, f4
        self.assertEqual(self.run_cli(FakeController(load=[], checks=READY), check_only=True), 0)
        self.assertEqual(self.lines[:2], ['  ! the saved settings did not answer within 10 s: going on without them',
                                          '  OK    git'])

    def test_a_checks_error_is_printed(self):
        ctl = FakeController(checks=[[ProcessError('checks', 'boom'), ChecksDone(False)]])
        self.assertEqual(self.run_cli(ctl), 2)
        self.assertEqual(self.lines, ['  ! checks: boom', 'Checks: blocked. Nothing was started.'])

    def test_settings_not_saved_while_waiting_for_idle(self):
        ctl = self.build([finished(Outcome.SUCCESS, 0, app='/A.app')], [SettingsNotSaved('disk full')])
        self.assertEqual(self.run_cli(ctl, yes=True), 0)
        self.assertIn('  ! settings not saved: disk full', self.lines)

    def test_tick_errors_are_shown_once_and_the_build_goes_on(self):
        ctl = self.build([finished(Outcome.SUCCESS, 0, app='/A.app')])
        ctl.tick_errors = deque([ValueError('feed'), ValueError('feed')])
        self.assertEqual(self.run_cli(ctl, yes=True), 0)
        self.assertEqual(self.lines.count("  ! tick: ValueError('feed')"), 1)

    def test_a_ctrl_c_after_repeated_tick_errors_stops_tracking(self):       # g2
        ctl = self.build([finished(Outcome.SUCCESS, 0)])
        ctl.tick_errors = deque(ValueError('stuck') for _ in range(10))
        code = self.view(ctl, interrupted=CtrlC(9)).run(cli_args(yes=True))  # turns 6-8 fail, the Ctrl-C at 9
        self.assertEqual((code, ctl.closes), (1, 0))                            # a busy controller is not closed
        self.assertEqual(self.lines[-1], 'Stopped tracking the build after repeated errors; build_app.py may still '
                                         'be running. Log: unknown')


class CancelledSentence(Base):
    def sentence(self, **kw):
        self.run_cli(self.build([finished(Outcome.CANCELLED, kw.pop('code', None), **kw)]), yes=True)
        return self.lines[-2]

    def test_the_three_sentences(self):
        self.assertEqual(self.sentence(), 'Cancelled before the build started.')
        self.assertEqual(self.sentence(code=-2, termination='signal SIGINT', signals_sent=(int(signal.SIGINT),),
                                       cancel_compatible=True),
                         'Cancelled: the build ended with signal SIGINT, which matches a signal that was accepted '
                         '(SIGINT).')
        self.assertEqual(self.sentence(code=0, termination='exit 0', signals_sent=(2, 99), cancel_compatible=False,
                                       observed=Outcome.SUCCESS),
                         'Cancel requested; the build ended with exit 0, which no accepted signal explains '
                         '(SIGINT, 99). Without the request it would have been: success.')


if __name__ == '__main__':
    unittest.main()
