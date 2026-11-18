"""MNG Launcher's adapter, the part every port's games/<game>/build.py shares (tools/port_app/__init__.py).

The launcher (tools/builder_jobs/build_job.py) runs

    python3 games/<game>/build.py <base> -k <keys> [--update U] [--dlc D ...] [--firmware F | --shared-fonts S] --work W -o OUT

and update_runtime.py imports the same file to call blocker(app, work) and update(app, work), which runs
`build.py --update-app <app> <work>` in the work folder's Python. A build.py keeps its game name, bundle ID, packages,
steps and blocker's file list, and uses these for the rest. Every function takes what differs as an argument: nothing
here knows a game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
XCODE = Path('/Applications/Xcode.app/Contents/Developer')
STOPPED = 'the build stopped (its ERROR line above says why)'
FULL_BUILD = 'a full build is required'


def say(message):
    print(message, flush=True)


def stage(number, total, words):
    say(f'{number}/{total} {words}')   # tools/builder/progress.py reads the 'i/N' lines as the build's stages


def run_logged(cmd, work, name, env=None, cwd=REPO):
    """One step, its output in <work>/logs/<name>.log (MNG Launcher shows the logs a build starts as its steps)."""
    log = Path(work) / 'logs' / f'{name}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        result = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=out, stderr=subprocess.STDOUT,
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', **(env or {})})
    if result.returncode:
        tail = log.read_text(errors='replace').strip().splitlines()[-12:]
        raise ValueError(f'{name} failed (full output: {log})\n' + '\n'.join(tail))


def stamp(path):
    """A file's identity for reuse checks: its real path, size and modification time."""
    st = Path(path).stat()
    return [str(Path(path).resolve()), st.st_size, st.st_mtime_ns]


# --- the work folder's Python -------------------------------------------------------------------------------------

def python_for(work, *, packages=None, requirements=None):
    """<work>/venv, made once per work folder and again when what it should hold changed: the pinned `packages`
    (recorded in venv/.packages; pip's output shown), or a `requirements` file (its SHA-256 in venv/.requirements.sha256;
    the output in <work>/logs/venv.log and pip.log)."""
    if (packages is None) == (requirements is None):
        raise TypeError('python_for: give packages or requirements')
    venv = Path(work) / 'venv'
    python = venv / 'bin/python'
    base = shutil.which('python3.14') or sys.executable
    if packages is not None:
        marker = venv / '.packages'
        if not python.is_file() or not marker.is_file() or marker.read_text().split() != list(packages):
            subprocess.run([base, '-m', 'venv', str(venv)], check=True)
            subprocess.run([str(python), '-m', 'pip', 'install', '-q', '--disable-pip-version-check', *packages], check=True)
            marker.write_text('\n'.join(packages) + '\n')
        return python
    marker = venv / '.requirements.sha256'
    wanted = hashlib.sha256(Path(requirements).read_bytes()).hexdigest()
    if not python.is_file() or not marker.is_file() or marker.read_text().strip() != wanted:
        run_logged([base, '-m', 'venv', venv], work, 'venv')
        run_logged([python, '-m', 'pip', 'install', '-q', '-r', requirements], work, 'pip')
        marker.write_text(wanted + '\n')
    return python


def in_venv(work):
    return Path(sys.prefix).resolve() == (Path(work) / 'venv').resolve()


def reexec(script, python, argv, *extra, stopped=STOPPED):
    """build.py again in the work folder's Python (its tools need the venv's packages)."""
    if subprocess.run([str(python), str(Path(script).resolve()), *extra, *map(str, argv)]).returncode:
        raise ValueError(stopped)


def checked_work(work, out, repo=REPO):
    if work == repo or repo in work.parents or work in repo.parents or work == out or out in work.parents:
        raise ValueError('the work folder must be outside the checkout and outside the app')
    return work


# --- the launcher's arguments ---------------------------------------------------------------------------------------

def parser(doc):
    return argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)


def add_inputs(p, *, game_help, update_help, update_required, keys_help=None):
    p.add_argument('game', type=Path, help=game_help)
    p.add_argument('-k', '--keys', type=Path, required=True, help=keys_help)
    p.add_argument('--update', type=Path, required=update_required, help=update_help)


def add_fonts(p):
    fonts = p.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder with the system fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='the shared-fonts folder of an earlier build')


def add_output(p, app_name, game):
    p.add_argument('--work', type=Path, help=f'work folder (default: the one the app at -o records, else MacNativeGames-work/{game})')
    p.add_argument('-o', '--out', '--output', dest='out', type=Path, help=f'the app to write (default: {app_name}.app next to the game file)')


