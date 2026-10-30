#!/usr/bin/env python3
"""Run the native vertex-buffer and discard/feedback regressions (no game files)."""
import argparse
from pathlib import Path
import subprocess
from build_jamboree import stage, environment, GAME, TITLE

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-dir', required=True, type=Path)
args = parser.parse_args()
work = args.work_dir.expanduser().resolve()
if GAME.parents[1] == work or GAME.parents[1] in work.parents:
    parser.error('use a directory outside the checkout')
source = stage(work)
env = environment(work)
subprocess.run(['make', '-C', str(source / 'dm/host'), f'GAME={TITLE}', 'game_profile'], check=True, env=env)
subprocess.run(['make', '-C', str(source / 'dm/gfx/tests'), f'GAME={TITLE}', 't_vbuf', 't_geometry'], check=True, env=env)
for test in ('t_vbuf', 't_geometry'):
    subprocess.run([str(source / 'dm/gfx/tests' / test)], check=True, env=dict(env, MTL_DEBUG_LAYER='1'))
