#!/usr/bin/env python3
"""Build an experimental macOS app from the player's TMNT 1.0.0 base game."""
import argparse
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parent
sys.path.insert(0, str(GAME / 'scripts'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='base game NSP or XCI')
    parser.add_argument('-k', '--keys', required=True, type=Path)
    parser.add_argument('--shared-fonts', type=Path, help='optional verified shared-font directory')
    parser.add_argument('--work', type=Path, help='private work directory; existing contents are retained')
    parser.add_argument('-o', '--out', required=True, type=Path, help='output .app; an existing TMNT app is updated in place')
    args = parser.parse_args()
    sys.path.insert(0, str(GAME.parents[1]))
    from build_app import default_work
    from tmnt_bundle import check_output
    out = args.out.resolve()
    work = args.work.resolve() if args.work else default_work(out, 'tmnt')[0]
    check_output(out)
    if work == GAME.parents[1] or work.is_relative_to(GAME.parents[1]):
        parser.error('work must stay outside the source checkout')
    if work == out or work in out.parents or out in work.parents:
        parser.error('--out and --work must be separate')
    if not args.game.is_file() or not args.keys.is_file():
        parser.error('game file and keys file must exist')

    if args.shared_fonts and not (args.shared_fonts / 'manifest.json').is_file():
        parser.error('--shared-fonts requires a verified font manifest')

    def run(name, *arguments):
        subprocess.run([sys.executable, GAME / 'scripts' / name, *arguments], check=True)

    from tmnt_work import Steps, digest, adopt_romfs, build_lock, protect_inputs
    run_work = work / 'full'
    if run_work.is_symlink():
        raise ValueError('fixed build directory must not be a symlink')
    protect_inputs([args.game, args.keys, args.shared_fonts], [run_work, out])
    with build_lock(run_work):
        steps = Steps(run_work)
        # Hash the supplied container, not just its name or timestamp.
        identity = {'source': str(args.game.resolve()), 'sha256': digest(args.game),
                    'extractors': {name: digest(GAME / 'scripts' / name) for name in
                                   ('extract_exefs.py', 'extract_romfs.py', 'expected_inputs.py')},
                    'expected': digest(GAME / 'assets/expected-base-1.0.0.json')}
        steps.run('exefs', identity, [run_work / 'exefs', run_work / 'extraction-manifest.json'],
                  lambda: run('extract_exefs.py', args.game, '--keys', args.keys, '--output', run_work / 'exefs'))
        def romfs():
            if not adopt_romfs(out, run_work / 'romfs'):
                run('extract_romfs.py', '--keys', args.keys, '--manifest', run_work / 'extraction-manifest.json',
                    '--output', run_work / 'romfs')
        steps.run('romfs', identity, [run_work / 'romfs'], romfs)
        run('build_private_bundle.py', '--exefs', run_work / 'exefs', '--romfs', run_work / 'romfs',
            *(['--fonts', args.shared_fonts] if args.shared_fonts else []), '--work', run_work / 'build-work', '--out', out, '--app', '--record-work', work)



if __name__ == '__main__':
    main()
