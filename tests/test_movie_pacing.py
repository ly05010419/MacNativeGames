"""Every Zelda-line port's host movie decoder (games/<port>/runtime/movie_host.c) paces across a seek or a loop the game
makes without Flush: a pts going back, or jumping more than a second ahead, starts the pacing again
(shared/runtime/movie_pace.h). Mario Kart 8 Deluxe's Grand Prix waited 134 s for the engine-class clip at 144 s of its
preview, and a loop back dropped the frames after it as late, because each copy paced from the first frame only.

- tests/movie_pace_probe.c runs against each port's real movie_host.c (VP9 frames from libvpx, a fake 60 Hz clock): a
  forward seek and a loop, no Flush, every frame out one per game frame, none dropped;
- each port's movie_host.c includes movie_pace.h through runtime/movie_pace.h, a link to the shared one, and is either
  the shared shared/runtime/movie_host.c (through a link) or its own copy with the same rule.

The nxnative-runtime games (runtime/host and runtime/gfx: mk8d, aoc, jamboree, la, ss, worldend) have no movie_host.c
and are not covered."""
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHARED_PACE = ROOT / 'shared/runtime/movie_pace.h'
SHARED_HOST = ROOT / 'shared/runtime/movie_host.c'
PROBE = ROOT / 'tests/movie_pace_probe.c'
NXNATIVE = {'mk8d', 'aoc', 'jamboree', 'la', 'ss', 'worldend'}
FRAMEWORKS = ['VideoToolbox', 'CoreMedia', 'CoreVideo', 'CoreFoundation', 'AudioToolbox']
PACE_SWITCHES = ('TOTK_MOVIE_PACE', 'PSV_MOVIE_PACE')   # the ports' pace_on() switches (Pokémon Scarlet's own prefix)


def ports():
    """The Zelda-line ports with a host movie decoder."""
    found = sorted(p.parents[1].name for p in (ROOT / 'games').glob('*/runtime/movie_host.c'))
    return [g for g in found if g not in NXNATIVE]


def libvpx():
    out = subprocess.run([sys.executable, str(ROOT / 'tools/deps'), 'prefix', 'libvpx'], capture_output=True, text=True)
    prefix = Path(out.stdout.strip()) if out.returncode == 0 and out.stdout.strip() else None
    return prefix if prefix and (prefix / 'lib/libvpx.a').is_file() else None


def build_and_run(game, prefix, folder):
    """Build the probe against one port's movie_host.c and run it: (ok, message)."""
    runtime = ROOT / 'games' / game / 'runtime'
    exe = Path(folder) / f'movie_pace_{game}'
    cmd = ['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{runtime}', f'-I{prefix}/include']
    nvn_include = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn/include'   # mach_vm_compat.h (Dread)
    if nvn_include.is_dir():
        cmd.append(f'-I{nvn_include}')
    cmd += [str(PROBE), str(runtime / 'movie_host.c'), str(prefix / 'lib/libvpx.a')]
    for framework in FRAMEWORKS:
        cmd += ['-framework', framework]
    built = subprocess.run(cmd + ['-o', str(exe)], capture_output=True, text=True)
    if built.returncode != 0:
        return False, f'build failed:\n{built.stderr[-3000:]}'
    # Pacing on, also where it is off by default (Animal Crossing, Mario vs. Donkey Kong, Breath of the Wild).
    env = dict(os.environ, **{name: '1' for name in PACE_SWITCHES})
    try:
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=env)
    except subprocess.TimeoutExpired:
        return False, 'the probe did not finish in 120 s'
    lines = out.stdout.strip().splitlines()
    status = json.loads(lines[-1]).get('status') if lines else None
    return out.returncode == 0 and status == 'pass', out.stderr.strip() or out.stdout.strip()


class MoviePacingEveryPort(unittest.TestCase):
    def test_ports_found(self):
        games = ports()
        self.assertGreaterEqual(len(games), 20, games)
        self.assertIn('mk8dx', games)
        self.assertIn('totk', games)
        self.assertFalse(NXNATIVE.intersection(games))

    def test_every_port_includes_the_shared_rule(self):
        for game in ports():
            with self.subTest(game=game):
                runtime = ROOT / 'games' / game / 'runtime'
                link = runtime / 'movie_pace.h'
                self.assertTrue(link.is_symlink() and link.resolve() == SHARED_PACE.resolve(),
                                f'{game}: runtime/movie_pace.h is not a link to shared/runtime/movie_pace.h')
                host = runtime / 'movie_host.c'
                if host.is_symlink():
                    self.assertEqual(host.resolve(), SHARED_HOST.resolve(), f'{game}: movie_host.c links elsewhere')
                text = host.read_text()
                self.assertIn('#include "movie_pace.h"', text, game)
                self.assertIn('movie_pace_jump(m->last_pts, m->decoded[0].time)', text, game)
                self.assertIn('!movie_pace_jump(m->decoded[0].time, m->decoded[1].time)', text, game)
                # the probe turns pacing on through this switch (some ports have it off by default)
                self.assertTrue(any(f'getenv("{name}")' in text for name in PACE_SWITCHES), game)

    def test_shared_movie_host_has_only_links(self):
        """shared/runtime/movie_host.c is the one file of the ports whose copies were the same (TotK's, behind GAME_BOTW
        for Breath of the Wild); a port that keeps its own copy has no link to it."""
        self.assertTrue(SHARED_HOST.is_file() and not SHARED_HOST.is_symlink())
        linked = [g for g in ports() if (ROOT / 'games' / g / 'runtime/movie_host.c').is_symlink()]
        self.assertIn('totk', linked)
        for game in linked:
            target = os.readlink(ROOT / 'games' / game / 'runtime/movie_host.c')
            self.assertEqual(target, '../../../shared/runtime/movie_host.c', game)

    def test_seek_and_loop_without_flush_in_every_port(self):
        prefix = libvpx()
        if not prefix:
            self.skipTest('libvpx from tools/deps is not built on this Mac')
        games = ports()
        with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            results = dict(zip(games, pool.map(lambda g: build_and_run(g, prefix, tmp), games)))
        for game in games:
            with self.subTest(game=game):
                ok, message = results[game]
                self.assertTrue(ok, f'{game}: {message}')


if __name__ == '__main__':
    unittest.main()
