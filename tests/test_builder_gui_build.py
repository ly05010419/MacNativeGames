"""tools/builder/gui_build.py and gui.py (S2.3, R2-R6): build, cancel, done and closing while busy, on a real withdrawn
Tk root (builder_tk_base.py) against the scripted FakeController. Each pump delivers one scripted batch."""
import io
import os
import signal
import time
import unittest

from builder_tk_base import Base, tk
from tools.builder.checks import CheckResult, Status
from tools.builder.session import (BuildStarted, CancelStuck, CheckResultReady, ChecksDone, Finished, LogLine,
                                   Outcome, ProcessError, SettingsNotSaved, StageChanged)

if tk is not None:
    from tools.builder.gui import exit_signals, settle, wire_quit

READY = [[CheckResultReady(CheckResult('git', Status.OK, 'ok', ''))], [ChecksDone(True)]]
LOG = '/logs/builder-totk.log'


def finished(outcome, code=0, **kw):
    return Finished(outcome, code, kw.pop('app', None), kw.pop('error', None), kw.pop('log', LOG), **kw)


class BuildBase(Base):
    def building(self, *batches, saving_ticks=0):
        """Checks ready, then Build clicked; the build's batches follow BuildStarted."""
        self.start(checks=READY, build=[[BuildStarted(('py',), LOG)], *batches], saving_ticks=saving_ticks)
        self.loaded('totk')
        self.view.refresh()
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.pump()
        self.view.checks.build_button.invoke()

    def pumps(self, n):
        for _ in range(n):
            self.view.pump()

    def shown(self, panel):
        return bool(getattr(self.view, panel).frame.winfo_manager())

    def done_buttons(self):
        return {w.button.cget('text'): w.button for row in self.view.done.body.winfo_children()
                for w in row.winfo_children() if w.winfo_class() == 'FlatButton'}


class Build(BuildBase):
    def test_build_starts_and_everything_is_disabled(self):
        self.building([])
        self.assertIn('start_build', self.ctl.calls)
        self.assertTrue(self.shown('build') and not self.shown('checks'))
        self.assertEqual({self.entry('keys').cget('state'), self.radio('botw').cget('state')}, {'disabled'})

    def test_stage_and_elapsed_time_from_the_click(self):
        self.building([StageChanged(2, 4, 'data', 'extracting')])
        self.now += 75
        self.pumps(2)
        self.assertEqual((self.view.build.stage.cget('text'), self.view.build.elapsed.cget('text')),
                         ('2/4 data: extracting', 'Elapsed: 1 min 15 s'))
        self.assertEqual(self.view.build.log_path.cget('text'), f'Log: {LOG}')

    def test_the_log_keeps_500_lines(self):
        self.building([LogLine(f'l{i}') for i in range(600)])
        self.pumps(2)
        log = self.view.build.log
        self.assertEqual((log.get('1.0', '1.end'), int(log.index('end-1c').split('.')[0]) - 1), ('l100', 500))

    def test_notes(self):
        self.building([ProcessError('kill', 'Operation not permitted'), CancelStuck(), SettingsNotSaved('full')])
        self.pumps(2)
        self.assertEqual(self.view.build.messages.cget('text').splitlines(),
                         ['kill: Operation not permitted',
                          'The build did not stop even after SIGKILL. It is still tracked.',
                          'Settings not saved: full'])


class Cancel(BuildBase):
    def test_cancel_once_then_retry_only_after_a_refusal(self):
        self.building([], [], [])
        button = self.view.build.cancel_button
        button.invoke()
        self.view.pump()
        self.assertEqual((self.ctl.calls.count('cancel'), button.cget('state'), button.cget('text')),
                         (1, 'disabled', 'Cancelling...'))
        self.ctl.cancel_failed = True
        self.view.pump()
        self.assertEqual((button.cget('state'), button.cget('text')), ('normal', 'Retry cancel'))
        button.invoke()
        self.assertEqual(self.ctl.calls.count('cancel'), 2)


