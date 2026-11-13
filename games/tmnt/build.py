#!/usr/bin/env python3
"""Build an experimental macOS app from the player's TMNT 1.0.0 base game."""
import argparse
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parent
sys.path.insert(0, str(GAME / 'scripts'))
from extract_exefs import validate_private_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='base game NSP or XCI')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--shared-fonts', type=Path, help='optional verified shared-font directory')
    parser.add_argument('--work', required=True, type=Path, help='fresh private work directory')
    parser.add_argument('--out', required=True, type=Path, help='fresh output .app')
    parser.add_argument('--dotnet', help='optional .NET 10 SDK executable')
    args = parser.parse_args()
    work, out = args.work.resolve(), args.out.resolve()
    for p in (work, out):
        validate_private_output(p)
    if work == out or work in out.parents or out in work.parents:
        parser.error('--out and --work must be separate')
    if not args.game.is_file() or not args.keys.is_file():
        parser.error('game file and keys file must exist')

    if args.shared_fonts and not (args.shared_fonts / 'manifest.json').is_file():
        parser.error('--shared-fonts requires a verified font manifest')

    def run(name, *arguments):
        subprocess.run([sys.executable, GAME / 'scripts' / name, *arguments], check=True)

    work.mkdir(parents=True)
    run('extract_exefs.py', args.game, '--keys', args.keys, '--output', work / 'exefs')
    run('extract_romfs.py', '--keys', args.keys, '--manifest', work / 'extraction-manifest.json',
        '--output', work / 'romfs')
    run('build_private_bundle.py', '--exefs', work / 'exefs', '--romfs', work / 'romfs',
        *(['--fonts', args.shared_fonts] if args.shared_fonts else []), '--work', work / 'build-work', '--out', out, '--app',
        *(['--dotnet', args.dotnet] if args.dotnet else []))


if __name__ == '__main__':
    main()
