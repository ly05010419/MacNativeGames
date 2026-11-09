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

    def processes(self, *args, **kwargs):
        result = subprocess.CompletedProcess(args, 0, stdout='', stderr='')
        if args[0] == ['ps', '-axo', 'pid=,comm=']:
            result.stdout = f'  101 {self.binary.resolve()}\n'
        else:
            result.stdout = self.command
        return result

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
                self.command = f'{self.binary.resolve()} {options}\n'
                with patch.object(build_app.subprocess, 'run', side_effect=self.processes), \
                        patch.object(build_app.time, 'sleep', side_effect=InterruptedError), \
                        patch.object(build_app, 'say'):
                    if should_wait:
                        with self.assertRaises(InterruptedError):
                            build_app.wait_until_closed(self.app)
                    else:
                        build_app.wait_until_closed(self.app)

    def test_exited_process_does_not_block_install(self):
        results = [
            subprocess.CompletedProcess([], 0, stdout=f'101 {self.binary.resolve()}\n'),
            subprocess.CompletedProcess([], 1, stdout='', stderr=''),
        ]
        with patch.object(build_app.subprocess, 'run', side_effect=results), \
                patch.object(build_app.time, 'sleep') as sleep:
            build_app.wait_until_closed(self.app)
            sleep.assert_not_called()

    def test_process_query_error_is_not_treated_as_closed(self):
        results = [
            subprocess.CompletedProcess([], 0, stdout=f'101 {self.binary.resolve()}\n'),
            subprocess.CompletedProcess([], 2, stdout='', stderr='ps failed'),
        ]
        with patch.object(build_app.subprocess, 'run', side_effect=results):
            with self.assertRaises(subprocess.CalledProcessError):
                build_app.wait_until_closed(self.app)

    def test_resource_runtime_is_still_waited_for(self):
        self.binary = self.app / 'Contents/Resources/totk/build/totk-native'
        self.binary.parent.mkdir(parents=True)
        self.binary.touch()
        self.command = f'{self.binary.resolve()} rewritten/botw160 boot\n'
        with patch.object(build_app.subprocess, 'run', side_effect=self.processes), \
                patch.object(build_app.time, 'sleep', side_effect=InterruptedError), \
                patch.object(build_app, 'say'):
            with self.assertRaises(InterruptedError):
                build_app.wait_until_closed(self.app)

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
