"""Where the builder's blocking work runs (S1.7a): one worker thread, so the view's thread never waits.

A job is a plain function of a frozen snapshot that returns a value; the view's thread reads that value through its Job
once done(). Jobs run one at a time, in order. InlineJobs has the same methods for tests: a job runs only when the test
calls run_next()."""
from concurrent.futures import CancelledError, Future, ThreadPoolExecutor
import threading


class Job:
    def __init__(self, future):
        self._future = future

    def done(self):
        return self._future.done()

    def result(self):
        """The job's value, or its exception re-raised. Only once done(): it never waits."""
        if not self._future.done():
            raise RuntimeError('result() before the job is done')
        return self._future.result()


class Jobs:
    def __init__(self):
        self._pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix='builder-job')

    def submit(self, fn, *args) -> Job:
        return Job(self._pool.submit(fn, *args))

    def shutdown(self):
        """Stops taking jobs (submit() then raises RuntimeError) and cancels the queued ones (done() becomes True,
        result() raises CancelledError); does not wait for one that is running."""
        self._pool.shutdown(wait=False, cancel_futures=True)


class DaemonJobs:
    """S3.4 f4: each job on its own daemon thread, for the settings load only. If reading the settings file never
    returns (a stuck volume), the worker stays free for the checks and the build, and quitting is not held up by it.
    shutdown() takes no new jobs; a running one is left to end or to die with the process."""

    def __init__(self):
        self._closed = False

    def submit(self, fn, *args) -> Job:
        if self._closed:
            raise RuntimeError('cannot schedule new jobs after shutdown')
        future = Future()
        def run():
            if future.set_running_or_notify_cancel():
                try:
                    future.set_result(fn(*args))
                except BaseException as e:      # kept for result(), as ThreadPoolExecutor keeps it
                    future.set_exception(e)
        threading.Thread(target=run, name='builder-load', daemon=True).start()
        return Job(future)

    def shutdown(self):
        self._closed = True


class InlineJob:
    def __init__(self, fn, args):
        self.fn, self.args = fn, args
        self._done, self._value, self._error = False, None, None

    def done(self):
        return self._done

    def result(self):
        if not self._done:
            raise RuntimeError('result() before the job is done')
        if self._error is not None:
            raise self._error
        return self._value

    def _run(self):
        try:
            self._value = self.fn(*self.args)
        except Exception as e:      # kept for result(), as a Future keeps it
            self._error = e
        self._done = True

    def _cancel(self):
        self._error, self._done = CancelledError(), True


class InlineJobs:
    """Jobs for tests: nothing runs until run_next(), so every interleaving can be set up on purpose. Shutting
    down behaves as Jobs does: queued jobs are cancelled and later submits raise RuntimeError."""

    def __init__(self):
        self._queue = []
        self._closed = False

    def submit(self, fn, *args) -> InlineJob:
        if self._closed:
            raise RuntimeError('cannot schedule new jobs after shutdown')
        job = InlineJob(fn, args)
        self._queue.append(job)
        return job

    def pending(self):
        return len(self._queue)

    def run_next(self):
        """Runs the oldest pending job; returns False when there is none."""
        if not self._queue:
            return False
        self._queue.pop(0)._run()
        return True

    def shutdown(self):
        self._closed = True
        for job in self._queue:
            job._cancel()
        self._queue.clear()
