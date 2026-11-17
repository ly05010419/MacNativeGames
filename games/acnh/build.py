#!/usr/bin/env python3
"""Animal Crossing: New Horizons in one command: the base game and a supported update in, a Mac app out.

    python3 games/acnh/build.py <base.nsp|xci> -k <prod.keys> --update <update.nsp> [--dlc <dlc.nsp> ...]
                                [--firmware <NCA folder> | --shared-fonts <fonts folder>] [--work <folder>] [-o <app>]

It runs the port's steps in order, each logging to <work>/logs/<step>.log, and reuses what an earlier run in the same
work folder made from the same inputs:
  1/7 tools        the build's Python (<work>/venv, requirements.txt) and the pinned hactool
  2/7 game data    prepare.py: verified ExeFS, rewritten modules, merged RomFS (<work>/prepared)
  3/7 fonts        the six system fonts: from --shared-fonts, --firmware, or a card image's update partition
  4/7 add-ons      prepare_dlc.py for the pinned DLC given with --dlc (3.0.1 and 3.0.3)
  5/7 frame rates  the pinned tmouh/acnh-60fps checkout (60 FPS; 120 FPS too on 3.0.3), fetched once into the work
                   folder unless --fps-mod names one or --no-fps-mod leaves it out
  6/7 host         build_host.py, the app icon and the shader seed (<work>/native)
  7/7 app          package_native_app.py; the app records this work folder (MNGWorkFolder)

The same file is MNG Launcher's update for an installed app (update_runtime.py): blocker(app, work) says why only a full
build fits, update(app, work) rebuilds the host and the app from the work folder's prepared files, without game files."""
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
APP_NAME = 'Animal Crossing New Horizons'   # tools/builder_jobs/repo.py GAMES['acnh']
BUNDLE_ID = 'org.macnativegames.acnh'       # scripts/package_native_app.py
FPS_SOURCE_URL = 'https://github.com/tmouh/acnh-60fps'
STEPS = 7
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))


def say(message):
    print(message, flush=True)


def stage(number, words):
    say(f'{number}/{STEPS} {words}')   # tools/builder/progress.py reads the 'i/N' lines as the build's stages


def run(cmd, work, name, env=None, cwd=REPO):
    """One step, its output in <work>/logs/<name>.log (MNG Launcher shows the logs a build starts as its steps)."""
    log = work / 'logs' / f'{name}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        result = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=out, stderr=subprocess.STDOUT,
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', **(env or {})})
    if result.returncode:
        tail = log.read_text(errors='replace').strip().splitlines()[-12:]
        raise ValueError(f'{name} failed (full output: {log})\n' + '\n'.join(tail))


def build_env():
    # The app and its runtime target the Mac they are built on (README: 27.0 on the maintainer's Mac).
    return {'MACOSX_DEPLOYMENT_TARGET': os.environ.get('MACOSX_DEPLOYMENT_TARGET', '26.0')}


def stamp(path):
    st = Path(path).stat()
    return [str(Path(path).resolve()), st.st_size, st.st_mtime_ns]


def python_for(work):
    """<work>/venv with requirements.txt, made once per work folder."""
    venv = work / 'venv'
    python = venv / 'bin/python'
    marker = venv / '.requirements.sha256'
    wanted = hashlib.sha256((GAME / 'requirements.txt').read_bytes()).hexdigest()
    if not python.is_file() or not marker.is_file() or marker.read_text().strip() != wanted:
        base = shutil.which('python3.14') or sys.executable
        run([base, '-m', 'venv', venv], work, 'venv')
        run([python, '-m', 'pip', 'install', '-q', '-r', GAME / 'requirements.txt'], work, 'pip')
        marker.write_text(wanted + '\n')
    return python


def hactool_for(work, python):
    tool = work / 'tools/hactool/hactool'
    if not (tool.is_file() and os.access(tool, os.X_OK)):
        run([python, GAME / 'tools/build_hactool.py', '--work', work], work, 'hactool')
    return tool


