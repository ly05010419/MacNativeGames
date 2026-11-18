"""One job at a time on a Mac, shared by MNG Launcher and tools/mng.py (tools/builder_jobs/shared.py), and mng's words
(tools/builder_jobs/text_cli.py): a build started in a terminal or by an AI is the window's too."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.builder_jobs import build_job, shared, text_cli  # noqa: E402


class Shared(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)

    def test_the_job_is_recorded_while_it_runs_and_its_events_kept_after(self):
        out = io.StringIO()
        with shared.claim(out, 'test', self.home.name) as (tee, record):
            shared.describe(record, game='totk', app='/A.app', kind='express')
            tee.write('{"event": "log", "text": "x"}\n'); tee.flush()
            now = shared.current(self.home.name)
            self.assertEqual((now['pid'], now['game'], now['kind'], now['source']), (os.getpid(), 'totk', 'express', 'test'))
            self.assertIn('"x"', Path(now['events']).read_text())
        jobs = shared.folder(self.home.name)
        self.assertIsNone(shared.current(self.home.name))
        self.assertFalse((jobs / 'current.json').exists())
        self.assertIn('"x"', (jobs / 'last.ndjson').read_text())
        self.assertIn('"x"', out.getvalue())   # the job's own output gets every line too

    def test_a_second_job_is_refused_and_names_the_first(self):
        script = (f'import sys, time; sys.path.insert(0, {str(REPO)!r}); from tools.builder_jobs import shared\n'
                  f'with shared.claim(sys.stdout, "the other one", {self.home.name!r}) as (tee, rec):\n'
                  f'    shared.describe(rec, game="botw"); print("held", flush=True); time.sleep(30)\n')
        holder = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, text=True)
        self.addCleanup(holder.kill)
        self.assertEqual(holder.stdout.readline().strip(), 'held')
        with self.assertRaisesRegex(shared.Busy, 'botw.*the other one'):
            with shared.claim(io.StringIO(), 'me', self.home.name):
                pass

    def test_a_refused_job_says_so_as_its_done_event(self):
        out = io.StringIO()
        with mock.patch.object(shared, 'claim', side_effect=shared.Busy('busy: botw')):
            code = build_job.start({'update': '/x.app'}, out, Path(self.home.name))
        done = json.loads(out.getvalue().splitlines()[-1])
        self.assertEqual((code, done['event'], done['ok'], done['busy']), (3, 'done', False, True))


class Words(unittest.TestCase):
    def say(self, *events):
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            w = text_cli.Words()
            for e in events:
                w.write(json.dumps(e) + '\n')
        return printed.getvalue()

    def test_a_job_reads_as_lines(self):
        text = self.say({'event': 'plan', 'kind': 'express', 'app': '/A.app', 'work': '/w', 'log': '/l'},
                        {'event': 'step', 'name': 'runtime'}, {'event': 'log', 'text': 'building the runtime'},
                        {'event': 'done', 'ok': False, 'message': 'make failed', 'step_log': '/w/logs/runtime.log'})
        for part in ('/A.app', '/w', '-- runtime', 'building the runtime', 'make failed', '/w/logs/runtime.log'):
            self.assertIn(part, text)


class Mng(unittest.TestCase):
    def test_macos_python_starts_it(self):
        # any python3 starts mng: it runs the jobs with a Python 3.14 (MNG Launcher's own, else one on the PATH)
        done = subprocess.run(['/usr/bin/python3', str(REPO / 'tools/mng.py'), '--help'], capture_output=True, text=True)
        if done.returncode and 'needs MNG Launcher' in done.stderr:
            self.skipTest('no Python 3.14 on this Mac')
        self.assertEqual(done.returncode, 0, done.stderr)
        for command in ('status', 'build', 'update', 'page', 'open'):
            self.assertIn(command, done.stdout)


if __name__ == '__main__':
    unittest.main()
