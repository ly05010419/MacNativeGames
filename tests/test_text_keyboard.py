"""Mac controller text entry (runtime/text_keyboard.h), the settings panel with a controller, and Command-Q while they are open:
games/totk/tests/text_keyboard, settings_controller and command_q, which compile the real AppKit views and gamepad code. No game content."""
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parents[1] / 'games/totk/tests'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class TextKeyboardTests(unittest.TestCase):
    def run_script(self, script):
        run = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=300)
        self.assertEqual(run.returncode, 0, run.stdout[-3000:] + run.stderr[-3000:])

    def test_text_keyboard(self):
        self.run_script(TESTS / 'text_keyboard/test.py')

    def test_settings_controller(self):
        self.run_script(TESTS / 'settings_controller/test.py')

    def test_command_q(self):
        self.run_script(TESTS / 'command_q/test.py')


if __name__ == '__main__':
    unittest.main()
