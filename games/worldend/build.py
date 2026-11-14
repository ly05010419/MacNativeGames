#!/usr/bin/env python3
"""MNG build adapter for Worldend Syndrome 1.0.0."""
import argparse
from pathlib import Path
import sys

GAME = Path(__file__).resolve().parent
sys.path.insert(0, str(GAME/'scripts'))
sys.path.insert(0, str(GAME.parents[1]))
import build_app
from build_worldend import main as build, APP_NAME
from worldend_bundle import check_output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('-k', '--keys', type=Path, required=True)
    parser.add_argument('-o', '--output', type=Path)
    parser.add_argument('--work', type=Path)
    args = parser.parse_args(argv)
    original = args.game.expanduser().resolve()
    output = (args.output or original.parent/f'{APP_NAME}.app').expanduser().resolve()
    check_output(output)
    work, why = (args.work.expanduser().resolve(), 'given with --work') if args.work else build_app.default_work(output, 'worldend')
    repo = GAME.parents[1]
    if work == repo or repo in work.parents or work in repo.parents or output == work or output in work.parents:
        raise ValueError('work/output must stay outside the checkout and each other')
    print(f'work folder {work} ({why})', flush=True)
    if original.is_file():
        inputs = work/'original-input'
        inputs.mkdir(parents=True, exist_ok=True)
        for old in inputs.iterdir():
            if old.is_symlink(): old.unlink()
            else: raise ValueError(f'non-managed input: {old}')
        (inputs/original.name).symlink_to(original)
    elif original.is_dir():
        inputs = original
    else:
        raise ValueError(f'input does not exist: {original}')
    build([str(inputs), '--keys', str(args.keys.expanduser().resolve()), '--work-dir', str(work), '--output', str(output)])


if __name__ == '__main__':
    main()
