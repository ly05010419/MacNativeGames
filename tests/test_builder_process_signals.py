"""tools/builder/process.py (S1.8a): a refused signal is kept pending, settled by the next poll() (shown as
signal_error, or dropped when the target was a zombie), and retried once per refusal by request_cancel(), by phase.
Base (real fakes, FakeClock, record(), stop() registered right after start()) comes from the S1.5 cancel tests."""
import errno
import os
import signal
import unittest
from unittest.mock import patch

from builder_fake_build import REAL_KILLPG
from test_builder_process_cancel import Base
from tools.builder import process
from tools.builder.process import State

INT, KILL = signal.SIGINT, signal.SIGKILL


class Signals(Base):
    def refuse(self, *refused):
        """A killpg that records every call and raises a new EPERM for the signals in refused; the others, including
        the probe (signal 0), go to the real one."""
        def killpg(pid, sig):
            self.signals.append(sig)
            if sig in refused:
                raise PermissionError(errno.EPERM, 'Operation not permitted')
            return REAL_KILLPG(pid, sig)
        return patch.object(process.os, 'killpg', side_effect=killpg)


class Refused(Signals):
    def test_refused_sigint_is_pending_then_shown_and_sigkill_follows(self):
        build = self.start('--sleep', '30')
        with self.refuse(INT):
            build.request_cancel(grace_s=10, kill_wait_s=5)                     # does not raise
            self.assertEqual(build.state, State.CANCELLING)
            self.assertIsNone(build.signal_error)                               # pending
            build.poll()
            self.assertEqual(build.signal_error.signal, INT)
            self.clock.now = 1010                                               # the grace deadline still holds
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.exit_code, build.signals_sent, build.signal_error), (-KILL, (KILL,), None))
        self.assertEqual(self.sent(), [INT, KILL])

    def test_both_refused_ends_unstoppable_with_both_errors_seen(self):
        build = self.start('--sleep', '30')
        with self.refuse(INT, KILL):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            build.poll()
            first = build.signal_error
            self.clock.now = 1010
            build.poll()                                                        # SIGKILL refused: pending
            self.assertIs(build.signal_error, first)
            build.poll()
            second = build.signal_error
            self.clock.now = 1015
            build.poll()
            self.assertEqual(build.state, State.UNSTOPPABLE)
        self.assertEqual((first.signal, second.signal, build.signals_sent), (INT, KILL, ()))
        self.assertIsNot(first.error, second.error)


class Retry(Signals):
    """The clock moves before each retry, so a retry that reset a deadline would move the next step later."""

    def test_retry_in_the_sigint_phase(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.refuse(INT):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            build.poll()
        self.clock.now = 1003
        with self.record():
            build.request_cancel(grace_s=10, kill_wait_s=5)                     # SIGINT again, accepted (ignored)
            build.request_cancel(grace_s=10, kill_wait_s=5)                     # nothing new refused: nothing sent
            self.assertIsNone(build.signal_error)
            self.clock.now = 1009.9
            build.poll()
            self.assertEqual(self.sent(), [INT, INT])
            self.clock.now = 1010                                               # the original grace deadline
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((self.sent(), build.signals_sent), ([INT, INT, KILL], (INT, KILL)))

    def test_retry_in_the_kill_phase_sends_sigkill_only(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.refuse(KILL):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            self.clock.now = 1010
            build.poll()                                                        # SIGKILL refused: pending
            build.poll()                                                        # settled
        self.assertEqual(build.signal_error.signal, KILL)
        self.clock.now = 1012
        with self.record(drop=(KILL,)):                                         # accepted, not delivered: it lives on
            build.request_cancel()
            self.clock.now = 1014.9
            build.poll()
            self.assertEqual(build.state, State.CANCELLING)
            self.clock.now = 1015                                               # the original kill deadline
            build.poll()
            self.assertEqual(build.state, State.UNSTOPPABLE)
        self.assertEqual((self.sent(), build.signal_error), ([INT, KILL, KILL], None))

    def test_retry_when_unstoppable_keeps_the_state(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.refuse(KILL):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            self.clock.now = 1010
            build.poll()
            build.poll()
            self.clock.now = 1015
            build.poll()
        self.assertEqual((build.state, build.signal_error.signal), (State.UNSTOPPABLE, KILL))
        with self.record():
            build.request_cancel()
            self.assertEqual(build.state, State.UNSTOPPABLE)
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((self.sent(), build.exit_code), ([INT, KILL, KILL], -KILL))

    def test_no_retry_without_a_settled_refusal(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.refuse(INT):
            build.request_cancel()
            build.request_cancel()                                              # refused, but still pending
        self.assertEqual(self.sent(), [INT])
        build.poll()
        with self.record():
            build.request_cancel()                                              # settled: one retry, accepted
            build.request_cancel()                                              # accepted last time: nothing
        self.assertEqual(self.sent(), [INT, INT])


class Accepted(Signals):
    def test_a_refusal_from_a_zombie_is_dropped(self):
        build = self.start('--sleep', '30')                     # alone in its group (no child), as in S1.8c
        REAL_KILLPG(build.pid, KILL)
        os.waitid(os.P_PID, build.pid, os.WEXITED | os.WNOWAIT)  # dead, not reaped: a zombie
        errors = []
        def killpg(pid, sig):
            try:
                return REAL_KILLPG(pid, sig)
            except OSError as e:
                errors.append((sig, type(e)))
                raise
        with patch.object(process.os, 'killpg', side_effect=killpg):
            build.request_cancel()                              # the real killpg answers EPERM: pending
        self.assertEqual(errors, [(INT, PermissionError)])
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.exit_code, build.signal_error, build.signals_sent), (-KILL, None, ()))

    def test_signals_sent_holds_only_accepted_signals(self):
        build = self.start('--sleep', '30')
        build.request_cancel()
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(build.signals_sent, (INT,))
        build.request_cancel()                                  # the group is latched gone: nothing
        self.assertEqual(build.signals_sent, (INT,))

    def test_the_end_clears_a_refusal(self):                                    # S1.8d
        build = self.start('--sleep', '0.3')
        with self.refuse(INT):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            build.poll()
        self.assertEqual(build.signal_error.signal, INT)
        self.poll_until(build, lambda: build.state is State.EXITED)                # the clock stays: no SIGKILL is due
        self.assertEqual((build.exit_code, build.signal_error, build.signals_sent), (0, None, ()))

    def test_a_group_already_gone_sends_nothing(self):
        build = self.start('--sleep', '0.3')
        with patch.object(process.os, 'killpg', side_effect=ProcessLookupError(errno.ESRCH, 'No such process')):
            build.request_cancel()
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.signals_sent, build.signal_error), ((), None))


if __name__ == '__main__':
    unittest.main()
