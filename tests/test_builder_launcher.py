"""Build Game.command (S2.4): the double-click launcher, run with bash and a fake Python that records how it was
called. Every run starts from a folder that is not the script's, so the script's own `cd` is what is tested."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'Build Game.command'
FAKE_PYTHON = '''#!/bin/bash
if [ "$1" = "-c" ]; then exit "$TK_STATUS"; fi
{ pwd; printf '%s\\n' "$@"; } > "$RECORD"
'''


class Launcher(unittest.TestCase):
    script = SCRIPT                     # the mutation script points this at a changed copy

    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp)
        self.python = self.tmp / 'python3.14'
        self.python.write_text(FAKE_PYTHON)
        self.python.chmod(0o755)
        self.record = self.tmp / 'record'

    def run_script(self, *args, tk=True, python=None):
        env = {'PATH': '/usr/bin:/bin', 'HOME': str(self.tmp), 'RECORD': str(self.record),
               'TK_STATUS': '0' if tk else '1', 'BUILDER_PYTHON': str(python or self.python)}
        return subprocess.run(['/bin/bash', str(self.script), *args], cwd=self.tmp, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=10)

    def recorded(self):
        return self.record.read_text().splitlines() if self.record.exists() else None

    def test_a_double_click_runs_the_builder_from_the_scripts_folder(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(self.recorded(), [str(self.script.parent), '-m', 'tools.builder'])

    def test_arguments_pass_through(self):
        self.run_script('--cli', '--game', 'totk', '--check-only')
        self.assertEqual(self.recorded()[1:], ['-m', 'tools.builder', '--cli', '--game', 'totk', '--check-only'])

    def test_without_tk_a_double_click_says_how_to_get_it(self):
        done = self.run_script(tk=False)
        self.assertEqual((done.returncode, self.recorded()), (1, None))
        self.assertIn('The window needs Tk: brew install python-tk@3.14', done.stdout)
        self.assertIn(f'"{self.python}" -m tools.builder --cli --game totk', done.stdout)

    def test_without_tk_arguments_still_reach_the_text_mode(self):        # audit point 1
        self.run_script('--cli', '--game', 'totk', tk=False)
        self.assertEqual(self.recorded()[1:], ['-m', 'tools.builder', '--cli', '--game', 'totk'])

    def test_an_explicit_python_that_cannot_run_is_an_error(self):        # audit point 3: no fallback
        for bad in (self.tmp / 'missing', self.tmp):                    # a missing file, then a folder
            with self.subTest(bad=bad):
                done = self.run_script(python=bad)
                self.assertEqual((done.returncode, self.recorded()), (1, None))
                self.assertIn(f'BUILDER_PYTHON is not executable: {bad}', done.stdout)

    def test_the_file_is_an_executable_bash_script(self):
        self.assertTrue(os.access(SCRIPT, os.X_OK))
        self.assertEqual(SCRIPT.read_text().splitlines()[0], '#!/bin/bash')


if __name__ == '__main__':
    unittest.main()
