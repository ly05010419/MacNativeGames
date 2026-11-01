"""S19.1: a cancel reaches the texture pack's fitter (texture_fit.fit through cancellable.run), alone, at a build's
start and at Apply and the icons' Apply, with a real fitter that blocks (a Python script that starts a `sleep` child,
writes both pids and waits). Only a cancel leaves fit as cancellable.Stopped; a timeout, a script that cannot start and
a non-zero exit stay failures. Every test registers the cleanup that kills the fitter's group before it waits for the
pids, so a mutation that drops the Event fails by a time limit and leaves nothing running. No game, no build."""
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
import unittest
from unittest.mock import patch

from builder_fake_process import Clock, FakeProcess, drain
from builder_fakes import FakeRunner, ok
from test_builder_controller import Check
from test_builder_textures import Pack
from test_builder_mod_library import REPO
from tools.builder import cancellable, controller_mods, mod_library, texture_fit
from tools.builder.checks import CheckContext
from tools.builder.config import SettingsStore
from tools.builder.controller import BuildController
from tools.builder.jobs import DaemonJobs, InlineJobs, Jobs
from tools.builder.session import Finished, IconsDone, ModsApplied, Outcome
from tools.builder.start_options import StartPaths

FITTER = '''import os, subprocess, sys, time
from pathlib import Path
here = Path(__file__).parent
mode = (here / 'mode').read_text().strip()
if mode == 'fail':
    print('one'); print('two'); print('three', file=sys.stderr); sys.exit(3)
if mode == 'ok':
    sys.exit(0)
child = subprocess.Popen(['/bin/sleep', '600'])
(here / 'pids.tmp').write_text(f'{os.getpid()} {child.pid}')
os.replace(here / 'pids.tmp', here / 'pids')
time.sleep(600)
'''
WAIT_S = 3.0


