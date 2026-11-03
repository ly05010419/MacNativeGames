"""tools/builder/jobs.py (S1.7a): the worker keeps the caller's thread free; InlineJobs runs only on request and
shuts down as the worker does."""
from concurrent.futures import CancelledError
import threading
import time
import unittest

from tools.builder.jobs import DaemonJobs, InlineJobs, Jobs


class RealWorker(unittest.TestCase):
    def setUp(self):
        self.jobs = Jobs()
        self.addCleanup(self.jobs.shutdown)

    def test_the_caller_keeps_looping_while_a_job_blocks(self):
        release = threading.Event()
        job = self.jobs.submit(lambda: release.wait(10) and 'done')
        loops, deadline = 0, time.monotonic() + 10
        while not job.done() and time.monotonic() < deadline:
            loops += 1
            if loops == 50:
                release.set()                       # the job ends only after the caller has looped 50 times
            time.sleep(0.001)
        self.assertGreaterEqual(loops, 50)
        self.assertEqual(job.result(), 'done')

    def test_result_reraises_and_never_waits(self):
        release = threading.Event()
        running = self.jobs.submit(release.wait, 10)
        with self.assertRaisesRegex(RuntimeError, 'before the job is done'):
            running.result()
        release.set()
        failing = self.jobs.submit(lambda: 1 / 0)
        self.assertTrue(wait_done(failing))
        self.assertRaises(ZeroDivisionError, failing.result)

    def test_jobs_run_one_at_a_time_in_order(self):
        order, lock = [], threading.Lock()
        def step(n):
            with lock:
                order.append(('start', n))
            time.sleep(0.01)
            with lock:
                order.append(('end', n))
        last = [self.jobs.submit(step, n) for n in range(3)][-1]
        self.assertTrue(wait_done(last))
        self.assertEqual(order, [(e, n) for n in range(3) for e in ('start', 'end')])


def wait_done(job, timeout_s=10):
    deadline = time.monotonic() + timeout_s
    while not job.done():
        if time.monotonic() > deadline:
            return False
        time.sleep(0.001)
    return True


class Inline(unittest.TestCase):
    def test_nothing_runs_until_run_next(self):
        jobs, ran = InlineJobs(), []
        first = jobs.submit(ran.append, 1)
        second = jobs.submit(ran.append, 2)
        self.assertEqual((ran, jobs.pending(), first.done()), ([], 2, False))
        self.assertRaises(RuntimeError, first.result)
        self.assertTrue(jobs.run_next())
        self.assertEqual((ran, first.done(), second.done()), ([1], True, False))
        jobs.run_next()
        self.assertFalse(jobs.run_next())
        self.assertEqual(ran, [1, 2])

    def test_exception_kept_for_result(self):
        jobs = InlineJobs()
        job = jobs.submit(lambda: [][1])
        jobs.run_next()
        self.assertTrue(job.done())
        self.assertRaises(IndexError, job.result)


class Shutdown(unittest.TestCase):
    """Review round 2: InlineJobs must shut down as Jobs does, or the controller's tests could hide a bug."""

    def assertShutdownContract(self, jobs, queued):
        for job in queued:
            self.assertTrue(job.done())
            self.assertRaises(CancelledError, job.result)
        with self.assertRaises(RuntimeError):
            jobs.submit(print)

    def test_real_worker(self):
        jobs, started, release = Jobs(), threading.Event(), threading.Event()
        self.addCleanup(jobs.shutdown)                  # cleanups run last-in first-out: release, then shutdown
        self.addCleanup(release.set)
        running = jobs.submit(lambda: started.set() or (release.wait(10) and 'finished'))
        # submit() does not guarantee the job has begun; only once it has can shutdown() leave it running.
        self.assertTrue(started.wait(10), 'the first job never started')
        queued = [jobs.submit(print), jobs.submit(print)]
        jobs.shutdown()
        self.assertShutdownContract(jobs, queued)
        release.set()                                   # a running job is not stopped and still completes
        self.assertTrue(wait_done(running))
        self.assertEqual(running.result(), 'finished')

    def test_inline_matches_it(self):
        jobs = InlineJobs()
        queued = [jobs.submit(print), jobs.submit(print)]
        jobs.shutdown()
        self.assertShutdownContract(jobs, queued)
        self.assertFalse(jobs.run_next())
        self.assertEqual(jobs.pending(), 0)


class Daemon(unittest.TestCase):                                                # S3.4, f4
    def test_a_stuck_job_holds_neither_the_worker_nor_the_process(self):
        release = threading.Event()
        self.addCleanup(release.set)                            # lets the stuck thread end after the test
        stuck = DaemonJobs().submit(release.wait)
        jobs = Jobs()
        self.addCleanup(jobs.shutdown)
        other = jobs.submit(lambda: 'checks ran')
        deadline = time.monotonic() + 5
        while not other.done() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual((other.result(), stuck.done()), ('checks ran', False))
        self.assertTrue(any(t.name == 'builder-load' and t.daemon for t in threading.enumerate()))
        release.set()
        deadline = time.monotonic() + 5
        while not stuck.done() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(stuck.result(), True)

    def test_errors_are_kept_and_shutdown_refuses(self):
        daemon = DaemonJobs()
        failing = daemon.submit(lambda: 1 / 0)
        deadline = time.monotonic() + 5
        while not failing.done() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertRaises(ZeroDivisionError, failing.result)
        daemon.shutdown()
        self.assertRaises(RuntimeError, daemon.submit, print)


if __name__ == '__main__':
    unittest.main()
