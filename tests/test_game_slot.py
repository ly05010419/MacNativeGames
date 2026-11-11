"""The game slot of Tears of the Kingdom (games/totk) and Ocarina of Time (games/oot): the scripts, the runtime and the
App's launcher use one folder by default, and the slot knows every App's game.

2026-11-11: both copies of scripts/dev/game_slot.py and runtime/game_slot.c defaulted to <game>/analysis/game-slot,
while the Apps' games take the slot in ~/Library/Application Support/MacNativeGames/TOTK/game-slot (app/launcher.m);
and neither counted OcarinaOfTime as a game. So Ocarina of Time's packaging step (games/oot/build.py, pipeline-archive)
and Tears of the Kingdom's Makefile probes (GAME_SLOT_RUN) never waited for a running game, and a user's game could not
stop an agent's Ocarina of Time test game holding the slot (preempt_holder only signals a pid it counts as a game)."""
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAMES = ('totk', 'oot')
SHARED = 'Library/Application Support/MacNativeGames/TOTK/game-slot'


def load_slot(game):
    spec = importlib.util.spec_from_file_location(f'{game}_game_slot_under_test', ROOT / f'games/{game}/scripts/dev/game_slot.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def environment(home, **extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_GAME_SLOT')}
    env.update(HOME=str(home), **extra)
    return env


class DefaultFolder(unittest.TestCase):
    def test_scripts_default_to_the_folder_the_game_uses(self):
        for game in GAMES:
            slot = load_slot(game)
            with tempfile.TemporaryDirectory() as home:
                with mock.patch.dict(os.environ, environment(home), clear=True):
                    self.assertEqual(slot.slot_dir(), (Path(home) / SHARED).resolve(), game)
                self.assertTrue((Path(home) / SHARED / 'queue').is_dir(), game)

    def test_an_explicit_folder_still_wins(self):
        for game in GAMES:
            slot = load_slot(game)
            with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as own:
                with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=own), clear=True):
                    self.assertEqual(slot.slot_dir(), Path(own).resolve(), game)

    def test_the_launchers_pass_the_same_folder(self):
        self.assertIn(f'@"{SHARED}"', (ROOT / 'games/oot/app/launcher.m').read_text(encoding='utf-8'))
        totk = (ROOT / 'games/totk/app/launcher.m').read_text(encoding='utf-8')
        self.assertIn('@"Library/Application Support/MacNativeGames/" GAME_FOLDER', totk)
        self.assertIn('[data stringByAppendingPathComponent:@"game-slot"]', totk)
        self.assertIn('#define GAME_FOLDER "TOTK"', (ROOT / 'games/totk/runtime/game_version.h').read_text(encoding='utf-8'))

    def test_games_outside_the_slot_are_never_ignored_in_the_shared_folder(self):
        for game in GAMES:
            slot = load_slot(game)
            with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as own:
                shared = Path(home) / SHARED
                with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=str(shared),
                                                             TOTK_GAME_SLOT_TEST_IGNORE_LEGACY='1'), clear=True):
                    with self.assertRaises(SystemExit, msg=game):
                        slot.test_ignores_legacy(slot.slot_dir())
                with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=own,
                                                             TOTK_GAME_SLOT_TEST_IGNORE_LEGACY='1'), clear=True):
                    self.assertTrue(slot.test_ignores_legacy(slot.slot_dir()), game)

    def test_the_packaging_step_and_the_probes_take_the_default_slot(self):
        """Neither names a folder of its own: they get the scripts' default, the game's folder."""
        source = (ROOT / 'games/oot/build.py').read_text(encoding='utf-8')
        self.assertIn("GameSlot(owner='build',purpose='OOT offline pipelines'", source)
        self.assertNotIn('TOTK_GAME_SLOT_DIR', source)
        makefile = (ROOT / 'games/totk/Makefile').read_text(encoding='utf-8')
        self.assertIn('GAME_SLOT_RUN = python3 scripts/dev/game_slot.py run', makefile)
        self.assertNotIn('TOTK_GAME_SLOT_DIR', makefile)


