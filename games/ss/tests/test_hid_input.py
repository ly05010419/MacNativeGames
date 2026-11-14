"""Controllers and scripted input (runtime/host/tests/t_hid.c) on the real hle_hid.c: built here with clang and run, no game files.
The test file is nxnative b1dee89's (players, rumble, motion, scripted input through its own path script_set, DM_REC) with MNG's AoC timing stubs;
LA's wait for a released sample between scripted presses is gone with LA's copy of the scripted path."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
HOST = GAME/'runtime/host'
TITLE = '01002DA013484000'


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


if __name__ == '__main__':
    unittest.main()