class Done(BuildBase):
    def test_success(self):
        self.building([finished(Outcome.SUCCESS, app='/A.app')])
        self.pumps(3)
        self.assertTrue(self.shown('done') and not self.shown('build'))
        self.assertIn('The build finished.', self.labels(self.view.done.body))
        buttons = self.done_buttons()
        buttons['Open app'].invoke()
        buttons['Show in Finder'].invoke()
        buttons['Copy'].invoke()
        self.assertEqual((self.opened, self.copied), ([['open', '/A.app'], ['open', '-R', '/A.app']], [LOG]))

    def test_failed_shows_the_error_and_the_last_lines(self):
        self.building([LogLine(f'l{i}') for i in range(25)], [finished(Outcome.FAILED, 1, error='boom')])
        self.pumps(3)
        found = self.labels(self.view.done.body)
        self.assertIn('Error: boom', found)
        self.assertIn('\n'.join(f'l{i}' for i in range(5, 25)), found)
        self.assertNotIn('Open app', self.done_buttons())

    def test_cancelled(self):
        self.building([finished(Outcome.CANCELLED, 130, termination='exit 130', signals_sent=(2,),
                                cancel_compatible=True)])
        self.pumps(3)
        self.assertIn('Cancelled: the build ended with exit 130, which matches a signal that was accepted (SIGINT).',
                      self.labels(self.view.done.body))

    def test_start_failed_without_a_log(self):
        self.start(checks=READY, build=[[ProcessError('prepare', 'x'), finished(Outcome.START_FAILED, None,
                                                                                  error='no python', log=None)]])
        self.loaded('totk')
        self.view.refresh()
        self.view.checks.check_button.invoke()
        self.pumps(2)
        self.view.checks.build_button.invoke()
        self.pumps(2)
        found = self.labels(self.view.done.body)
        self.assertEqual(found[:3], ['The build could not start.', 'Error: no python', 'prepare: x'])
        self.assertNotIn('Copy', self.done_buttons())

    def test_a_save_failure_after_finished_is_shown(self):                    # closing audit 1
        self.building([finished(Outcome.SUCCESS, app='/A.app')], [SettingsNotSaved('full'), ProcessError('save', 'x')])
        self.pumps(4)
        self.assertIn('Settings not saved: full', self.labels(self.view.done.body))
        self.assertIn('save: x', self.labels(self.view.done.body))             # S3.4, f3

    def test_new_build_waits_for_idle(self):
        self.building([finished(Outcome.SUCCESS, app='/A.app')], saving_ticks=2)
        self.pumps(3)
        self.assertEqual(self.view.done.new_button.cget('state'), 'disabled')      # still saving
        self.pumps(3)
        self.assertEqual(self.view.done.new_button.cget('state'), 'normal')
        self.assertEqual(self.entry('keys').cget('state'), 'normal')               # controls back at idle
        self.view.done.new_button.invoke()
        self.assertTrue(self.shown('checks') and not self.shown('done'))
        self.assertEqual(self.labels(self.view.checks.rows), [])


