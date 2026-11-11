"""The native-texture movie slots (runtime/host/tests/t_movie_slots.c) on the real hle_movie.c: built here with clang and run, no game files."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
HOST = GAME/'runtime/host'
TITLE = '01002DA013484000'


def build(tmp, source=HOST/'hle_movie.c'):
    """The test binary; source is the decoder file the test includes (a copy for the unmutated control)."""
    inc = Path(tmp)/'gen'; inc.mkdir()
    subprocess.run([sys.executable, str(GAME/'runtime/games/game.py'), 'header', TITLE, str(inc/'game_profile.h')], check=True, capture_output=True)
    test = HOST/'tests/t_movie_slots.c'
    if source != HOST/'hle_movie.c':   # the copy sits where the test's relative includes find it
        work = Path(tmp)/'host'; (work/'tests').mkdir(parents=True)
        for p in HOST.iterdir():
            if p.is_file(): (work/p.name).symlink_to(p)
        (work/'hle_movie.c').unlink(); (work/'hle_movie.c').write_bytes(source.read_bytes())
        (work/'tests/t_movie_slots.c').symlink_to(test); test = work/'tests/t_movie_slots.c'
    out = Path(tmp)/'t_movie_slots'
    subprocess.run(['clang', '-O1', '-g', '-Wno-unused-function', '-Wno-tautological-compare', f'-I{inc}', f'-I{GAME}/runtime/games/{TITLE}',
                    f'-I{GAME}/runtime/third_party/libvpx', f'-I{GAME}/runtime/third_party/libvpx/config', f'-I{GAME}/runtime/third_party/stb_vorbis',
                    '-o', str(out), str(test), '-framework', 'AudioToolbox'], check=True, capture_output=True, text=True)
    return out


@unittest.skipUnless(sys.platform == 'darwin', 'macOS host code')
class MovieSlotTests(unittest.TestCase):
    def test_every_case_on_the_real_decoder(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([str(build(tmp))], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-2000:])
            for line in ('reused slot', 'explicit release', 'never released', 'flush and stop'):
                self.assertIn(line, r.stdout)


if __name__ == '__main__':
    unittest.main()
