#!/usr/bin/env python3
"""MNG Launcher's build of Super Mario Odyssey: the launcher's arguments in, scripts/build_odyssey.py run, the app where
the launcher asked for it.

    python3 games/odyssey/build.py <game.xci|nsp> -k <prod.keys> [--update <update.nsp>]
                                   [--firmware <NCA folder> | --shared-fonts <fonts folder>] [--work <folder>] [-o <app>]

<game> is a combined card image or NSP with update 1.3.0 or 1.4.1 (a "1G+1U" card too), or the base NSP with --update.
build_odyssey.py does the work in its own steps, each skipped when its verified output is there; this adapter gives it
a Python of the work folder's (<work>/venv: capstone, cryptography, lz4), the fonts folder when one is given, and has
scripts/package_native_app.py write the app to -o and record the work folder in it (MNGWorkFolder).

The update an installed app gets without game files (update_runtime.py) is blocker(app, work) and update(app, work):
the loader tables checked against the pinned hashes again, the runtime compiled again and the app packaged again."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parent
REPO = GAME.parents[1]
APP_NAME = 'Super Mario Odyssey'       # tools/builder_jobs/repo.py GAMES['odyssey']
BUNDLE_ID = 'org.macnativegames.odyssey'   # scripts/package_native_app.py
PACKAGES = ('capstone==5.0.9', 'cryptography==50.0.2', 'lz4==4.4.5')   # the README's three, at the pins the other ports use
MODULES = ('main', 'sdk', 'subsdk0')   # scripts/build_odyssey.py MODULES
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))


def say(message):
    print(message, flush=True)


def python_for(work):
    venv = work / 'venv'
    python, marker = venv / 'bin/python', venv / '.packages'
    if not python.is_file() or not marker.is_file() or marker.read_text().split() != list(PACKAGES):
        base = shutil.which('python3.14') or sys.executable
        subprocess.run([base, '-m', 'venv', str(venv)], check=True)
        subprocess.run([str(python), '-m', 'pip', 'install', '-q', '--disable-pip-version-check', *PACKAGES], check=True)
        marker.write_text('\n'.join(PACKAGES) + '\n')
    return python


def in_venv(work):
    return Path(sys.prefix).resolve() == (work / 'venv').resolve()


def reexec(python, argv, *extra):
    if subprocess.run([str(python), str(Path(__file__).resolve()), *extra, *map(str, argv)]).returncode:
        raise ValueError('the build stopped (its message above says why)')


def checked_work(work, out):
    if work == REPO or REPO in work.parents or work in REPO.parents or work == out or out in work.parents:
        raise ValueError('the work folder must be outside the checkout and outside the app')
    if ' ' in os.path.relpath(work, GAME):
        raise ValueError(f'make cannot build into a folder whose path has a space: {work}')
    return work


def package_env(work, out):
    """scripts/package_native_app.py: where the app goes and what it records (build_app.record_update_source's keys)."""
    return {'ODYSSEY_WORK': str(work), 'ODYSSEY_APP': str(out), 'ODYSSEY_RECORD_WORK': str(work),
            'ODYSSEY_RECORD_REPO': str(REPO), 'ODYSSEY_RECORD_PYTHON': sys.executable}


def copy_fonts(given, work):
    """A shared-fonts folder of an earlier build: its six BFTTF files where build_odyssey.py looks (it then skips its
    font step); the runtime checks them against the hashes prepare_runtime.py binds."""
    files = sorted(given.glob('*.bfttf'))
    if len(files) != 6:
        raise ValueError(f'{given} is not a shared-fonts folder (six .bfttf files)')
    fonts = work / 'extracted/firmware/shared-fonts'
    shutil.rmtree(fonts, ignore_errors=True)
    fonts.mkdir(parents=True)
    for file in [*files, *([given / 'manifest.json'] if (given / 'manifest.json').is_file() else [])]:
        shutil.copy2(file, fonts / file.name)


def build(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path, help='a card image or NSP with update 1.3.0 or 1.4.1, or the base NSP with --update')
    p.add_argument('-k', '--keys', type=Path, required=True)
    p.add_argument('--update', type=Path, help='the 1.3.0 or 1.4.1 update NSP, when the game file is the base NSP alone')
    fonts = p.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder with the system fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='the shared-fonts folder of an earlier build')
    p.add_argument('--work', type=Path, help='work folder (default: the one the app at -o records, else MacNativeGames-work/odyssey)')
    p.add_argument('-o', '--out', '--output', dest='out', type=Path, help=f'the app to write (default: {APP_NAME}.app next to the game file)')
    args = p.parse_args(argv)
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    for path in (game, keys, *([args.update] if args.update else [])):
        if not Path(path).expanduser().is_file():
            raise ValueError(f'no such file: {path}')
    out = (args.out.expanduser() if args.out else game.parent / f'{APP_NAME}.app').resolve()
    if out.suffix != '.app':
        raise ValueError(f'the output must end in .app: {out}')
    if args.work:
        work, why = args.work.expanduser().resolve(), 'given with --work'
    else:
        import build_app
        work, why = build_app.default_work(out, 'odyssey')
    work = checked_work(work, out)
    say(f'work folder {work} ({why})')
    work.mkdir(parents=True, exist_ok=True)
    python = python_for(work)
    if not in_venv(work):
        raw = list(sys.argv[1:] if argv is None else argv)
        return reexec(python, [*raw, '--work', str(work), '-o', str(out)])
    if args.shared_fonts:
        copy_fonts(args.shared_fonts.expanduser().resolve(), work)
    cmd = [sys.executable, GAME / 'scripts/build_odyssey.py', '--game', game, '--keys', keys, '--work', work]
    if args.update:
        cmd += ['--update', args.update.expanduser().resolve()]
    if args.firmware:
        cmd += ['--firmware', args.firmware.expanduser().resolve()]
    done = subprocess.run([str(c) for c in cmd], cwd=GAME, env={**os.environ, **package_env(work, out)})
    if done.returncode:
        raise ValueError('build_odyssey.py stopped (its message above says why)')
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def blocker(app, work):
    work = Path(work)
    try:
        info = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())
    except (OSError, ValueError):
        return 'the app has no readable Info.plist: a full build is required'
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        return 'an Odyssey app made before MNG Launcher builds it: a full build is required once'
    needed = [(work / 'venv/bin/python', 'build Python'), (work / 'extracted/local/romfs/index.json', 'game data'),
              (work / 'extracted/extraction-manifest.json', 'program modules'),
              (work / 'app-assets/OdysseyIcon.jpg', 'app icon'),
              *((work / f'rewritten/{m}.patched.bin', 'rewritten code') for m in MODULES)]
    for path, what in needed:
        if not path.is_file():
            return f'the work folder has no {what}: a full build is required'
    if len(list((work / 'extracted/firmware/shared-fonts').glob('*.bfttf'))) < 6:
        return 'the work folder has no system fonts: a full build is required'
    return ''


