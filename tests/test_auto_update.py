"""The Update page's check and install (games/totk/app/auto_update.h): real git repositories in a temporary folder, a stand-in for
update_runtime.py, nothing of the player's."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation requires macOS')
class AutoUpdate(unittest.TestCase):
    def test_probe(self):
        compiler = shutil.which('clang')
        if not compiler or not shutil.which('git'):
            self.skipTest('clang and git are required')
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory).resolve() / 'probe'
            subprocess.run([compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/auto_update_probe.m'),
                            '-framework', 'Foundation', '-o', str(probe)], check=True, timeout=90)
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: auto update', run.stdout)

    def test_launcher_updates_only_when_asked(self):
        launcher = (ROOT / 'games/totk/app/launcher.m').read_text(encoding='utf-8')
        main = launcher[launcher.index('int main(int argc, char **argv) {'):]
        self.assertIn('return update_check_run();', main)
        self.assertIn('return update_install_run((pid_t)atoi(argv[2]));', main)
        self.assertNotIn('mng_update_install(', main)   # nothing at launch: only the Update button installs
        self.assertLess(main.index('setenv("TOTK_UPDATE_LAUNCHER"'), main.index('execv(arguments[0], arguments);'))
        self.assertNotIn('fork(', (ROOT / 'games/totk/runtime/update_tab.h').read_text(encoding='utf-8'))   # the game's address space is huge


if __name__ == '__main__':
    unittest.main()