class GameProcesses(unittest.TestCase):
    GAME_PATHS = ('/Applications/Ocarina of Time.app/Contents/MacOS/OcarinaOfTime', '/w/scratch/build/oot/build/oot-native',
                  '/w/build/totk-native.base', '/A/Tears of the Kingdom.app/Contents/MacOS/TOTK',
                  '/A/Breath of the Wild.app/Contents/MacOS/BotW',
                  '/A/Pokémon Pokopia.app/Contents/MacOS/Pokopia', '/A/Donkey Kong Bananza.app/Contents/MacOS/Bananza')
    OTHER_PATHS = ('/tmp/OcarinaOfTime', '/A/X.app/Contents/MacOS/OcarinaOfTimeHelper', '/usr/bin/python3', '')

    def test_every_app_the_packaging_makes_is_known(self):
        """build_app.py APPS names each App's game program (its third field); every one of them is a game to the slot.
        Read from the source: Bananza and Pokopia use their own slot folders, so only their process name tells."""
        import ast
        tree = ast.parse((ROOT / 'build_app.py').read_text(encoding='utf-8'))
        names = set()
        for node in ast.walk(tree):
            value = getattr(node, 'value', None)
            targets = getattr(node, 'targets', [])
            if not (isinstance(value, (ast.Dict, ast.Tuple)) and targets):
                continue
            target = targets[0]
            is_apps = (isinstance(target, ast.Name) and target.id == 'APPS') or \
                      (isinstance(target, ast.Subscript) and getattr(target.value, 'id', '') == 'APPS')
            if not is_apps:
                continue
            for entry in (value.values if isinstance(value, ast.Dict) else [value]):
                if isinstance(entry, ast.Tuple) and len(entry.elts) == 3 and isinstance(entry.elts[2], ast.Constant):
                    names.add(entry.elts[2].value)
        self.assertTrue({'TOTK', 'BotW', 'Pokopia', 'Bananza'} <= names, names)
        for game in GAMES:
            slot = load_slot(game)
            for name in sorted(names):
                self.assertTrue(slot.is_game_path(f'/A/X.app/Contents/MacOS/{name}'), f'{game}: {name}')

    def test_scripts_know_every_apps_game(self):
        for game in GAMES:
            slot = load_slot(game)
            for path in self.GAME_PATHS:
                self.assertTrue(slot.is_game_path(path), f'{game}: {path}')
            for path in self.OTHER_PATHS:
                self.assertFalse(slot.is_game_path(path), f'{game}: {path}')

    def test_runtimes_know_every_apps_game_and_the_folder(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        for game in GAMES:
            with self.subTest(game=game), tempfile.TemporaryDirectory() as work:
                exe = Path(work) / 'game-slot-probe'
                build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                        f'-DGAME_SLOT_C="../games/{game}/runtime/game_slot.c"',
                                        str(ROOT / 'tests/game_slot_probe.c'), '-o', str(exe)],
                                       capture_output=True, text=True, timeout=120)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                run = subprocess.run([str(exe), '/Users/someone'], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: game processes and the default slot folder', run.stdout)


class AgentGameWatch(unittest.TestCase):
    """An agent's test game closes itself when nobody uses it (runtime/agent_slot_limit.h; user 2026-11-11: "你这样开着游戏
    别的session 无法使用 … 必须用代码保证"): the real runtime/game_slot.c in tests/game_slot_probe.c, a test slot folder, short
    TOTK_AGENT_* times, and a timeline of the first frame, input-file commands and user input."""
    # name: (environment, probe timeline, the close expected between these seconds, or None: no close)
    CASES = {
        'idle from the last command, not before the first frame': (
            dict(TOTK_SLOT_OWNER='claude', TOTK_AGENT_IDLE_S='2'), ['3', '9', 'c1', 'c4'], (5.9, 7.0)),
        'the user\'s game is never closed': (
            dict(TOTK_SLOT_OWNER='user', TOTK_AGENT_IDLE_S='1', TOTK_SLOT_EXPECT_S='1', TOTK_AGENT_GRACE_S='0'), ['0', '3'], None),
        'a game without an owner is never closed': (
            dict(TOTK_AGENT_IDLE_S='1', TOTK_SLOT_EXPECT_S='1', TOTK_AGENT_GRACE_S='0'), ['0', '3'], None),
        'over its expected time': (
            dict(TOTK_SLOT_OWNER='claude', TOTK_AGENT_IDLE_S='0', TOTK_SLOT_EXPECT_S='2', TOTK_AGENT_GRACE_S='1'),
            ['0', '9', 'c1', 'c2', 'c2.5'], (2.9, 4.0)),
        'not over its time while the user plays': (
            dict(TOTK_SLOT_OWNER='claude', TOTK_AGENT_IDLE_S='0', TOTK_SLOT_EXPECT_S='1', TOTK_AGENT_GRACE_S='0'),
            ['0', '4', 'u0.5'], None),
    }

    def test_both_runtimes_share_one_rule(self):
        self.assertEqual((ROOT / 'games/totk/runtime/agent_slot_limit.h').read_bytes(),
                         (ROOT / 'games/oot/runtime/agent_slot_limit.h').read_bytes())
        for game in GAMES:
            source = (ROOT / f'games/{game}/runtime/input.c').read_text(encoding='utf-8')
            self.assertIn('runtime_slot_activity(1);', source, game)   # user_input()
            self.assertIn('runtime_slot_activity(0);', source, game)   # every input-file command

    def test_agent_games_close_themselves(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        for game in GAMES:
            with self.subTest(game=game), tempfile.TemporaryDirectory() as work:
                exe = Path(work) / 'game-slot-probe'
                build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                        f'-DGAME_SLOT_C="../games/{game}/runtime/game_slot.c"',
                                        str(ROOT / 'tests/game_slot_probe.c'), '-o', str(exe)],
                                       capture_output=True, text=True, timeout=120)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                runs = {}
                for i, (name, (extra, timeline, _)) in enumerate(self.CASES.items()):
                    slot = Path(work) / f'slot{i}'
                    env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
                    env.update(HOME=work, TOTK_GAME_SLOT_DIR=str(slot), TOTK_GAME_SLOT_TEST_IGNORE_LEGACY='1', **extra)
                    runs[name] = (slot, subprocess.Popen([str(exe), 'watch', *timeline], env=env, text=True,
                                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE))
                for name, (slot, proc) in runs.items():
                    out, err = proc.communicate(timeout=60)
                    window = self.CASES[name][2]
                    with self.subTest(case=name):
                        self.assertEqual(proc.returncode, 0, out + err)
                        history = (slot / 'history.log').read_text(encoding='utf-8')
                        if window is None:
                            self.assertIn('no close by', out, err)
                            self.assertNotIn('agent-close', history)
                            continue
                        self.assertIn('close requested at', out, err)
                        at = float(out.split('close requested at ')[1].split()[0])
                        self.assertTrue(window[0] <= at <= window[1], f'closed at {at} s, expected {window}\n{err}')
                        self.assertIn('[slot] agent game claude: closes itself after', err)
                        self.assertIn('agent-close owner=claude', history)
                        if 'idle' in name:
                            self.assertIn('[slot] agent game claude closed: no command or input for 2 s', err)
                            self.assertIn('reason=idle', history)
                        else:
                            self.assertIn('[slot] agent game claude closed: over its expected 2 s', err)
                            self.assertIn('[slot] warning: TOTK_AGENT_IDLE_S=0', err)
                            self.assertIn('reason=limit', history)


if __name__ == '__main__':
    unittest.main()
