"""The app launchers of Super Mario Odyssey, Animal Crossing and Echoes of Wisdom on their shared core
(shared/app/app_launcher.h, 2026-11-18): each game's own launcher compiled as its packager compiles it, put in a fake
bundle in a temporary folder, and run with HOME in that folder and a shell script as the runtime. Nothing here reads
or writes a player's data folder, and no game is started."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SUPPORT = 'Library/Application Support/MacNativeGames'

# The fake runtime: it records its arguments, environment and working directory, then does what FAKE_MODE asks.
RUNTIME = r'''#!/bin/sh
echo run >> runs.txt
echo "$@" > args.txt
pwd > cwd.txt
env | sort > env.txt
echo "runtime output $$"
case "$FAKE_MODE" in
  restart-once) if [ ! -f restarted ]; then touch restarted; echo restart > restart-requested.txt; fi ;;
  crash) exit 3 ;;
  session)   # the settings panel's row changes the session limit while the game runs; the game waits for "close"
    echo 0.05 > session-minutes.txt
    i=0
    while [ $i -lt 40 ]; do
      if grep -q close commands.txt 2>/dev/null; then exit 0; fi
      sleep 0.5; i=$((i+1))
    done
    exit 4 ;;
esac
exit 0
'''


class Game:
    def __init__(self, name, source, executable, runtime, folder, old_folder, defines=(), frameworks=(), extra=()):
        self.name, self.source, self.executable, self.runtime = name, source, executable, runtime
        self.folder, self.old_folder, self.defines, self.frameworks, self.extra = folder, old_folder, defines, frameworks, extra


ODYSSEY = Game('odyssey', 'games/odyssey/scripts/odyssey_app_launcher.c', 'Odyssey', 'MacOS/odyssey-native', 'Odyssey',
               'Odyssey Data', frameworks=['CoreFoundation'])
ACNH = Game('acnh', 'games/acnh/scripts/acnh_app_launcher.c', 'Animal Crossing', 'MacOS/acnh-native', 'ACNH-3.0.3',
            'Animal Crossing Data', defines=['ACNH_DATA_FOLDER="ACNH-3.0.3"'])
EOW = Game('eow', 'games/eow/app/launcher.c', 'Echoes of Wisdom', 'Resources/game/build/eow-native', 'EoW', None,
           frameworks=['CoreFoundation'], extra=['Resources/game/romfs/runtime-index.bin'])

_built = {}


def launcher_binary(game, folder):
    """The game's launcher, compiled once per test run with its packager's flags (tools/port_app/bundle.py CLANG)."""
    if game.name not in _built:
        out = Path(folder) / f'{game.name}-launcher'
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', *(f'-D{d}' for d in game.defines),
                        str(ROOT / game.source), *(a for f in game.frameworks for a in ('-framework', f)), '-o', str(out)],
                       check=True)
        _built[game.name] = out
    return _built[game.name]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class LauncherCase(unittest.TestCase):
    game = None

    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()
        _built.clear()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.home = self.root / 'home'
        self.home.mkdir()
        self.apps = self.root / 'apps'
        self.app = self.apps / f'{self.game.executable}.app'
        contents = self.app / 'Contents'
        (contents / 'MacOS').mkdir(parents=True)
        self.exe = contents / 'MacOS' / self.game.executable
        shutil.copy2(launcher_binary(self.game, self.compiled.name), self.exe)
        runtime = contents / self.game.runtime
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.write_text(RUNTIME)
        runtime.chmod(0o755)
        for path in self.game.extra:
            (contents / path).parent.mkdir(parents=True, exist_ok=True)
            (contents / path).write_bytes(b'')

    @property
    def data(self):
        return self.home / SUPPORT / self.game.folder

    def launch(self, *args, timeout=60, **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'ACNH_', 'EOW_', 'ODYSSEY_', 'DM_', 'BY3_'))}
        return subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=timeout,
                              env={**clean, 'HOME': str(self.home), **env})

    def env(self):
        lines = (self.data / 'env.txt').read_text().splitlines()
        return dict(line.split('=', 1) for line in lines if '=' in line)

    def save(self, text='save'):
        save = self.data / 'profile/save'
        save.mkdir(parents=True, exist_ok=True)
        (save / 'File1.bin').write_text(text)


class Common:
    """What every launcher does the same way (the shared core)."""

    def test_the_data_folder_is_made_and_the_common_environment_names_it(self):
        done = self.launch()
        self.assertEqual(done.returncode, 0, done.stderr)
        env = self.env()
        game = self.app / 'Contents/Resources/game'
        self.assertEqual(Path((self.data / 'cwd.txt').read_text().strip()).resolve(), self.data.resolve())
        self.assertEqual(env['TOTK_PROFILE_DIR'], str(self.data / 'profile'))
        self.assertEqual(env['TOTK_ROMFS'], str(game / 'romfs'))
        self.assertEqual(env['TOTK_SHARED_FONTS'], str(game / 'fonts'))
        self.assertEqual(env['TOTK_ROMFS_PROOF_DIR'], str(self.data))
        self.assertEqual(env['TOTK_INPUT_FILE'], str(self.data / 'commands.txt'))
        self.assertEqual(env['TOTK_CONTROLS'], str(self.data / 'controls.txt'))
        self.assertEqual(env['TOTK_BOARD_DIR'], str(self.data))
        self.assertEqual(env['TOTK_GAME_SLOT_DIR'], str(self.data / 'game-slot'))
        self.assertEqual(env['TOTK_SLOT_OWNER'], 'user')
        self.assertTrue((self.data / 'profile').is_dir())
        self.assertEqual((self.data / 'profile').stat().st_mode & 0o777, 0o700)
        self.assertTrue((self.data / 'args.txt').read_text().strip().endswith(' boot'))

    def test_test_switches_are_removed_and_a_given_board_folder_is_kept(self):
        self.launch(TOTK_HEADLESS='1', TOTK_ASSUME_FOCUS='1', TOTK_VERIFY_STARTUP='1', TOTK_BOARD_DIR=str(self.root))
        env = self.env()
        for name in ('TOTK_HEADLESS', 'TOTK_ASSUME_FOCUS', 'TOTK_VERIFY_STARTUP'):
            self.assertNotIn(name, env)
        self.assertEqual(env['TOTK_BOARD_DIR'], str(self.root))

    def test_the_log_of_the_last_session_is_kept(self):
        self.launch()
        first = (self.data / 'last-run.log').read_text()
        self.launch()
        self.assertEqual((self.data / 'previous-run.log').read_text(), first)
        self.assertNotEqual((self.data / 'last-run.log').read_text(), first)
        self.assertIn('runtime output', first)

    def test_backups_keep_the_newest_five(self):
        self.save('now')
        backups = self.data / 'backups'
        for i in range(6):   # older starts' backups
            (backups / f'2000010{i}-000000-1').mkdir(parents=True)
        done = self.launch()
        self.assertEqual(done.returncode, 0, done.stderr)
        kept = sorted(p.name for p in backups.iterdir())
        self.assertEqual(len(kept), 5, kept)
        newest = kept[-1]
        self.assertFalse(newest.startswith('2000'))
        self.assertEqual((backups / newest / 'File1.bin').read_text(), 'now')
        self.assertNotIn('20000100-000000-1', kept)
        self.assertNotIn('20000101-000000-1', kept)

    def test_no_save_no_backup(self):
        self.launch()
        self.assertFalse((self.data / 'backups').exists())

    def test_restart_requested_starts_the_game_again_with_a_new_backup(self):
        self.save()
        done = self.launch(FAKE_MODE='restart-once')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((self.data / 'runs.txt').read_text().split(), ['run', 'run'])
        self.assertFalse((self.data / 'restart-requested.txt').exists())
        self.assertGreaterEqual(len(list((self.data / 'backups').iterdir())), 1)

    def test_a_left_restart_request_is_removed_before_the_start(self):
        self.data.mkdir(parents=True)
        (self.data / 'restart-requested.txt').write_text('old')
        self.launch()
        self.assertEqual((self.data / 'runs.txt').read_text().split(), ['run'])

    def test_the_game_exit_code_is_returned(self):
        self.assertEqual(self.launch(FAKE_MODE='crash').returncode, 3)


class Moves:
    """The one-time move of the data folder from next to the app."""

    def test_an_old_folder_next_to_the_app_is_moved_once(self):
        old = self.apps / self.game.old_folder
        (old / 'profile/save').mkdir(parents=True)
        (old / 'profile/save/File1.bin').write_text('old save')
        done = self.launch()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertFalse(old.exists())
        self.assertEqual((self.data / 'profile/save/File1.bin').read_text(), 'old save')

    def test_data_already_in_the_new_place_is_never_replaced(self):
        self.save('new save')
        old = self.apps / self.game.old_folder
        (old / 'profile/save').mkdir(parents=True)
        (old / 'profile/save/File1.bin').write_text('old save')
        self.launch()
        self.assertTrue(old.is_dir())
        self.assertEqual((self.data / 'profile/save/File1.bin').read_text(), 'new save')


class Check:
    """--check: the packager's self-check; the game is not started."""

    def test_check_does_not_start_the_game(self):
        done = self.launch('--check')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('not launched', done.stdout)
        self.assertFalse((self.home / 'Library').exists())

    def test_check_refuses_a_bundle_without_runtime(self):
        (self.app / 'Contents' / self.game.runtime).unlink()
        self.assertNotEqual(self.launch('--check').returncode, 0)

    def test_other_arguments_get_the_usage(self):
        done = self.launch('--nope')
        self.assertEqual(done.returncode, 2)
        self.assertIn(f'usage: {self.game.executable} [--check]', done.stderr)

    def test_the_data_folder_override_must_be_absolute(self):
        variable = f'{self.game.name.upper()}_DATA_DIR'
        self.assertEqual(self.launch(**{variable: 'relative/folder'}).returncode, 1)
        folder = self.root / 'elsewhere'
        done = self.launch(**{variable: str(folder)})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue((folder / 'runs.txt').is_file())


