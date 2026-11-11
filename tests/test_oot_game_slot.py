"""Ocarina of Time's game slot: the scripts, the runtime and the App's launcher use one folder by default, and the slot
knows Ocarina of Time's own processes.

2026-11-11: the packaging's pipeline-archive step (games/oot/build.py) took the slot in games/oot/analysis/game-slot, the
scripts' old default, while the App's game takes it in ~/Library/Application Support/MacNativeGames/TOTK/game-slot
(app/launcher.m); and neither side counted OcarinaOfTime as a game. So that GPU step never waited for a running game,
and a user's game could not stop an agent's Ocarina of Time test game that held the slot (preempt_holder)."""
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
SHARED = 'Library/Application Support/MacNativeGames/TOTK/game-slot'


def load_slot():
    spec = importlib.util.spec_from_file_location('oot_game_slot_under_test', GAME / 'scripts/dev/game_slot.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def environment(home, **extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_GAME_SLOT')}
    env.update(HOME=str(home), **extra)
    return env


class DefaultFolder(unittest.TestCase):
    def test_scripts_default_to_the_folder_the_game_uses(self):
        slot = load_slot()
        with tempfile.TemporaryDirectory() as home:
            with mock.patch.dict(os.environ, environment(home), clear=True):
                self.assertEqual(slot.slot_dir(), (Path(home) / SHARED).resolve())
            self.assertTrue((Path(home) / SHARED / 'queue').is_dir())

    def test_an_explicit_folder_still_wins(self):
        slot = load_slot()
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as own:
            with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=own), clear=True):
                self.assertEqual(slot.slot_dir(), Path(own).resolve())

    def test_the_launcher_passes_the_same_folder(self):
        self.assertIn(f'@"{SHARED}"', (GAME / 'app/launcher.m').read_text(encoding='utf-8'))

    def test_games_outside_the_slot_are_never_ignored_in_the_shared_folder(self):
        slot = load_slot()
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as own:
            shared = Path(home) / SHARED
            with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=str(shared),
                                                         TOTK_GAME_SLOT_TEST_IGNORE_LEGACY='1'), clear=True):
                with self.assertRaises(SystemExit):
                    slot.test_ignores_legacy(slot.slot_dir())
            with mock.patch.dict(os.environ, environment(home, TOTK_GAME_SLOT_DIR=own,
                                                         TOTK_GAME_SLOT_TEST_IGNORE_LEGACY='1'), clear=True):
                self.assertTrue(slot.test_ignores_legacy(slot.slot_dir()))

    def test_the_packaging_step_takes_the_default_slot(self):
        """build.py names no folder of its own: it gets the scripts' default, the game's folder."""
        source = (GAME / 'build.py').read_text(encoding='utf-8')
        self.assertIn("GameSlot(owner='build',purpose='OOT offline pipelines'", source)
        self.assertNotIn('TOTK_GAME_SLOT_DIR', source)


class GameProcesses(unittest.TestCase):
    GAMES = ('/Applications/Ocarina of Time.app/Contents/MacOS/OcarinaOfTime', '/w/scratch/build/oot/build/oot-native',
             '/w/build/totk-native.base', '/A/Tears of the Kingdom.app/Contents/MacOS/TOTK',
             '/A/Breath of the Wild.app/Contents/MacOS/BotW')
    OTHERS = ('/tmp/OcarinaOfTime', '/A/X.app/Contents/MacOS/OcarinaOfTimeHelper', '/usr/bin/python3', '')

    def test_scripts_know_ocarina_of_time(self):
        slot = load_slot()
        for path in self.GAMES:
            self.assertTrue(slot.is_game_path(path), path)
        for path in self.OTHERS:
            self.assertFalse(slot.is_game_path(path), path)

    def test_runtime_knows_ocarina_of_time_and_the_folder(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as work:
            exe = Path(work) / 'oot-game-slot-probe'
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                    str(ROOT / 'tests/oot_game_slot_probe.c'), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe), '/Users/someone'], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: game processes and the default slot folder', run.stdout)


if __name__ == '__main__':
    unittest.main()
