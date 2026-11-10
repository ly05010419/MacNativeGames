"""Controllers and scripted input (runtime/host/tests/t_hid.c) on the real hle_hid.c: built here with clang and run, no game files.
The scripted part drives DM_KEYS and DM_CTL step by step (the test owns their sleeps): two presses always reach the game with a
released sample between them, a pending press or a sample taken while held never counts as that release, and a release the game
never reads is reported after 1 s."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
HOST = GAME/'runtime/host'
TITLE = '01002B00111A2000'


@unittest.skipUnless(sys.platform == 'darwin', 'macOS host code')
class HidInputTests(unittest.TestCase):
    def test_players_and_scripted_presses(self):
        with tempfile.TemporaryDirectory() as tmp:
            inc = Path(tmp)/'gen'; inc.mkdir()
            subprocess.run([sys.executable, str(GAME/'runtime/games/game.py'), 'header', TITLE, str(inc/'game_profile.h')], check=True, capture_output=True)
            out = Path(tmp)/'t_hid'
            subprocess.run(['clang', '-O1', '-Wall', '-Werror', '-Wno-unused-function', f'-I{HOST}', f'-I{inc}', f'-I{GAME}/runtime/games/{TITLE}',
                            '-o', str(out), str(HOST/'tests/t_hid.c'), str(HOST/'hle_haptics.c')], check=True, capture_output=True, text=True)
            r = subprocess.run([str(out)], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-2000:])
            self.assertIn(', 0 failed', r.stdout)
            self.assertIn('no sample without 0x1 reached the game within 1 s', r.stderr)


if __name__ == '__main__':
    unittest.main()
