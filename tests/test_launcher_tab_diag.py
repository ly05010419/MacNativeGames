"""MNG Launcher's 诊断模式 (tools/builder_jobs/tab_diag.py; user 2026-11-13): the game starts with the debug session's
switches and a run folder; the commands the card sends are checked first (a line the game does not take ends the game,
runtime/input.c runtime_fail) and appended to the game's input file; the game log is kept in the run folder once the
game closed. Start game itself carries none of it (tests/test_launcher_play_clean.py)."""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.builder.games import OOT, TOTK   # noqa: E402
from tools.builder_jobs import tab_diag, tabs   # noqa: E402


def ctx_for(home, spec=OOT):
    support = home / 'Library/Application Support/MacNativeGames'
    (support / spec.data_folder).mkdir(parents=True, exist_ok=True)
    return tabs.Context(spec, home / 'Applications/Game.app', home, ROOT, support / spec.data_folder, support / 'Builder')


class Commands(unittest.TestCase):
    def test_each_command_is_one_the_game_takes(self):
        run = Path('/runs/oot-1')
        self.assertEqual(tab_diag.line_for('shot', run, 3), f'wshot {run}/shot-003.png')
        self.assertEqual(tab_diag.line_for('passdump', run, 0), 'passdump 1')
        self.assertEqual(tab_diag.line_for('skip', run, 0, '0123456789abcdef'), 'flag skipfs_0123456789abcdef 1')
        self.assertEqual(tab_diag.line_for('unskip', run, 0, '0123456789abcdef'), 'flag skipfs_0123456789abcdef 0')
        self.assertEqual(tab_diag.line_for('close', run, 0), 'close')
        # what the runtime parses (runtime/input.c, nvn_mtl.m dm_gfx_set_flag)
        source = (ROOT / 'games/oot/runtime/input.c').read_text()
        for parsed in ('"wshot ', '"passdump %u %n"', '"flag ', 'close\\n'):
            self.assertIn(parsed, source)
        self.assertIn('skipfs_', (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text())

    def test_a_bad_shader_or_command_never_reaches_the_game(self):
        for key in ('', '0123', 'ABCDEF0123456789', '0123456789abcdeg', '0123456789abcdef0', 'abc def0123456789'):
            with self.assertRaises(tabs.TabError, msg=key):
                tab_diag.line_for('skip', Path('/r'), 0, key)
        with self.assertRaises(tabs.TabError):
            tab_diag.line_for('flag anything 1', Path('/r'), 0)

    def test_the_switches_are_the_runtime_s(self):
        env = tab_diag.switches('/runs/x')
        self.assertEqual(env['DM_NVN_TSV'], '/runs/x/frames.tsv')
        sources = '\n'.join(p.read_text(errors='replace') for p in (
            ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c', ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m',
            ROOT / 'games/oot/runtime/threads.c', ROOT / 'games/oot/app/launcher.m'))
        for name in env:
            if name != 'TOTK_SLOT_PURPOSE':
                self.assertIn(f'"{name}"', sources, name)

    def test_logs_are_the_games(self):
        for spec, log in ((OOT, 'ocarina'), (TOTK, 'totk')):
            self.assertEqual(tab_diag.LOGS[spec.id], log)


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)
        self.ctx = ctx_for(self.home)

    def tearDown(self):
        self.tmp.cleanup()

    def start(self):
        calls = []
        def runner(argv, **kw):
            calls.append(argv)
            return SimpleNamespace(returncode=0, stderr='')
        with mock.patch.object(tab_diag.game_state, 'game_running', return_value=False):
            result = tab_diag.start(self.ctx, runner=runner)
        return result, calls

    def test_start_opens_the_app_with_the_switches_and_a_run_folder(self):
        result, calls = self.start()
        run = Path(result['run'])
        self.assertTrue(run.is_dir())
        self.assertTrue(str(run).startswith(str(self.home / 'Library/Logs/MacNativeGames/diagnostics/oot-')))
        argv = calls[0]
        self.assertEqual(argv[0], '/usr/bin/open')
        self.assertEqual(argv[-1], str(self.ctx.app))
        self.assertIn(f'DM_NVN_TSV={run}/frames.tsv', argv)
        self.assertIn('TOTK_DEBUG_LOG=1', argv)
        self.assertEqual(json.loads(tab_diag.record_path(self.ctx).read_text())['run'], str(run))

    def test_start_refuses_while_the_game_runs(self):
        with mock.patch.object(tab_diag.game_state, 'game_running', return_value=True), self.assertRaises(tabs.TabError):
            tab_diag.start(self.ctx, runner=lambda *a, **k: self.fail('started'))

    def test_commands_are_appended_and_counted(self):
        result, _ = self.start()
        inputs = self.ctx.data_dir / 'input.txt'
        inputs.write_text('earlier\n')
        with mock.patch.object(tab_diag.game_state, 'game_running', return_value=True):
            tab_diag.command(self.ctx, 'shot')
            tab_diag.command(self.ctx, 'shot')
            tab_diag.command(self.ctx, 'skip', '0123456789abcdef')
        self.assertEqual(inputs.read_text().splitlines(), [
            'earlier', f'wshot {result["run"]}/shot-001.png', f'wshot {result["run"]}/shot-002.png',
            'flag skipfs_0123456789abcdef 1'])

    def test_no_command_without_a_running_diagnostic_game(self):
        with self.assertRaises(tabs.TabError):
            tab_diag.command(self.ctx, 'shot')   # no run yet
        self.start()
        with mock.patch.object(tab_diag.game_state, 'game_running', return_value=False), self.assertRaises(tabs.TabError):
            tab_diag.command(self.ctx, 'shot')
        self.assertFalse((self.ctx.data_dir / 'input.txt').exists())

    def test_the_game_log_is_kept_in_the_run_once_closed(self):
        result, _ = self.start()
        log = tab_diag.game_log(self.ctx)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text('the run\n')
        os.utime(log, (time.time() + 1, time.time() + 1))
        with mock.patch.object(tab_diag.game_state, 'game_running', return_value=False):
            state = tab_diag.state(self.ctx)
        self.assertEqual(Path(state['log']), Path(result['run']) / 'game.log')
        self.assertEqual(Path(state['log']).read_text(), 'the run\n')


class Pages(unittest.TestCase):
    def test_every_game_with_pages_has_the_diagnostic_page_and_play_stays_clean(self):
        from tools.builder.games import PAGES
        self.assertIn('diag', tabs.TABS)
        for game, pages in PAGES.items():
            self.assertIn('diag', pages, game)
        swift = (ROOT / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('case "diag": DiagPage(app: app)', swift)
        play = (ROOT / 'tools/builder/start_options.py').read_text()
        self.assertNotIn('DM_NVN_TSV', play)


if __name__ == '__main__':
    unittest.main()
