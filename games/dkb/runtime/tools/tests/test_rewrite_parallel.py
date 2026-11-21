"""Worker configuration and cancellation of the rewriter's actual fork pool, without game files."""
import ast
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / 'dm/tools/patch_nso.py'


def alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False


def wait_until(test):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        if test(): return True
        time.sleep(0.01)
    return False


def pool_helper():
    """patch_nso is a command-line program: its real pool helper and process signal handler, extracted as the spin tests extract its analysis."""
    tree = ast.parse(SOURCE.read_text())
    body: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'in_parallel'
            or isinstance(n, ast.Expr) and isinstance(n.value, ast.Call) and isinstance(n.value.func, ast.Attribute)
            and isinstance(n.value.func.value, ast.Name) and n.value.func.value.id == 'signal' and n.value.func.attr == 'signal']
    return 'import multiprocessing, os, signal, sys, time\nfrom pathlib import Path\nWORKERS = 2\n' + ast.unparse(ast.Module(body=body, type_ignores=[])) + '\n'


class RewriteWorkers(unittest.TestCase):
    def test_invalid_worker_count_stops_before_loading_the_module(self):
        for value in ('0', '-1', 'many'):
            with self.subTest(value=value):
                r = subprocess.run([sys.executable, str(SOURCE), 'not-a-module', 'unused', 'main'],
                                   env=dict(os.environ, DM_REWRITE_J=value), capture_output=True, text=True)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('[RED] DM_REWRITE_J must be a positive integer', r.stderr)
                self.assertNotIn('Traceback', r.stderr)

    def test_sigterm_reaps_the_active_pool(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / 'pool.py'
            script.write_text(pool_helper()
                              + f'def task(n):\n    Path({tmp!r}, str(os.getpid())).touch()\n    while True: time.sleep(1)\n'
                              + '\nin_parallel(task, [(0,), (1,)])\n')
            p = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            pids = []
            try:
                self.assertTrue(wait_until(lambda: len(list(root.glob('[0-9]*'))) == 2), 'pool did not start both jobs')
                pids = [int(f.name) for f in root.glob('[0-9]*')]
                p.terminate(); p.communicate(timeout=5)
                self.assertTrue(wait_until(lambda: all(not alive(pid) for pid in pids)), 'workers still alive after the rewriter stopped')
            finally:
                try: os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                p.communicate(timeout=5)

    def test_workers_cannot_lose_the_pool_sigterm(self):
        # Pool.terminate's one SIGTERM must end a worker wherever it lands. A worker with the rewriter's Python handler could lose it while starting and hang the rewrite in about 1 of 6,000
        # small pools under load, too rare and too slow to catch by repeating them. So check what rules it out: blocked from the fork on, then the default action, unblocked, before any job.
        script = pool_helper() + '''
import json, os, signal, sys
TERM = signal.SIGTERM
at_fork = {'blocked': None}
os.register_at_fork(after_in_child=lambda: at_fork.update(blocked=TERM in signal.pthread_sigmask(signal.SIG_BLOCK, [])))
def job(n):
    return at_fork['blocked'], signal.getsignal(TERM) == signal.SIG_DFL, TERM in signal.pthread_sigmask(signal.SIG_BLOCK, [])
results = in_parallel(job, [(0,), (1,)])
print(json.dumps({'workers': results, 'parent_blocked': TERM in signal.pthread_sigmask(signal.SIG_BLOCK, []), 'parent_handler': callable(signal.getsignal(TERM))}))
'''
        r = subprocess.run([sys.executable, '-c', script], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        got = json.loads(r.stdout)
        self.assertEqual(got['workers'], [[True, True, False]] * 2)
        self.assertEqual((got['parent_blocked'], got['parent_handler']), (False, True))

    def test_sigterm_during_teardown_waits_for_it_to_finish(self):
        # raised inside Pool.terminate, the handler's sys.exit would stop it halfway: it must act once the pool is gone
        script = pool_helper() + '''
import multiprocessing.pool, os, signal
real = multiprocessing.pool.Pool.terminate
def signal_then_terminate(self):
    os.kill(os.getpid(), signal.SIGTERM)
    real(self)
    print('terminated', *(w.pid for w in self._pool), flush=True)
multiprocessing.pool.Pool.terminate = signal_then_terminate
def job(n): return n
in_parallel(job, [(0,), (1,)])
print('carried on', flush=True)
'''
        r = subprocess.run([sys.executable, '-c', script], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 143, r.stderr)
        self.assertIn('terminated', r.stdout)
        self.assertNotIn('carried on', r.stdout)

    def test_sigterm_during_pool_start_reaps_the_workers(self):
        # a SIGTERM that arrives while the mask is on takes effect once the pool exists, and still goes through its context manager
        script = pool_helper() + '''
import os, signal, sys, time
real = multiprocessing.context.BaseContext.Pool
def pool_then_signal(self, *args, **kwargs):
    pool = real(self, *args, **kwargs)
    print(*(w.pid for w in pool._pool), flush=True)
    os.kill(os.getpid(), signal.SIGTERM)
    return pool
multiprocessing.context.BaseContext.Pool = pool_then_signal
def job(n): time.sleep(60)
in_parallel(job, [(0,), (1,)])
'''
        p = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            out, _ = p.communicate(timeout=20)
            self.assertEqual(p.returncode, 143)
            pids = [int(pid) for pid in out.split()]
            self.assertEqual(len(pids), 2)
            self.assertTrue(wait_until(lambda: all(not alive(pid) for pid in pids)), 'workers still alive after the rewriter stopped')
        finally:
            try: os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            p.communicate(timeout=5)


if __name__ == '__main__': unittest.main()
