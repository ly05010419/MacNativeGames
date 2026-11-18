"""A shared game/updater executable must not make its own installer wait forever."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app


class WaitUntilClosed(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.app = Path(self.tmp.name).resolve() / 'Breath of the Wild.app'
        self.binary = self.app / 'Contents/MacOS/BotW'
        self.binary.parent.mkdir(parents=True)
        self.binary.touch()

    def listing(self):   # what running_executables() answers: one process running the app's binary
        return [(101, str(self.binary.resolve()))]

    def patched(self, args):
        return (patch.object(build_app, 'running_executables', side_effect=self.listing),
                patch.object(build_app, 'process_args', side_effect=args))

    def test_only_exact_update_modes_are_excluded(self):
        for options, should_wait in [
            ('--update-install 12986', False),
            ('--update-check', False),
            ('', True),
            ('boot', True),
            ('--update-install 0', True),
            ('--update-install invalid', True),
            ('--update-install 12986 extra', True),
            ('--update-check extra', True),
            ('--other --update-install 12986', True),
        ]:
            with self.subTest(options=options):
                command = f'{self.binary.resolve()} {options}'.strip()
                listing, args = self.patched(lambda pid: command)
                with listing, args, patch.object(build_app.time, 'sleep', side_effect=InterruptedError), \
                        patch.object(build_app, 'say'):
                    if should_wait:
                        with self.assertRaises(InterruptedError):
                            build_app.wait_until_closed(self.app)
                    else:
                        build_app.wait_until_closed(self.app)

    def test_exited_process_does_not_block_install(self):
        listing, args = self.patched(lambda pid: None)   # exited between the listing and the argument query
        with listing, args, patch.object(build_app.time, 'sleep') as sleep:
            build_app.wait_until_closed(self.app)
            sleep.assert_not_called()

    def test_process_query_error_is_not_treated_as_closed(self):
        def fail(pid):
            raise OSError(1, 'cannot read the arguments')
        listing, args = self.patched(fail)
        with listing, args:
            with self.assertRaises(OSError):
                build_app.wait_until_closed(self.app)
        with patch.object(build_app, 'running_executables', side_effect=OSError(1, 'cannot list processes')):
            with self.assertRaises(OSError):
                build_app.wait_until_closed(self.app)

    def test_resource_runtime_is_still_waited_for(self):
        self.binary = self.app / 'Contents/Resources/totk/build/totk-native'
        self.binary.parent.mkdir(parents=True)
        self.binary.touch()
        command = f'{self.binary.resolve()} rewritten/botw160 boot'
        listing, args = self.patched(lambda pid: command)
        with listing, args, patch.object(build_app.time, 'sleep', side_effect=InterruptedError), \
                patch.object(build_app, 'say'):
            with self.assertRaises(InterruptedError):
                build_app.wait_until_closed(self.app)

    def test_no_ps(self):
        """/bin/ps is setuid and cannot run under this machine's storage guard: the process queries use libproc / sysctl."""
        source = (ROOT / 'build_app.py').read_text()
        body = source[source.index('def wait_until_closed('):source.index('def record_update_source(')]
        self.assertNotIn("'ps'", body)
        self.assertIn('running_executables()', body)

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'macOS process paths and clang required')
    def test_real_updater_and_game_share_executable(self):
        source = Path(self.tmp.name) / 'process.c'
        source.write_text('#include <unistd.h>\n#include <stdio.h>\n'
                          'int main(void) { puts("ready"); fflush(stdout); for (;;) pause(); }\n')
        subprocess.run(['clang', str(source), '-o', str(self.binary)], check=True, timeout=30)
        updater = subprocess.Popen([str(self.binary), '--update-install', '12986'], stdout=subprocess.PIPE, text=True)
        game = None
        try:
            self.assertEqual(updater.stdout.readline(), 'ready\n')
            with patch.object(build_app.time, 'sleep', side_effect=AssertionError('waited for updater')):
                build_app.wait_until_closed(self.app)
            game = subprocess.Popen([str(self.binary), 'boot'], stdout=subprocess.PIPE, text=True)
            self.assertEqual(game.stdout.readline(), 'ready\n')
            with patch.object(build_app.time, 'sleep', side_effect=InterruptedError), \
                    patch.object(build_app, 'say'):
                with self.assertRaises(InterruptedError):
                    build_app.wait_until_closed(self.app)
        finally:
            for process in (game, updater):
                if process is not None:
                    process.terminate()
                    process.wait(timeout=10)
                    process.stdout.close()


if __name__ == '__main__':
    unittest.main()
