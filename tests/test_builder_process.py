"""tools/builder/process.py (S1.5): start, output and files. Cancelling is in test_builder_process_cancel.py.
Each test registers stop() right after start() and before any assertion; waits are polling loops with a deadline."""
import errno
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from builder_fake_build import fake_argv, stop, wait_until
from tools.builder import process
from tools.builder.ports import RealClock
from tools.builder.process import LINE_LIMIT, BuildProcess, State


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.log = self.tmp / 'logs' / 'run.log'

    def start(self, *args, env=None):
        build = BuildProcess(RealClock())
        build.start(fake_argv(*args), self.log, env)
        self.addCleanup(stop, build)
        return build

    def collect(self, build):
        """Every line until the build has finished."""
        lines = []
        self.assertTrue(wait_until(lambda: lines.extend(build.poll()) or build.finished), 'never finished')
        return lines


class Output(Base):
    def test_lines_in_order_with_stderr_merged_and_the_log_kept(self):
        build = self.start('--lines', '50', '--stderr', 'ERROR: boom')
        lines = self.collect(build)
        self.assertEqual(lines, [f'line {i}' for i in range(50)] + ['ERROR: boom'])
        self.assertEqual(self.log.read_text(), '\n'.join(lines) + '\n')

    def test_reading_while_the_child_writes(self):
        build = self.start('--lines', '20', '--every', '0.05')
        seen, prefixes = [], []
        def step():
            seen.extend(build.poll())
            prefixes.append(len(seen))
            return build.finished
        self.assertTrue(wait_until(step))
        self.assertEqual(seen, [f'line {i}' for i in range(20)])
        self.assertGreater(len(set(prefixes)), 2)           # lines arrived over several polls, while writing
        self.assertEqual(self.log.read_text(), ''.join(f'line {i}\n' for i in range(20)))

    def test_unfinished_line_only_after_the_end(self):
        build = self.start('--lines', '1', '--partial', 'abc', '--sleep', '0.5')
        early = []
        self.assertTrue(wait_until(lambda: early.extend(build.poll()) or early))
        self.assertEqual(early, ['line 0'])
        self.assertEqual(self.collect(build), ['abc'])

    def test_burst_over_several_polls(self):
        build = self.start('--lines', '200000')                # about 2.2 MiB
        calls, lines = [0], []
        def step():
            calls[0] += 1
            lines.extend(build.poll())
            return build.finished
        self.assertTrue(wait_until(step, 20))
        self.assertEqual(lines, [f'line {i}' for i in range(200000)])
        self.assertGreaterEqual(calls[0], 3)

    def test_long_line_is_cut_at_the_limit_and_the_log_keeps_it(self):
        build = self.start('--long', str(200 * 1024))
        lines = self.collect(build)
        self.assertEqual([len(line) for line in lines], [LINE_LIMIT] * 3 + [200 * 1024 - 3 * LINE_LIMIT])
        self.assertEqual(self.log.read_bytes(), b'x' * (200 * 1024))

    def test_character_split_between_reads(self):
        with patch.object(process, 'READ_LIMIT', 3):
            build = self.start('--partial', 'a é–b')
            self.assertEqual(self.collect(build), ['a é–b'])

    def test_invalid_bytes_are_replaced(self):
        build = self.start('--raw', 'ff41fe0a')
        self.assertEqual(self.collect(build), ['�A�'])

    def test_a_descendant_writing_after_the_leader_is_done(self):        # review round 2, finding 1
        # The child writes 'abécd' in two halves that split 'é', 2 s after the leader has exited.
        build = self.start('--lines', '1', '--child', 'plain', '--child-writes', 'abécd'.encode().hex(),
                           '--child-delay', '2', '--ready', str(self.tmp / 'ready'))
        lines = []
        self.assertTrue(wait_until(lambda: lines.extend(build.poll()) or build.exit_code is not None))
        self.assertEqual((build.state, build.group_alive, build.finished), (State.EXITED, True, False))
        self.assertTrue(wait_until(lambda: lines.extend(build.poll()) or build.finished))
        self.assertEqual(lines, ['line 0', 'abécd'])
        self.assertFalse(build.group_alive)

    def test_exit_codes_and_finished(self):
        for code in (0, 1):
            with self.subTest(code=code):
                build = self.start('--exit', str(code))
                self.collect(build)
                self.assertEqual((build.state, build.exit_code, build.finished), (State.EXITED, code, True))

    def test_environment(self):
        build = self.start('--print-env', 'PYTHONUNBUFFERED', '--print-env', 'BUILDER_TEST', env={'BUILDER_TEST': 'yes'})
        self.assertEqual(self.collect(build), ['PYTHONUNBUFFERED=1', 'BUILDER_TEST=yes'])


class Files(Base):
    def record_open(self):
        handles = []
        real_open = open
        def recording(path, mode='r', *a, **k):
            handle = real_open(path, mode, *a, **k)
            handles.append((mode, handle))
            return handle
        return handles, recording

    def test_writer_closed_and_reader_separate(self):
        handles, recording = self.record_open()
        with patch.object(process, 'open', recording, create=True):
            build = self.start('--lines', '3')
        (wmode, writer), (rmode, reader) = handles
        self.assertEqual((wmode, rmode), ('wb', 'rb'))
        self.assertTrue(writer.closed)
        self.assertFalse(reader.closed)
        self.assertIsNot(writer, reader)
        self.assertEqual(self.collect(build), ['line 0', 'line 1', 'line 2'])

    def test_reader_failure_closes_the_writer_and_never_starts(self):
        handles, recording = self.record_open()
        def failing(path, mode='r', *a, **k):
            if mode == 'rb':
                raise PermissionError(errno.EACCES, 'Permission denied')
            return recording(path, mode, *a, **k)
        build = BuildProcess(RealClock())
        with patch.object(process, 'open', failing, create=True), patch.object(process.subprocess, 'Popen') as popen:
            with self.assertRaises(PermissionError):
                build.start(fake_argv(), self.log)
        popen.assert_not_called()
        self.assertTrue(handles[0][1].closed)
        self.assertIsNone(build.pid)

    def test_popen_failure_closes_both(self):
        handles, recording = self.record_open()
        build = BuildProcess(RealClock())
        with patch.object(process, 'open', recording, create=True):
            with self.assertRaises(FileNotFoundError):
                build.start([str(self.tmp / 'no-such-python')], self.log)
        self.assertTrue(all(h.closed for _, h in handles))
        self.assertEqual(len(handles), 2)

    def test_close_is_idempotent_and_poll_still_reaps(self):
        build = self.start('--sleep', '0.3')
        build.close()
        build.close()
        self.assertTrue(wait_until(lambda: build.poll() == [] and build.state is State.EXITED))
        self.assertEqual(build.exit_code, 0)

    def test_second_start_and_poll_before_start(self):
        build = self.start()
        with self.assertRaises(RuntimeError):
            build.start(fake_argv(), self.log)
        with self.assertRaises(RuntimeError):
            BuildProcess(RealClock()).poll()

    def test_popen_gets_the_log_file_not_a_pipe(self):
        real_popen, seen = subprocess.Popen, {}
        def recording(*a, **k):
            seen.update(k)
            return real_popen(*a, **k)
        with patch.object(process.subprocess, 'Popen', recording):
            build = self.start()
        self.assertIsInstance(seen['stdout'], io.BufferedWriter)
        self.assertEqual((seen['stderr'], seen['stdin'], seen['start_new_session']),
                         (subprocess.STDOUT, subprocess.DEVNULL, True))
        self.collect(build)


if __name__ == '__main__':
    unittest.main()