def prepare_game(work, python, base, update, keys, hactool):
    """prepare.py into <work>/prepared, reused while the inputs are the same files."""
    prepared, record = work / 'prepared', work / 'prepared-inputs.json'
    inputs = {'base': stamp(base), 'update': stamp(update), 'keys': hashlib.sha256(keys.read_bytes()).hexdigest()}
    try:
        done = json.loads((prepared / 'preparation.json').read_text()).get('status') == 'prepared_for_host_bringup'
        same = json.loads(record.read_text()) == inputs
    except (OSError, ValueError):
        done = same = False
    if done and same:
        say('game data: reusing the verified files of the last build')
        return prepared
    if prepared.exists():
        shutil.rmtree(prepared)
    run([python, GAME / 'prepare.py', base, '--update', update, '--keys', keys, '--hactool', hactool, '--work', prepared],
        work, 'prepare')
    record.write_text(json.dumps(inputs, indent=2) + '\n')
    return prepared


def fonts_ready(folder):
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6)) and all(
            (folder / f'{f["type"]}-{f["name"]}.bfttf').is_file() and 'bfttf_bytes' in f and 'font_bytes' in f
            for f in manifest['fonts'])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def copy_fonts(given, fonts):
    """A shared-fonts folder of an earlier build (this port's, or build_app.py's): its six BFTTF files, with this port's
    manifest written from their bytes (build_host.py checks each file's SHA-256 against it)."""
    from games.acnh.tools.extract_shared_fonts import FONTS, bfttf_size
    rows = []
    fonts.mkdir(parents=True, exist_ok=True)
    for kind, name, title, file in FONTS:
        source = given / f'{kind}-{name}.bfttf'
        if not source.is_file():
            raise ValueError(f'{given} is not a shared-fonts folder: {source.name} is missing')
        data = source.read_bytes()
        size = bfttf_size(data)
        (fonts / source.name).write_bytes(data)
        rows.append(dict(type=kind, name=name, title_id=title, file=file, bfttf_bytes=len(data), font_bytes=size,
                         sha256=hashlib.sha256(data).hexdigest()))
    (fonts / 'manifest.json').write_text(json.dumps(dict(status='pass', integrity=f'copied from {given}; BFTTF headers decode',
                                                         fonts=rows), indent=2) + '\n')


def prepare_fonts(work, python, keys, base, firmware=None, shared=None):
    fonts = work / 'fonts'
    if shared:
        if fonts.exists():
            shutil.rmtree(fonts)
        if fonts_ready(shared):
            shutil.copytree(shared, fonts)
        else:
            copy_fonts(shared, fonts)
    elif firmware or not fonts_ready(fonts):
        if fonts.exists():
            shutil.rmtree(fonts)
        source = ['--firmware', firmware] if firmware else ['--xci', base] if base.suffix.lower() == '.xci' else None
        if not source:
            raise ValueError('an NSP holds no system fonts: give --firmware <unpacked firmware NCA folder> or '
                             '--shared-fonts <the shared-fonts folder of an earlier build>')
        run([python, GAME / 'tools/extract_shared_fonts.py', '--keys', keys, *source, '--output', fonts], work, 'fonts')
    else:
        say('fonts: reusing the verified fonts of the last build')
    if not fonts_ready(fonts):
        raise ValueError(f'the system fonts in {fonts} are incomplete')
    return fonts


def prepare_dlc(work, python, keys, dlcs, version):
    if not dlcs:
        return None
    if version not in (2097152, 2228224):
        raise ValueError('the add-ons are supported with the 3.0.1 and 3.0.3 updates only')
    folder, record = work / 'dlc', work / 'dlc-inputs.json'
    inputs = sorted(stamp(d) for d in dlcs)
    try:
        if json.loads(record.read_text()) == inputs and (folder / 'manifest.json').is_file():
            say('add-ons: reusing the verified add-ons of the last build')
            return folder
    except (OSError, ValueError):
        pass
    if folder.exists():
        shutil.rmtree(folder)
    run([python, GAME / 'tools/prepare_dlc.py', *[a for d in dlcs for a in ('--source', d)], '--keys', keys,
         '--output', folder], work, 'dlc')
    record.write_text(json.dumps(inputs) + '\n')
    return folder


