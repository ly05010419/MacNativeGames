"""tools/builder/ports.py (S1.1b): CommandRunner (cases 1-9 of the mini plan), DiskInfo, Clock.

Every test that starts a process registers a cleanup that SIGKILLs its group and reaps the leader, before any
assertion, so a failing assertion leaves no process behind. No test synchronises on a PID file, and none asserts a
timing tighter than "did not wait for the command"."""
import errno
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.builder import ports
from tools.builder.ports import CommandRunner, DiskInfo, Outcome, RealClock

REAL_KILLPG = os.killpg


def stop_group(pid):
    """Cleanup: kill whatever is left of the group and reap the leader if it is still our child."""
    if pid is None:
        return
    try:
        REAL_KILLPG(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass


def group_is_empty(pid, wait_s=10.0):
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        try:
            REAL_KILLPG(pid, 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            pass        # EPERM means the group exists; on macOS its only member is a zombie not reaped yet (S1.8c)
        time.sleep(0.05)
    return False


class GroupIsEmpty(unittest.TestCase):
    def start(self, argv):
        proc = subprocess.Popen(argv, start_new_session=True)
        self.addCleanup(proc.wait)                  # runs after stop_group: tells Popen its process is gone
        self.addCleanup(stop_group, proc.pid)
        return proc

    def test_a_live_member_is_not_empty(self):
        proc = self.start(['/bin/sleep', '30'])
        self.assertFalse(group_is_empty(proc.pid, wait_s=0.2))

    def test_a_zombie_is_not_empty_until_it_is_reaped(self):
        proc = self.start(['/usr/bin/true'])
        os.waitid(os.P_PID, proc.pid, os.WEXITED | os.WNOWAIT)      # dead and not reaped: a zombie now
        self.assertFalse(group_is_empty(proc.pid, wait_s=0.2))      # before the fix: PermissionError
        proc.wait()
        self.assertTrue(group_is_empty(proc.pid))


class Commands(unittest.TestCase):
    def setUp(self):
        self.runner = CommandRunner(RealClock())

    def run_cmd(self, argv, **kw):
        result = self.runner.run(argv, **kw)
        self.addCleanup(stop_group, result.pid)
        return result

    def test_1_exit_codes(self):
        r = self.run_cmd(['/usr/bin/true'])
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.OK, 0, True))
        r = self.run_cmd(['/usr/bin/false'])
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.NONZERO, 1, True))
        r = self.run_cmd(['/bin/sh', '-c', 'echo x >&2; exit 3'])
        self.assertEqual((r.outcome, r.code, r.stderr), (Outcome.NONZERO, 3, 'x\n'))

    def test_2_missing_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self.run_cmd([Path(tmp) / 'never-created'])
        self.assertEqual((r.outcome, r.code, r.pid, r.reaped), (Outcome.NOT_FOUND, None, None, False))

    def test_3_invalid_utf8_is_replaced(self):
        r = self.run_cmd(['/usr/bin/printf', '\\377'])
        self.assertEqual((r.outcome, r.stdout), (Outcome.OK, '�'))

    def test_4_leader_exits_while_a_descendant_holds_the_output(self):
        r = self.run_cmd(['/bin/sh', '-c', '(sleep 30 &); echo done'])
        self.assertEqual((r.outcome, r.code, r.reaped, r.stdout), (Outcome.OK, 0, True, 'done\n'))
        self.assertLess(r.elapsed_s, 30)        # with pipes this would wait for the sleep

    def test_5_timeout_kills_the_whole_group_and_keeps_partial_output(self):
        # The only timing assumption in this file: the shell prints "before" within the 5 s timeout.
        r = self.run_cmd(['/bin/sh', '-c', 'echo before; sleep 30 & sleep 30'], timeout_s=5)
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.TIMEOUT, -9, True))
        self.assertIn('before', r.stdout)
        self.assertLess(r.elapsed_s, 30)
        self.assertTrue(group_is_empty(r.pid), 'a member of the group survived')

    def test_6_timeout_without_descendants(self):
        r = self.run_cmd(['/bin/sleep', '30'], timeout_s=0.5)
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.TIMEOUT, -9, True))
        self.assertLess(r.elapsed_s, 30)


