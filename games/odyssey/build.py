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
the loader tables checked against the pinned hashes again, the runtime compiled again and the app packaged again.
What every port's build.py does the same way (the venv, the run inside it, the arguments, the work folder, the update's
entry) is tools/port_app/adapter.py; this file keeps what is Odyssey's."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parent
REPO = GAME.parents[1]
APP_NAME = 'Super Mario Odyssey'       # tools/builder_jobs/repo.py GAMES['odyssey']
BUNDLE_ID = 'org.macnativegames.odyssey'   # scripts/package_native_app.py
PACKAGES = ('capstone==5.0.9', 'cryptography==50.0.2', 'lz4==4.4.5')   # the README's three, at the pins the other ports use
MODULES = ('main', 'sdk', 'subsdk0')   # scripts/build_odyssey.py MODULES
STOPPED = 'the build stopped (its message above says why)'
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from tools.port_app import adapter  # noqa: E402  MNG Launcher's side every port's build.py shares
from tools.port_app.adapter import say  # noqa: E402


def python_for(work):
    return adapter.python_for(work, packages=PACKAGES)


def checked_work(work, out):
    adapter.checked_work(work, out, REPO)
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
    p = adapter.parser(__doc__)
    adapter.add_inputs(p, game_help='a card image or NSP with update 1.3.0 or 1.4.1, or the base NSP with --update',
                       update_help='the 1.3.0 or 1.4.1 update NSP, when the game file is the base NSP alone',
                       update_required=False)
    adapter.add_fonts(p)
    adapter.add_output(p, APP_NAME, 'odyssey')
    args = p.parse_args(argv)
    game, keys, update = adapter.inputs(args)
    out, work = adapter.output_and_work(args, game, APP_NAME, 'odyssey', checked_work)
    python = python_for(work)
    if adapter.rerun_in_venv(__file__, python, work, argv, out, STOPPED):
        return None
    if args.shared_fonts:
        copy_fonts(args.shared_fonts.expanduser().resolve(), work)
    cmd = [sys.executable, GAME / 'scripts/build_odyssey.py', '--game', game, '--keys', keys, '--work', work]
    if update:
        cmd += ['--update', update]
    if args.firmware:
        cmd += ['--firmware', args.firmware.expanduser().resolve()]
    done = subprocess.run([str(c) for c in cmd], cwd=GAME, env={**os.environ, **package_env(work, out)})
    if done.returncode:
        raise ValueError('build_odyssey.py stopped (its message above says why)')
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def blocker(app, work):
    work = Path(work)
    info = adapter.app_info(app)
    if info is None:
        return 'the app has no readable Info.plist: a full build is required'
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        return 'an Odyssey app made before MNG Launcher builds it: a full build is required once'
    needed = [(work / 'venv/bin/python', 'build Python'), (work / 'extracted/local/romfs/index.json', 'game data'),
              (work / 'extracted/extraction-manifest.json', 'program modules'),
              (work / 'app-assets/OdysseyIcon.jpg', 'app icon'),
              *((work / f'rewritten/{m}.patched.bin', 'rewritten code') for m in MODULES)]
    missing = adapter.first_missing(needed)
    if missing:
        return missing
    if len(list((work / 'extracted/firmware/shared-fonts').glob('*.bfttf'))) < 6:
        return 'the work folder has no system fonts: a full build is required'
    return ''


def update(app, work):
    ready = adapter.update_in_venv(__file__, app, work, blocker=blocker, python=python_for, check=checked_work,
                                   stopped=STOPPED)
    if not ready:
        return None
    app, work, _ = ready
    env = {**os.environ, **package_env(work, app)}
    make_work = os.path.relpath(work, GAME)
    steps = [(5, 'Loader tables (verifies every input against the pinned hashes)', [sys.executable, 'scripts/prepare_runtime.py', '--work', work], {}),
             (6, 'Shader tools and the runtime', ['make', f'-j{os.cpu_count() or 8}', 'runtime', f'WORK={make_work}', f'PY={sys.executable}'],
              adapter.xcode_env()),
             (8, 'Package the app', [sys.executable, 'scripts/package_native_app.py'], {})]
    for number, words, cmd, extra in steps:
        adapter.stage(number, 8, words)   # build_odyssey.py's numbering: the update runs its steps 5, 6 and 8
        if subprocess.run([str(c) for c in cmd], cwd=GAME, env={**env, **extra}).returncode:
            raise ValueError(f'{words} failed')
    say(f'Built: {app}')
    return None


def main(argv=None):
    return adapter.main(build, update, argv)


if __name__ == '__main__':
    raise SystemExit(main())
