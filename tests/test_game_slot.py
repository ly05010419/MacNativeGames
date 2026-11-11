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


if __name__ == '__main__':
    unittest.main()