class SimulatedKill(unittest.TestCase):
    def setUp(self):
        self.runner = CommandRunner(RealClock())

    def test_7_not_reaped_when_the_kill_has_no_effect(self):
        # The SIGKILL is "sent" but nothing happens. Python warns when run drops the Popen of a running process.
        with patch.object(ports.os, 'killpg') as killpg, self.assertWarnsRegex(ResourceWarning, 'still running'):
            r = self.runner.run(['/bin/sleep', '30'], timeout_s=0.3, kill_wait_s=0.3)
            self.addCleanup(stop_group, r.pid)      # before leaving the block, whose exit asserts the warning
        killpg.assert_called_once_with(r.pid, signal.SIGKILL)
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.TIMEOUT, None, False))
        self.assertIn(f'builder: could not stop process {r.pid} within 0.3 s', r.stderr)
        # run left no thread behind and no Popen was started since, so nothing else can reap it: this waitpid
        # getting the pid proves the leader really was left unreaped.
        REAL_KILLPG(r.pid, signal.SIGKILL)
        self.assertEqual(os.waitpid(r.pid, 0)[0], r.pid)

    def test_8_killpg_refused_and_the_leader_ends_by_itself(self):
        with patch.object(ports.os, 'killpg', side_effect=PermissionError(errno.EPERM, 'Operation not permitted')):
            r = self.runner.run(['/bin/sh', '-c', 'sleep 0.5'], timeout_s=0.1, kill_wait_s=5)
        self.addCleanup(stop_group, r.pid)
        self.assertEqual((r.outcome, r.code, r.reaped), (Outcome.TIMEOUT, 0, True))
        self.assertIn('Operation not permitted', r.stderr)

    def test_9_start_errors(self):
        cases = [(PermissionError(errno.EACCES, 'Permission denied'), Outcome.NOT_EXECUTABLE),
                 (OSError(errno.ENOEXEC, 'Exec format error'), Outcome.NOT_EXECUTABLE),
                 (FileNotFoundError(errno.ENOENT, 'No such file'), Outcome.NOT_FOUND)]
        for error, outcome in cases:
            with self.subTest(error=error), patch.object(ports.subprocess, 'Popen', side_effect=error):
                r = self.runner.run(['/whatever'])
                self.assertEqual((r.outcome, r.code, r.pid, r.reaped), (outcome, None, None, False))
                self.assertIn(error.strerror, r.stderr)

    def test_other_system_errors_at_start_propagate(self):
        for code in (errno.EMFILE, errno.ENOMEM, errno.EAGAIN):
            error = OSError(code, os.strerror(code))
            with self.subTest(errno=code), patch.object(ports.subprocess, 'Popen', side_effect=error):
                with self.assertRaises(OSError) as raised:
                    self.runner.run(['/whatever'])
                self.assertEqual(raised.exception.errno, code)

    def test_elapsed_comes_from_the_injected_clock(self):
        clock = SimpleNamespace(monotonic=iter([10.0, 12.5]).__next__, wall=time.time)
        self.assertEqual(CommandRunner(clock).run(['/usr/bin/true']).elapsed_s, 2.5)


class Disk(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name).resolve()
        self.disk = DiskInfo()

    def test_missing_path_probes_its_nearest_existing_ancestor(self):
        info = self.disk.probe(self.tmp / 'a/b/c')
        self.assertEqual((info.asked, info.probed), (self.tmp / 'a/b/c', self.tmp))
        self.assertGreater(info.free_bytes, 0)

    def test_file_probes_itself(self):
        (self.tmp / 'f').write_text('x')
        self.assertEqual(self.disk.probe(self.tmp / 'f').probed, self.tmp / 'f')

    def test_relative_path_is_resolved_against_the_current_folder(self):
        old = os.getcwd()
        self.addCleanup(os.chdir, old)
        os.chdir(self.tmp)
        self.assertEqual(self.disk.probe('not/yet').probed, self.tmp)

    def test_symlink_probes_its_target(self):
        (self.tmp / 'target').mkdir()
        (self.tmp / 'link').symlink_to(self.tmp / 'target')
        self.assertEqual(self.disk.probe(self.tmp / 'link/new').probed, self.tmp / 'target')

    def test_same_volume_for_a_missing_folder_inside_tmp(self):
        self.assertTrue(self.disk.same_volume(self.tmp / 'a/b', self.tmp))

    def test_devices_compared_simulated(self):
        (self.tmp / 'x').mkdir()
        devices = {self.tmp / 'x': 1, self.tmp: 2}
        with patch.object(ports, '_stat', side_effect=lambda p: SimpleNamespace(st_dev=devices[p])):
            self.assertFalse(self.disk.same_volume(self.tmp / 'x', self.tmp))
            self.assertTrue(self.disk.same_volume(self.tmp / 'x', self.tmp / 'x/new'))

    def test_errors_on_the_existing_ancestor_propagate(self):
        with patch.object(ports, '_stat', side_effect=PermissionError('denied')):
            self.assertRaises(PermissionError, self.disk.probe, self.tmp)
        with patch.object(ports, '_disk_usage', side_effect=OSError('I/O error')):
            self.assertRaises(OSError, self.disk.probe, self.tmp)


class Clock(unittest.TestCase):
    def test_monotonic_never_decreases(self):
        clock = RealClock()
        readings = [clock.monotonic() for _ in range(1000)]
        self.assertEqual(readings, sorted(readings))

    def test_wall_is_a_timestamp(self):
        self.assertAlmostEqual(RealClock().wall(), time.time(), delta=5)


if __name__ == '__main__':
    unittest.main()