def given(path):
    """An optional path argument, expanded and resolved (None stays None)."""
    return path.expanduser().resolve() if path else None


def inputs(args):
    """The game file, the keys and the update (None when not given), resolved; each must exist."""
    game, keys, update = given(args.game), given(args.keys), given(args.update)
    for path in (game, keys, *([update] if update else [])):
        if not path.is_file():
            raise ValueError(f'no such file: {path}')
    return game, keys, update


def output_and_work(args, game_file, app_name, game, check=checked_work):
    """The app to write and the work folder: --work, else build_app.default_work (the one the app records, else
    MacNativeGames-work/<game>), checked, said and made."""
    out = (args.out.expanduser() if args.out else game_file.parent / f'{app_name}.app').resolve()
    if out.suffix != '.app':
        raise ValueError(f'the output must end in .app: {out}')
    if args.work:
        work, why = args.work.expanduser().resolve(), 'given with --work'
    else:
        if str(REPO) not in sys.path:
            sys.path.insert(0, str(REPO))
        import build_app
        work, why = build_app.default_work(out, game)
    work = check(work, out)
    say(f'work folder {work} ({why})')
    work.mkdir(parents=True, exist_ok=True)
    return out, work


def rerun_in_venv(script, python, work, argv, out, stopped=STOPPED):
    """False inside the work folder's Python; else build.py run there with the work folder and app named, and True."""
    if in_venv(work):
        return False
    raw = list(sys.argv[1:] if argv is None else argv)
    reexec(script, python, [*raw, '--work', str(work), '-o', str(out)], stopped=stopped)
    return True


# --- the express update (update_runtime.py) -----------------------------------------------------------------------

def app_info(app):
    """The app's Info.plist, or None when it has none that can be read."""
    try:
        return plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())
    except (OSError, ValueError):
        return None


def first_missing(needed):
    """The blocker line for the first (path, what) whose file is missing, '' when every one is there."""
    for path, what in needed:
        if not Path(path).is_file():
            return f'the work folder has no {what}: {FULL_BUILD}'
    return ''


def update_in_venv(script, app, work, *, blocker, python, check=checked_work, stopped=STOPPED):
    """The start of every update(app, work): both resolved, the work folder checked, the blocker asked, the work folder's
    Python made. Inside that Python: (app, work, that Python); outside it: build.py --update-app run there, and None."""
    app, work = Path(app).resolve(), Path(work).resolve()
    check(work, app)
    problem = blocker(app, work)
    if problem:
        raise ValueError(problem)
    interpreter = python(work)
    if not in_venv(work):
        reexec(script, interpreter, [app, work], '--update-app', stopped=stopped)
        return None
    return app, work, interpreter


def main(build, update, argv=None):
    """build.py's entry: `--update-app <app> <work>` (update_runtime.py's update in the work folder's Python), else the
    launcher's build. A ValueError is the ERROR line MNG Launcher shows."""
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


# --- fonts and Xcode ------------------------------------------------------------------------------------------------

def fonts_ready(folder, row_keys=()):
    """A verified shared-fonts folder: manifest.json passed, its six fonts in type order, each file there (and each row
    holding row_keys)."""
    try:
        manifest = json.loads((Path(folder) / 'manifest.json').read_text())
        return manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6)) and all(
            (Path(folder) / f'{f["type"]}-{f["name"]}.bfttf').is_file() and all(k in f for k in row_keys)
            for f in manifest['fonts'])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def xcode_env():
    """Xcode's developer folder for make, when Xcode is installed (whatever xcode-select names)."""
    return {'DEVELOPER_DIR': str(XCODE)} if XCODE.parent.parent.is_dir() else {}


def metal_env():
    """The Metal compiler for the shader libraries: the selected developer folder's, else Xcode's."""
    if subprocess.run(['xcrun', '-f', 'metal'], capture_output=True).returncode == 0:
        return {}
    if (XCODE / 'usr/bin').is_dir() and subprocess.run(['xcrun', '-f', 'metal'], capture_output=True,
                                                       env={**os.environ, 'DEVELOPER_DIR': str(XCODE)}).returncode == 0:
        return {'DEVELOPER_DIR': str(XCODE)}
    raise ValueError('the shaders are compiled with the Metal compiler, which is not installed: install Xcode, then '
                     'xcodebuild -downloadComponent MetalToolchain')