class OdysseyLauncher(Common, Moves, LauncherCase):
    game = ODYSSEY

    def test_odyssey_environment(self):
        self.launch()
        env = self.env()
        self.assertEqual(env['TOTK_SHADER_CACHE'], str(self.data / 'shader-cache/rt-totk-native-v4-unverified'))
        self.assertEqual(env['TOTK_SLOT_PURPOSE'], 'Odyssey.app')
        self.assertEqual((env['TOTK_MFX_UPSCALE'], env['TOTK_MFX_OUTPUT'], env['BY3_FULLSCREEN']), ('1', '1', '1'))
        self.assertEqual(env['ODYSSEY_GFX_SETTINGS'], str(self.data / 'graphics.txt'))
        self.assertEqual(env['TOTK_SHADER_CONTRIB'], str(self.data / 'shader-list-contribution.txt'))
        self.assertTrue(env.get('TOTK_LOCALE'))   # the Mac's first preferred language (launcher_set_locale)
        self.assertNotIn('TOTK_FACE_BUTTONS', env)

    def test_display_face_buttons_and_no_metalfx(self):
        self.data.mkdir(parents=True)
        (self.data / 'display.txt').write_text('window\n')
        (self.data / 'face-buttons.txt').write_text('position\n')
        (self.data / 'no-metalfx').write_text('')
        self.launch()
        env = self.env()
        self.assertNotIn('BY3_FULLSCREEN', env)
        self.assertNotIn('TOTK_MFX_UPSCALE', env)
        self.assertEqual(env['TOTK_FACE_BUTTONS'], 'position')

    def test_a_given_controls_file_decides_first(self):
        self.launch(TOTK_CONTROLS=str(self.root / 'mine.txt'))
        self.assertEqual(self.env()['TOTK_CONTROLS'], str(self.root / 'mine.txt'))

    def test_romfs_mods_are_the_data_folders_mods_for_the_apps_version(self):   # issue #1788
        self.launch()   # no game-version.txt: 1.3.0 (runtime/game_version.h)
        self.assertEqual(self.env()['TOTK_ROMFS_MODS'], str(self.data / 'mods/v130'))
        resources = self.app / 'Contents/Resources'
        resources.mkdir(parents=True, exist_ok=True)
        (resources / 'game-version.txt').write_text('1.4.1\n')
        self.launch()
        env = self.env()
        self.assertEqual(env['ODYSSEY_GAME_VERSION'], '1.4.1')
        self.assertEqual(env['TOTK_ROMFS_MODS'], str(self.data / 'mods/v141'))

    def test_a_given_romfs_mods_folder_decides_first(self):
        self.launch(TOTK_ROMFS_MODS='0')
        self.assertEqual(self.env()['TOTK_ROMFS_MODS'], '0')

    def test_backups_are_named_by_time_alone(self):
        self.save()
        self.launch()
        self.assertRegex(next((self.data / 'backups').iterdir()).name, r'^\d{8}-\d{6}$')

    def test_a_crash_keeps_its_log_apart(self):
        self.launch(FAKE_MODE='crash')
        crashes = list(self.data.glob('crash-*.log'))
        self.assertEqual(len(crashes), 1)
        self.assertIn('runtime output', crashes[0].read_text())

    def test_the_session_limit_is_read_again_while_the_game_runs(self):
        self.data.mkdir(parents=True)
        (self.data / 'session-minutes.txt').write_text('0\n')   # no limit when the game starts
        started = time.monotonic()
        done = self.launch(FAKE_MODE='session', timeout=60)   # the game sets 0.05 minutes (3 s) while it runs
        took = time.monotonic() - started
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('close', (self.data / 'commands.txt').read_text())
        self.assertIn('0.05-minute session limit: closing the game', (self.data / 'last-run.log').read_text())
        self.assertLess(took, 15)
        self.assertEqual(list(self.data.glob('crash-*.log')), [])


