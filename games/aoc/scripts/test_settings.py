#!/usr/bin/env python3
"""Build/run the AppKit settings tests without game data (requires a macOS GUI session)."""
import argparse
import os
from pathlib import Path
import subprocess
from build_aoc import GAME

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-dir', required=True, type=Path)
args = parser.parse_args()
work = args.work_dir.expanduser().resolve()
if work == GAME.parents[1] or GAME.parents[1] in work.parents:
    parser.error('use a private directory outside the checkout')
work.mkdir(parents=True, exist_ok=True)
binary = work / 'settings-preview'
subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-O1', '-fobjc-arc', '-Wall', '-Wextra',
                str(GAME / 'tests/settings_preview.m'), str(GAME / 'runtime/host/front_settings.m'),
                '-framework', 'Cocoa', '-framework', 'QuartzCore', '-o', str(binary)], check=True)
for language in ('zh-Hans', 'en-US'):
    subprocess.run([str(binary), '--self-test'], env=dict(os.environ, DM_LANG=language), check=True)
print('English settings controls and output sizing passed with both game language overrides.')
