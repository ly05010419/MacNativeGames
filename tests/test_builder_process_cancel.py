"""tools/builder/process.py (S1.5): the cancel state machine driven by poll(). Deadlines run on a FakeClock that the
test advances; processes are real fakes (tests/builder_fake_build.py). stop() is registered right after start()."""
import errno
import os
from pathlib import Path
import signal
import tempfile
import unittest
from unittest.mock import patch

from builder_fake_build import REAL_KILLPG, fake_argv, stop, wait_until
from tools.builder import process
from tools.builder.process import BuildProcess, State


class FakeClock:
    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def wall(self):
        return self.now


def group_gone(pid):
    try:
        REAL_KILLPG(pid, 0)
    except ProcessLookupError:
        return True
    return False


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.clock = FakeClock()
        self.signals = []

    def start(self, *args):
        """Starts the fake and waits until it has set up its signal handling (its ready file)."""
        ready = self.tmp / 'ready'
        build = BuildProcess(self.clock)
        build.start(fake_argv(*args, '--ready', str(ready)), self.tmp / 'run.log')
        self.addCleanup(stop, build)
        self.assertTrue(wait_until(ready.exists), 'the fake never got ready')
        return build

    def record(self, real=REAL_KILLPG, drop=()):
        """A killpg that records (signal) and calls the real one, except for the signals in drop."""
        def killpg(pid, sig):
            self.signals.append(sig)
            if sig not in drop:
                return real(pid, sig)
        return patch.object(process.os, 'killpg', side_effect=killpg)

    def poll_until(self, build, predicate):
        self.assertTrue(wait_until(lambda: build.poll() is not None and predicate()), f'state stayed {build.state}')

    def sent(self):
        return [s for s in self.signals if s != 0]


class Cancel(Base):
    def test_cooperative(self):
        build = self.start('--sleep', '30')
        with self.record():
            build.request_cancel()
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.exit_code, self.sent()), (130, [signal.SIGINT]))

    def test_leader_exits_but_a_child_ignores_sigint(self):                    # review point 1
        build = self.start('--child', 'ignore', '--sleep', '30')
        with self.record():
            build.request_cancel(grace_s=10, kill_wait_s=5)
            self.poll_until(build, lambda: build.exit_code is not None)
            self.assertEqual((build.exit_code, build.state), (130, State.CANCELLING))
            self.assertFalse(group_gone(build.pid))
            self.clock.now += 10
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(self.sent(), [signal.SIGINT, signal.SIGKILL])
        self.assertTrue(wait_until(lambda: group_gone(build.pid)))
        self.assertEqual(build.exit_code, 130)

    def test_stubborn_leader_and_request_cancel_returns_at_once(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.record():
            build.request_cancel(grace_s=10)
            self.assertIsNone(build.exit_code)                  # the call did not wait for the process
            os.kill(build.pid, 0)                               # still alive (raises if not)
            for _ in range(5):
                build.poll()
            self.assertEqual(build.state, State.CANCELLING)
            self.clock.now += 10
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.exit_code, self.sent()), (-signal.SIGKILL, [signal.SIGINT, signal.SIGKILL]))

    def test_unstoppable_then_reaped_later(self):
        build = self.start('--ignore-sigint', '--sleep', '30')
        with self.record(drop=(signal.SIGKILL,)):
            build.request_cancel(grace_s=10, kill_wait_s=5)
            self.clock.now += 10
            build.poll()
            self.clock.now += 5
            build.poll()
            self.assertEqual(build.state, State.UNSTOPPABLE)
        REAL_KILLPG(build.pid, signal.SIGKILL)
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(build.exit_code, -signal.SIGKILL)

    def test_descendants_after_a_natural_end(self):                             # review point 2
        build = self.start('--child', 'ignore')
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual((build.exit_code, build.group_alive), (0, True))
        with self.record():
            build.request_cancel(grace_s=10, kill_wait_s=5)
            self.assertEqual(build.state, State.CANCELLING)
            for _ in range(5):
                build.poll()
            self.assertEqual(build.state, State.CANCELLING)                     # the child ignores SIGINT
            self.assertFalse(build.finished)
            self.clock.now += 10
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(self.sent(), [signal.SIGINT, signal.SIGKILL])
        self.assertTrue(group_gone(build.pid))
        self.assertEqual(build.exit_code, 0)


class Latch(Base):
    def test_no_signal_after_the_group_is_seen_gone(self):
        build = self.start()
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertFalse(build.group_alive)                     # probes once and latches
        with self.record():
            build.request_cancel()
            build.poll()
            self.assertFalse(build.group_alive)
        self.assertEqual(self.signals, [])

    def test_cancel_twice_sends_sigint_once(self):
        build = self.start('--sleep', '30')
        with self.record():
            build.request_cancel()
            build.request_cancel()
            self.poll_until(build, lambda: build.state is State.EXITED)
            build.request_cancel()
        self.assertEqual(self.sent(), [signal.SIGINT])


class KillpgErrors(Base):
    def test_process_lookup_error_on_sigint_counts_as_gone(self):
        build = self.start('--sleep', '0.3')
        def killpg(pid, sig):
            self.signals.append(sig)
            if sig == signal.SIGINT:
                raise ProcessLookupError(errno.ESRCH, 'No such process')
            return REAL_KILLPG(pid, sig)
        with patch.object(process.os, 'killpg', side_effect=killpg):
            build.request_cancel()
            self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(self.signals, [signal.SIGINT])                         # latched: no probe afterwards

    def test_permission_error_is_kept_not_raised(self):                        # S1.8a: it used to propagate
        build = self.start('--sleep', '30')
        error = PermissionError(errno.EPERM, 'Operation not permitted')
        with patch.object(process.os, 'killpg', side_effect=error):
            build.request_cancel()
            self.assertIsNone(build.signal_error)                               # pending until the next poll()
            build.poll()
        self.assertIs(build.signal_error.error, error)
        self.assertEqual((build.signal_error.signal, build.state), (signal.SIGINT, State.CANCELLING))


class ZombieGroup(Base):
    """S1.8c: on macOS, killpg(pgid, 0) answers EPERM while the group's only member is a zombie (dead, not reaped)."""

    def test_probe_sees_a_zombie_as_existing_and_the_next_poll_reaps_it(self):
        build = self.start('--sleep', '30')                     # alive and alone in its group when killed (no child)
        REAL_KILLPG(build.pid, signal.SIGKILL)
        # waitid with WNOWAIT returns once the process is dead without reaping it: it is a zombie now. No timeout is
        # needed: SIGKILL cannot be ignored.
        os.waitid(os.P_PID, build.pid, os.WEXITED | os.WNOWAIT)
        self.assertTrue(build.group_alive)                      # before S1.8c: PermissionError
        self.poll_until(build, lambda: build.state is State.EXITED)
        self.assertEqual(build.exit_code, -signal.SIGKILL)
        self.assertFalse(build.group_alive)


if __name__ == '__main__':
    unittest.main()