class AnimalCrossingLauncher(Common, Moves, Check, LauncherCase):
    game = ACNH

    def test_animal_crossing_environment(self):
        game = self.app / 'Contents/Resources/game'
        (game / 'fps60').mkdir(parents=True)
        (game / 'fps60/manifest.json').write_text('{}')
        self.launch()
        env = self.env()
        self.assertEqual(env['TOTK_SHADER_CACHE'], str(self.data / 'shader-cache'))
        self.assertTrue((self.data / 'shader-cache').is_dir())
        self.assertEqual(env['TOTK_SLOT_PURPOSE'], 'Animal Crossing New Horizons.app')
        self.assertEqual(env['ACNH_DLC_DIR'], str(game / 'dlc'))
        self.assertEqual(env['ACNH_FPS_ASSETS'], str(game / 'fps60'))
        self.assertNotIn('ACNH_FPS120_ASSETS', env)
        self.assertEqual((env['DM_PRESENT_HZ'], env['TOTK_UNCAPPED'], env['TOTK_GPU_MIDCLEAR']), ('60', '0', '1'))
        self.assertNotIn('TOTK_LOCALE', env)   # runtime/services.c reads the Mac's language itself

    def test_a_given_controls_file_is_replaced(self):
        self.launch(TOTK_CONTROLS=str(self.root / 'mine.txt'))
        self.assertEqual(self.env()['TOTK_CONTROLS'], str(self.data / 'controls.txt'))

    def test_backups_carry_the_process_id(self):
        self.save()
        self.launch()
        self.assertRegex(next((self.data / 'backups').iterdir()).name, r'^\d{8}-\d{6}-\d+$')