class Closing(BuildBase):
    def test_busy_and_no(self):
        self.building([], [], [])
        self.view.close()
        self.assertEqual((len(self.asked), 'cancel' in self.ctl.calls, self.view.closed), (1, False, False))

    def test_busy_and_yes_closes_once_idle(self):
        self.building([], [])
        self.confirm_answer = True
        self.view.close()
        self.assertEqual(('cancel' in self.ctl.calls, self.view.closed), (True, False))
        self.pumps(6)
        self.assertEqual((self.view.closed, self.ctl.closes), (True, 1))

    def test_closing_with_yes_shows_the_cancel(self):                         # closing audit 2
        self.building([], [], [])
        self.confirm_answer = True
        self.view.close()
        button = self.view.build.cancel_button
        self.assertEqual((button.cget('text'), button.cget('state')), ('Cancelling...', 'disabled'))
        self.ctl.cancel_failed = True
        self.view.pump()
        self.assertEqual(button.cget('text'), 'Retry cancel')

    def test_settle_cancels_and_waits(self):
        self.building([], [])
        updates, err = [], io.StringIO()
        def update():
            updates.append(1)
            if len(updates) == 1:
                raise KeyboardInterrupt                         # a second Ctrl-C does not stop the wait
            self.view.pump()
        settle(self.view, update, lambda: self.now, lambda s: None, err=err)
        self.assertEqual((self.ctl.calls.count('cancel'), self.view.closed), (1, True))
        self.assertEqual(err.getvalue(), 'Still stopping the build...\n')

    def test_quit_goes_through_close(self):                                   # S3.4, B1: Cmd-Q
        self.building([], [], [])
        wire_quit(self.root, self.view)
        self.root.tk.call('::tk::mac::Quit')                    # busy, and the question is answered no
        self.assertEqual((len(self.asked), self.view.closed), (1, False))
        for _ in range(10):                                     # the scripted build ends
            if self.ctl.idle:
                break
            self.view.pump()
        self.assertTrue(self.ctl.idle)
        self.root.tk.call('::tk::mac::Quit')
        self.assertTrue(self.view.closed)

    def test_quit_signals_only_ask_to_exit(self):                             # S3.4, B1
        self.start()
        sigs = (signal.SIGINT, signal.SIGHUP, signal.SIGTERM)
        before = {sig: signal.getsignal(sig) for sig in sigs}
        for sig in sigs:
            self.addCleanup(signal.signal, sig, before[sig])
        for sig in sigs:
            with self.subTest(sig=sig):
                self.view.exit_requested = False
                with exit_signals(self.view):
                    try:
                        os.kill(os.getpid(), sig)
                        time.sleep(0.05)
                    except KeyboardInterrupt:                   # g3: a FAIL, not an aborted suite
                        self.fail('the handler raised')
                self.assertTrue(self.view.exit_requested)
        self.assertEqual({sig: signal.getsignal(sig) for sig in sigs}, before)

    def test_a_signal_inside_a_tk_callback_is_not_lost(self):                 # S3.4, B1
        self.building([], [], [])
        self.addCleanup(signal.signal, signal.SIGHUP, signal.getsignal(signal.SIGHUP))
        with exit_signals(self.view):
            self.root.after(0, lambda: (os.kill(os.getpid(), signal.SIGHUP), time.sleep(0.05)))
            self.root.update()                                  # the callback runs inside Tk's dispatch
            self.assertTrue(self.view.exit_requested)
            self.view.pump()
        self.assertEqual((self.ctl.calls.count('cancel'), self.asked, self.view.closed), (1, [], False))
        for _ in range(10):
            self.view.pump()
        self.assertTrue(self.view.closed)                       # closed once the build stopped, with no question

    def test_settle_survives_a_closed_terminal(self):                         # S3.4, B1
        self.building([], [])
        class Gone:
            def write(self, text):
                raise OSError(5, 'Input/output error')
        def update():
            if len(self.ctl.calls) and update.n == 0:
                update.n = 1
                raise KeyboardInterrupt                         # a second signal: settle tells, the tell fails
            self.view.pump()
        update.n = 0
        settle(self.view, update, lambda: self.now, lambda s: None, err=Gone())
        self.assertEqual((self.ctl.calls.count('cancel'), self.view.closed), (1, True))

    def test_settle_keeps_following_past_30_s(self):                          # S3.4, B1: it never gives up
        self.building([], [])
        self.ctl.saving_ticks = 10 ** 6                         # not idle until the 8th update
        updates, err = [], io.StringIO()
        def update():
            updates.append(1)
            self.now += 10
            if len(updates) == 8:
                self.ctl.saving_ticks = 0
            self.view.pump()
        settle(self.view, update, lambda: self.now, lambda s: None, err=err)
        self.assertEqual((self.view.closed, len(updates) >= 8), (True, True))
        self.assertIn('The build has not stopped after 30 s; the builder keeps waiting for it.', err.getvalue())


if __name__ == '__main__':
    unittest.main()