def stat(pid):
    """'' when there is no such process, else its ps state (a zombie's starts with Z)."""
    return subprocess.run(['/bin/ps', '-o', 'stat=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()


def gone(pid, wait_s=WAIT_S):
    """Waits until pid is no process at all (a zombie still waiting to be reaped is not gone); its last state."""
    end = time.monotonic() + wait_s
    while stat(pid) and time.monotonic() < end:
        time.sleep(0.05)
    return stat(pid)


class Fitter(Pack):
    def setUp(self):
        super().setUp()
        self.fitter = self.tmp / 'fitter'
        self.fitter.mkdir()
        (self.fitter / 'texture_pack_tables.py').write_text(FITTER)
        self.mode('block')
        script = patch.object(texture_fit, 'SCRIPT', str(self.fitter / 'texture_pack_tables.py'))   # absolute
        script.start()
        self.addCleanup(script.stop)

    def mode(self, mode):
        (self.fitter / 'pids').unlink(missing_ok=True)
        (self.fitter / 'mode').write_text(mode)

    def kill_fitter(self):
        """The cleanup: the fitter's group and its child, whatever the code under test did."""
        try:
            pids = [int(p) for p in (self.fitter / 'pids').read_text().split()]
        except (FileNotFoundError, ValueError):
            return
        for kill, target in ((os.killpg, pids[0]), (os.kill, pids[1])):
            try:
                kill(target, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass

    def pids(self):
        end = time.monotonic() + 15                     # a Python start
        while not (self.fitter / 'pids').exists():
            self.assertLess(time.monotonic(), end, 'the fitter never started')
            time.sleep(0.05)
        return [int(p) for p in (self.fitter / 'pids').read_text().split()]

    def in_thread(self, fn, *args):
        box = {}
        def run():
            try:
                box['value'] = fn(*args)
            except BaseException as e:
                box['error'] = e
        thread = threading.Thread(target=run, daemon=True)
        self.addCleanup(thread.join, 10)                # runs after kill_fitter (cleanups are last in, first out)
        self.addCleanup(self.kill_fitter)
        thread.start()
        return thread, box

    def assert_gone(self, script, child):
        self.assertEqual(stat(script), '')              # cancellable waited for it: no zombie left behind
        self.assertEqual(gone(child), '')               # the group's child: killed, then reaped by launchd


class Fit(Fitter):
    def fit(self, cancel):
        return texture_fit.fit(REPO, self.app, 'v143', self.tmp / 'romfs', cancel)

    def test_a_cancel_kills_the_group_and_is_the_only_raise(self):
        cancel = threading.Event()
        thread, box = self.in_thread(self.fit, cancel)
        script, child = self.pids()
        asked = time.monotonic()
        cancel.set()
        thread.join(WAIT_S)
        self.assertFalse(thread.is_alive())
        self.assertLess(time.monotonic() - asked, 2.0)
        self.assertIsInstance(box.get('error'), cancellable.Stopped)
        self.assertEqual(box['error'].reason, 'cancelled')
        self.assert_gone(script, child)

    def test_without_the_event_it_runs_on(self):                   # the control
        thread, box = self.in_thread(self.fit, None)
        self.pids()
        thread.join(2.0)
        self.assertTrue(thread.is_alive())

    def test_a_timeout_is_a_failure_and_leaves_nothing(self):
        fit_s = patch.object(texture_fit, 'FIT_S', 1.0)
        fit_s.start()
        self.addCleanup(fit_s.stop)                     # after the thread's join (registered first)
        thread, box = self.in_thread(self.fit, None)
        script, child = self.pids()
        thread.join(WAIT_S + 1)
        self.assertNotIn('error', box)
        self.assertIn('timeout', box['value'][0])
        self.assert_gone(script, child)

    def test_a_script_that_cannot_start_is_a_failure(self):
        with patch.object(texture_fit.sys, 'executable', str(self.tmp / 'no-python')):
            failed = self.fit(None)
        self.assertIn('not_started', failed[0])

    def test_a_non_zero_exit_keeps_its_last_lines(self):
        self.mode('fail')
        self.assertEqual(self.fit(None), ('one', 'two', 'three'))
        self.mode('ok')
        self.assertIsNone(self.fit(None))


class Controller(Fitter):
    def setUp(self):
        super().setUp()
        self.add_pack()                                 # switched on, with its HeapDef
        self.jobs = InlineJobs()
        self.ctl = BuildController(base_context=CheckContext(runner=None),
                                   system_checks=[Check('build_python', detail='/opt/py')], input_checks=(),
                                   store=SettingsStore(self.tmp / 's.json', Clock()),
                                   process_factory=FakeProcess, jobs=self.jobs,
                                   runner=FakeRunner({lambda argv: argv[0] == 'git': ok('cafe\n')}), clock=Clock(),
                                   repo_root=REPO, logs_dir=self.tmp / 'logs')
        self.ctl.start.paths = StartPaths(self.tmp / 'Builder/start-options.json', self.data)
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'), update=str(self.tmp / 'u.nsp'),
                           out=str(self.app))
        drain(self.jobs, self.ctl.tick)
        self.ctl.take_events()

    def until(self, kind):
        end, seen = time.monotonic() + WAIT_S, []
        while time.monotonic() < end:
            self.ctl.tick()
            seen += self.ctl.take_events()
            found = [e for e in seen if isinstance(e, kind)]
            if found:
                return found[0]
            time.sleep(0.02)
        self.fail(f'no {kind.__name__} within {WAIT_S} s: {seen}')

    def test_cancel_at_the_builds_start_stops_the_fit_and_the_window_may_close(self):
        self.ctl.run_checks()
        drain(self.jobs, self.ctl.tick)
        self.ctl.jobs = Jobs()                          # a real worker for the start
        self.addCleanup(self.ctl.jobs.shutdown)
        self.addCleanup(self.kill_fitter)
        self.ctl.start_build()
        script, child = self.pids()
        self.ctl.cancel()
        finished = self.until(Finished)
        self.assertEqual((finished.outcome, finished.log_path), (Outcome.CANCELLED, None))     # no log reserved
        self.assertTrue(self.ctl.idle)
        self.ctl.close()
        self.assert_gone(script, child)
        self.assertFalse((self.tmp / 'logs').exists() and any((self.tmp / 'logs').iterdir()))

    def installed(self):
        """The game's mod folder as {path: bytes}: a changed file shows, not only an added or removed one."""
        return {p.relative_to(self.load): p.read_bytes() for p in self.load.rglob('*') if p.is_file()}

    def applied_failed(self):
        return mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state['applied_failed']

    def test_cancel_at_apply_keeps_the_installed_mods_and_asks_for_apply_again(self):
        self.ctl.load_jobs = DaemonJobs()
        self.addCleanup(self.kill_fitter)
        self.mode('ok')                                 # first a working Apply installs the pack
        self.ctl.mods.apply()
        self.assertTrue(self.until(ModsApplied).result.ok)
        before = self.installed()
        self.assertEqual(before[Path('TexToGo/Grass.txtg')], b'a much bigger texture')
        self.assertFalse(self.applied_failed())
        self.mode('block')
        self.ctl.mods.apply()
        script, child = self.pids()
        self.assertFalse(self.ctl.idle)
        self.ctl.cancel()
        applied = self.until(ModsApplied)
        self.assertEqual((applied.result.ok, applied.result.reason), (False, 'cancelled'))
        self.assertEqual(self.installed(), before)      # the same paths with the same bytes
        self.assertTrue(self.applied_failed())
        self.assertTrue(self.ctl.idle)
        self.assert_gone(script, child)
        self.mode('ok')                                 # then an Apply whose fit works clears the failure
        self.ctl.mods.apply()
        self.assertTrue(self.until(ModsApplied).result.ok)
        self.assertFalse(self.applied_failed())
        self.ctl.close()

    def test_cancel_reaches_the_icons_apply(self):
        self.ctl.load_jobs = DaemonJobs()
        self.addCleanup(self.kill_fitter)
        valid = type('Validation', (), {'valid': True})()
        with patch.object(controller_mods.icons_mod, 'install', return_value=valid):
            self.ctl.mods.install_icons(self.tmp / 'work', {'controls': 'h'})
            script, child = self.pids()
            self.ctl.cancel()
            done = self.until(IconsDone)
        self.assertEqual((done.result.ok, done.result.reason), (False, 'cancelled'))
        self.assertTrue(self.ctl.idle)
        self.assert_gone(script, child)


if __name__ == '__main__':
    unittest.main()