def fps_source(work, version, given=None):
    """The tmouh/acnh-60fps checkout at the commit pinned for this version (tools/prepare_fps.py PROFILES; prepare_fps
    checks every file's SHA-256), or None for a version without one. Fetched into the work folder once."""
    from games.acnh.tools.prepare_fps import PROFILES
    row = PROFILES.get(version)
    if not row:
        return None
    if given:
        return given
    folder = work / 'acnh-60fps-source'
    head = subprocess.run(['git', '-C', folder, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if head.returncode == 0 and head.stdout.strip() == row['commit']:
        return folder
    if not folder.is_dir():
        run(['git', 'clone', '--quiet', FPS_SOURCE_URL, folder], work, 'fps-mod')
    run(['git', '-C', folder, 'fetch', '--quiet', 'origin'], work, 'fps-mod-fetch')
    run(['git', '-C', folder, 'checkout', '--quiet', '--detach', row['commit']], work, 'fps-mod-checkout')
    return folder


def build_host(work, python, prepared, fonts, fps=None, dlc=None, keep_inputs=False):
    """build_host.py, the icon and the shader seed into <work>/native. keep_inputs: an update, which reuses the
    frame-rate and add-on inputs the native work folder remembers."""
    native = work / 'native'
    try:
        info = json.loads((native / 'host-info.json').read_text())
        if Path(info['prepared']) != prepared.resolve():
            shutil.rmtree(native)   # made from other game data: build_host.py refuses to mix them
    except (OSError, ValueError, KeyError):
        pass
    args = [python, GAME / 'build_host.py', '--prepared', prepared, '--work', native, '--shared-fonts', fonts]
    if not keep_inputs:
        args += ['--fps-mod', fps] if fps else ['--no-fps-mod']
        args += ['--dlc', dlc] if dlc else ['--no-dlc']
    run(args, work, 'host', build_env())
    return native


def finish_app(work, python, prepared, native, fonts, update_package, keys, out):
    if update_package and keys:
        run([python, GAME / 'tools/extract_app_icon.py', update_package, '--keys', keys, '--prepared', prepared,
             '--output', native / 'build/AppIcon.icns'], work, 'icon')
    run([python, GAME / 'tools/prepare_shader_seed.py', '--prepared', prepared,
         '--translator', native / 'build/shader-service/totk_xlate', '--output', native / 'build/shader-seed'],
        work, 'shader-seed')
    stage(7, 'Packaging the app')
    run([python, GAME / 'scripts/package_native_app.py', '--prepared', prepared, '--work', native,
         '--shared-fonts', fonts, '--output', out, '--record-work', work], work, 'package', build_env())
    say(f'Built: {out}')


def in_venv(work):
    return Path(sys.prefix).resolve() == (work / 'venv').resolve()


def reexec(python, argv, *extra):
    result = subprocess.run([str(python), str(Path(__file__).resolve()), *extra, *map(str, argv)])
    if result.returncode:
        raise ValueError('the build stopped (its ERROR line above says why)')


def default_work(out):
    import build_app
    return build_app.default_work(out, 'acnh')


def checked_work(work, out):
    if work == REPO or REPO in work.parents or work in REPO.parents or work == out or out in work.parents:
        raise ValueError('the work folder must be outside the checkout and outside the app')
    return work


def build(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path, help='the base game 1.0.0 (NSP or XCI)')
    p.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    p.add_argument('--update', type=Path, required=True, help='the 1.2.1, 3.0.1 or 3.0.3 update NSP')
    p.add_argument('--dlc', type=Path, action='append', help='a pinned add-on NSP (3.0.1, 3.0.3); repeat for each')
    fonts = p.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder with the system fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='the shared-fonts folder of an earlier build')
    fps = p.add_mutually_exclusive_group()
    fps.add_argument('--fps-mod', type=Path, help='a local tmouh/acnh-60fps checkout at the pinned commit')
    fps.add_argument('--no-fps-mod', action='store_true', help='original 30 FPS only; nothing is fetched')
    p.add_argument('--work', type=Path, help='work folder (default: the one the app at -o records, else MacNativeGames-work/acnh)')
    p.add_argument('-o', '--out', '--output', dest='out', type=Path, help=f'the app to write (default: {APP_NAME}.app next to the game file)')
    args = p.parse_args(argv)

    base, keys, update = (path.expanduser().resolve() for path in (args.game, args.keys, args.update))
    for path in (base, keys, update):
        if not path.is_file():
            raise ValueError(f'no such file: {path}')
    out = (args.out.expanduser() if args.out else base.parent / f'{APP_NAME}.app').resolve()
    if out.suffix != '.app':
        raise ValueError(f'the output must end in .app: {out}')
    work, why = (args.work.expanduser().resolve(), 'given with --work') if args.work else default_work(out)
    work = checked_work(work, out)
    say(f'work folder {work} ({why})')
    work.mkdir(parents=True, exist_ok=True)

    stage(1, 'Preparing the build tools')
    python = python_for(work)
    if not in_venv(work):   # the port's tools need the venv's packages (cryptography, lz4, capstone)
        raw = list(sys.argv[1:] if argv is None else argv)
        return reexec(python, [*raw, '--work', str(work), '-o', str(out)])
    hactool = hactool_for(work, python)
    stage(2, 'Reading and verifying the game data')
    prepared = prepare_game(work, python, base, update, keys, hactool)
    version = json.loads((prepared / 'preparation.json').read_text())['inputs']['update']['cnmt_version_raw']
    stage(3, 'Preparing the system fonts')
    fonts = prepare_fonts(work, python, keys, base,
                          args.firmware.expanduser().resolve() if args.firmware else None,
                          args.shared_fonts.expanduser().resolve() if args.shared_fonts else None)
    stage(4, 'Preparing the add-ons')
    dlc = prepare_dlc(work, python, keys, [d.expanduser().resolve() for d in args.dlc or []], version)
    stage(5, 'Preparing the frame-rate options')
    fps = None if args.no_fps_mod else fps_source(work, version, args.fps_mod.expanduser().resolve() if args.fps_mod else None)
    stage(6, 'Building the native host and the shaders')
    native = build_host(work, python, prepared, fonts, fps, dlc)
    finish_app(work, python, prepared, native, fonts, update, keys, out)
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def app_version(app):
    info = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        raise ValueError('not an Animal Crossing app of this repository')
    return info.get('CFBundleShortVersionString')


def blocker(app, work):
    """Why an update cannot rebuild the app from this work folder ('' when it can)."""
    work = Path(work)
    try:
        shown = app_version(app)
        summary = json.loads((work / 'prepared/preparation.json').read_text())
    except (OSError, ValueError):
        return 'the work folder has no verified game data of this app: a full build is required'
    if summary.get('status') != 'prepared_for_host_bringup':
        return 'the work folder\'s game data was not prepared to the end: a full build is required'
    try:
        version = int(summary['inputs']['update']['cnmt_version_raw'])
        expected = json.loads((GAME / f'assets/expected-v{version}.json').read_text())
    except (OSError, ValueError, KeyError, TypeError):
        return 'the work folder\'s game version is not one this repository builds: a full build is required'
    if expected['display_version'] != shown:
        return f'the work folder holds {expected["display_version"]}, the app is {shown}: a full build is required'
    if not fonts_ready(work / 'fonts'):
        return 'the work folder has no system fonts: a full build is required'
    if not (work / 'venv/bin/python').is_file():
        return 'the work folder has no build Python: a full build is required'
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
    prepared, fonts = work / 'prepared', work / 'fonts'
    stage(6, 'Building the native host and the shaders')
    native = build_host(work, python, prepared, fonts, keep_inputs=True)
    finish_app(work, python, prepared, native, fonts, None, None, app)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv[:1] == ['--update-app']:   # update() in the work folder's Python
            update(Path(argv[1]), Path(argv[2]))
        else:
            build(argv)
    except ValueError as error:
        print(f'ERROR: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