def update(app, work):
    app, work = Path(app).resolve(), Path(work).resolve()
    checked_work(work, app)
    problem = blocker(app, work)
    if problem:
        raise ValueError(problem)
    python = python_for(work)
    if not in_venv(work):
        return reexec(python, [app, work], '--update-app')
    env = {**os.environ, **package_env(work, app)}
    make_work = os.path.relpath(work, GAME)
    steps = [('5/8 Loader tables (verifies every input against the pinned hashes)', [sys.executable, 'scripts/prepare_runtime.py', '--work', work], {}),
             ('6/8 Shader tools and the runtime', ['make', f'-j{os.cpu_count() or 8}', 'runtime', f'WORK={make_work}', f'PY={sys.executable}'],
              {'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer'} if Path('/Applications/Xcode.app').is_dir() else {}),
             ('8/8 Package the app', [sys.executable, 'scripts/package_native_app.py'], {})]
    for title, cmd, extra in steps:
        say(title)
        if subprocess.run([str(c) for c in cmd], cwd=GAME, env={**env, **extra}).returncode:
            raise ValueError(f'{title.split(" ", 1)[1]} failed')
    say(f'Built: {app}')


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv[:1] == ['--update-app']:
            update(Path(argv[1]), Path(argv[2]))
        else:
            build(argv)
    except (ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