class EchoesOfWisdomLauncher(Common, Check, LauncherCase):
    game = EOW

    def test_echoes_environment_and_real_path_start(self):
        self.launch()
        env = self.env()
        self.assertEqual(env['TOTK_SHADER_CACHE'], str(self.data / 'shader-cache'))
        self.assertEqual(env['TOTK_SLOT_PURPOSE'], 'Echoes of Wisdom.app')
        self.assertEqual((env['TOTK_GPU_BUDGET_MB'], env['DM_PRESENT_LINK_FALLBACK']), ('5000', '1'))
        self.assertTrue((self.data / 'args.txt').read_text().startswith(
            str(self.app / 'Contents/Resources/game/rewritten/v110') + ' '))

    def test_the_language_is_the_macs_first_preferred_one_unless_given(self):
        self.launch()
        tag = self.env().get('TOTK_LOCALE')
        self.assertTrue(tag and re.match(r'^[A-Za-z]{2,3}([-_][A-Za-z0-9]+)*$', tag), tag)
        self.launch(TOTK_LOCALE='ja-JP')
        self.assertEqual(self.env()['TOTK_LOCALE'], 'ja-JP')
        self.launch(TOTK_LANG='fr')
        self.assertNotIn('TOTK_LOCALE', self.env())

    def test_check_needs_the_game_data_too(self):
        (self.app / 'Contents/Resources/game/romfs/runtime-index.bin').unlink()
        self.assertNotEqual(self.launch('--check').returncode, 0)

    def test_the_runtime_is_started_by_its_real_path(self):
        source = (ROOT / self.game.source).read_text()
        self.assertIn('launcher_join(runtime, b.game, "build/eow-native")', source)


class SharedFile(unittest.TestCase):
    def test_each_game_reaches_the_shared_file_through_a_link(self):
        shared = ROOT / 'shared/app/app_launcher.h'
        for link in ('games/odyssey/scripts/app_launcher.h', 'games/acnh/scripts/app_launcher.h', 'games/eow/app/app_launcher.h'):
            path = ROOT / link
            self.assertTrue(path.is_symlink(), link)
            self.assertFalse(os.readlink(path).startswith('/'), link)
            self.assertTrue(path.resolve().samefile(shared), link)


if __name__ == '__main__':
    unittest.main()
