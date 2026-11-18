#!/usr/bin/env python3
"""MNG build-engine adapter for Skyward Sword HD 1.0.1 (Mac)."""
import argparse
from pathlib import Path
import subprocess
import sys

GAME = Path(__file__).resolve().parent
sys.path.insert(0, str(GAME/'scripts'))
sys.path.insert(0, str(GAME.parents[1]))
import build_app
from build_ss import main as build_extracted
from package_ss import APP_NAME
from ss_bundle import adopt_inputs, check_output


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('-k', '--keys', type=Path)
    p.add_argument('--update', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('-o', '--output', type=Path)
    args = p.parse_args(argv)
    original = args.game.expanduser().resolve()
    if not original.exists():
        raise ValueError(f'no such input: {original}')
    output = (args.output or original.parent/f'{APP_NAME}.app').expanduser().resolve()
    check_output(output)
    work, why = (args.work.expanduser().resolve(), 'given with --work') if args.work else build_app.default_work(output, 'ss')
    print(f'work folder {work} ({why})', flush=True)
    repo = GAME.parents[1]
    if work == repo or repo in work.parents or work in repo.parents or output == work or output in work.parents:
        raise ValueError('work/output must stay outside the checkout and each other')
    from ss_python import build_python
    python = build_python(work)
    if Path(sys.prefix).resolve() != (work/'venv').resolve():
        subprocess.run([str(python), str(__file__), *(argv if argv is not None else sys.argv[1:])], check=True)
        return
    if original.is_dir():
        extracted = original
    else:
        extracted = work/'inputs'
        if not extracted.exists() and not adopt_inputs(output, extracted):
            if not args.keys or not args.update:
                raise ValueError('Skyward Sword HD requires the base, update 1.0.1 and prod.keys')
            from tools.deps import package
            libhac = package('libhac')
            import time
            subprocess.run([sys.executable, str(GAME/'scripts/extract_ss.py'), '--base', str(original),
                            '--update', str(args.update.expanduser().resolve()), '--keys', str(args.keys.expanduser().resolve()),
                            '--libhac', str(libhac), '--work-dir', str(work), '--output', str(extracted),
                            '--evidence', str(work/f'extraction-{time.time_ns()}')], check=True)
    build_extracted([str(extracted), '--work-dir', str(work), '--output', str(output)])


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, build_app.BuildError) as error:
        sys.exit(f'ERROR: {error}')
