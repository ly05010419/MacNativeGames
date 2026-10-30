#!/usr/bin/env python3
"""Run the copied NVN-on-Metal regressions from a staged source tree (no game files): by default the ones for what this game needed, --all for the whole suite."""
import argparse
from pathlib import Path
import subprocess
from build_mk8d import stage, environment, GAME, TITLE

# t_cube_placeholder: a 2D array of fewer than six layers in a cube slot binds the zero cube (the race's slot 10, tex_fit); t_vbuf and t_geometry: vertex input and geometry basics
TESTS = ('t_cube_placeholder', 't_vbuf', 't_geometry')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-dir', required=True, type=Path)
parser.add_argument('--all', action='store_true', help='build and run every test of runtime/gfx/tests (make run)')
args = parser.parse_args()
work = args.work_dir.expanduser().resolve()
if GAME.parents[1] == work or GAME.parents[1] in work.parents:
    parser.error('use a directory outside the checkout')
source = stage(work, work / 'dependencies/nuget')
env = environment(work)
tests = source / 'dm/gfx/tests'
subprocess.run(['make', '-C', str(source / 'dm/host'), f'GAME={TITLE}', 'game_profile'], check=True, env=env)
if args.all:
    subprocess.run(['make', '-C', str(tests), f'GAME={TITLE}', 'run'], check=True, env=env)
else:
    subprocess.run(['make', '-C', str(tests), f'GAME={TITLE}', *TESTS], check=True, env=env)
    for test in TESTS:
        subprocess.run([str(tests / test)], check=True, cwd=tests, env=dict(env, MTL_DEBUG_LAYER='1'))
