"""tools/builder/build_run.py (S1.8a, R1): a refused signal moves the run to CANCELLING at once, is published after the
poll() that settles it (once per refusal, by object identity), and can be retried from CANCELLING and STUCK. Base
(FakeProcess, InlineJobs, a real settings store in a temporary folder) comes from the S1.7b tests.
S1.8b (Report): signals_sent reaches Finished."""
import errno
import signal
import unittest

from test_builder_build_run import BOTW, Base
from tools.builder.build_run import RunState
from tools.builder.process import State
from tools.builder.session import Finished, Outcome, ProcessError


def eperm():
    return PermissionError(errno.EPERM, 'Operation not permitted')


class Refusals(Base):
    def where(self):
        return [e.where for e in self.of(ProcessError)]

    def test_a_refusal_is_published_after_the_poll_that_settles_it_once(self):
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.assertEqual((self.run.state, self.where()), (RunState.CANCELLING, []))
        self.run.tick()
        self.assertEqual((self.where(), self.run.cancel_failed), (['cancel'], True))
        self.run.tick()
        self.run.tick()
        self.assertEqual(self.where(), ['cancel'])

    def test_retry_from_cancelling(self):
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.run.tick()
        self.proc.cancel_error = None
        self.run.cancel()
        self.assertEqual((self.proc.cancels, self.run.cancel_failed, self.run.state), (2, False, RunState.CANCELLING))
        self.assertEqual(self.proc.signals_sent, (signal.SIGINT,))

    def test_retry_from_stuck_after_a_refused_sigkill(self):
        self.started()
        self.run.cancel()
        self.proc.refuse_kill(eperm())
        self.proc.state = State.UNSTOPPABLE
        self.run.tick()
        self.assertEqual((self.run.state, self.where(), self.run.cancel_failed), (RunState.STUCK, ['kill'], True))
        self.run.cancel()
        self.assertEqual((self.proc.cancels, self.run.state, self.run.cancel_failed), (2, RunState.STUCK, False))
        self.assertEqual(self.proc.signals_sent, (signal.SIGINT, signal.SIGKILL))

    def test_no_request_from_cancelling_without_a_refusal(self):
        self.started()
        self.run.cancel()
        self.run.tick()
        self.run.cancel()
        self.assertEqual((self.proc.cancels, self.where()), (1, []))

    def test_a_refused_mismatch_cancel_is_published_once(self):
        self.started()
        self.proc.cancel_error = eperm()
        self.proc.emit(BOTW)
        for _ in range(3):
            self.run.tick()
        self.assertEqual((self.proc.cancels, self.where()), (1, ['cancel']))

    def test_two_refusals_with_the_same_text_are_two_errors(self):
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.run.tick()
        self.run.cancel()                                   # retried and refused again: a new refusal
        self.run.tick()
        self.assertEqual(self.where(), ['cancel', 'cancel'])
        first, second = self.of(ProcessError)
        self.assertEqual(first.error, second.error)

    def test_a_refusal_is_read_on_the_poll_error_path_too(self):
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.proc.poll_error = OSError(errno.EIO, 'Input/output error')
        self.run.tick()
        self.assertEqual((self.where(), self.run.cancel_failed), (['poll', 'cancel'], True))

    def test_the_end_clears_cancel_failed(self):                                # S1.8d
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.run.tick()
        self.assertTrue(self.run.cancel_failed)
        self.proc.end(130)
        self.run.tick()
        self.assertEqual((self.of(Finished)[0].outcome, self.run.state, self.run.cancel_failed),
                         (Outcome.CANCELLED, RunState.SAVING, False))

    def test_the_fake_retries_only_while_cancelling(self):                     # S1.8d: as BuildProcess does
        self.started()
        self.proc.cancel_error = eperm()
        self.run.cancel()
        self.run.tick()                                     # the refusal is settled
        self.proc.cancel_error = None
        self.proc.state = State.EXITED                      # exited with the error still set: no retry
        self.proc.request_cancel()
        self.assertEqual((self.proc.cancels, self.proc.signals_sent), (2, ()))


class Report(Base):
    def test_signals_sent_reach_finished(self):                                # S1.8b
        self.started()
        self.run.cancel()
        self.proc.end(130)
        self.run.tick()
        f = self.of(Finished)[0]
        self.assertEqual((f.outcome, f.signals_sent, f.termination, f.cancel_compatible),
                         (Outcome.CANCELLED, (signal.SIGINT,), 'exit 130', True))


if __name__ == '__main__':
    unittest.main()
