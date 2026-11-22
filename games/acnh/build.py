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
build fits, update(app, work) rebuilds the host and the app from the work folder's prepared files, without game files.
What every port's build.py does the same way (the venv, the run inside it, the arguments, the work folder, the update's
entry) is tools/port_app/adapter.py; this file keeps Animal Crossing's steps and checks."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parent
REPO = GAME.parents[1]
APP_NAME = 'Animal Crossing New Horizons'   # tools/builder_jobs/repo.py GAMES['acnh']
BUNDLE_ID = 'org.macnativegames.acnh'       # scripts/package_native_app.py
TITLE_ID = '01006F8002326000'
FPS_SOURCE_URL = 'https://github.com/tmouh/acnh-60fps'
STEPS = 7
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from tools.port_app import adapter  # noqa: E402  MNG Launcher's side every port's build.py shares
from tools.port_app.adapter import say, stamp  # noqa: E402


def stage(number, words):
    adapter.stage(number, STEPS, words)


def run(cmd, work, name, env=None, cwd=REPO):
    """One step, its output in <work>/logs/<name>.log (MNG Launcher shows the logs a build starts as its steps)."""
    adapter.run_logged(cmd, work, name, env, cwd)


def build_env():
    # The app and its runtime target the Mac they are built on (README: 27.0 on the maintainer's Mac).
    return {'MACOSX_DEPLOYMENT_TARGET': os.environ.get('MACOSX_DEPLOYMENT_TARGET', '26.0')}


def python_for(work):
    """<work>/venv with requirements.txt, made once per work folder."""
    return adapter.python_for(work, requirements=GAME / 'requirements.txt')


def hactool_for(work, python):
    """The pinned hactool: the release's prebuilt one when this source has it (tools/prebuilt.py: build_app.build_hactool,
    the same commit and hooks as tools/build_hactool.py; user 2026-11-22: a player compiles nothing), else built here."""
    tool = work / 'tools/hactool/hactool'
    sys.path.insert(0, str(REPO))
    import tools.prebuilt as prebuilt
    if prebuilt.keep_placed(REPO, 'hactool', tool):
        return tool
    if not (tool.is_file() and os.access(tool, os.X_OK)):
        prebuilt.compiler_needed('hactool')
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


def combined_card(work, base, update, dlcs, keys):
    """(base, update, add-ons) to build from: a combined card image (a merged XCI, "1G+1U+3D") is split into one NSP per
    title in <work>/card (games/acnh/tools/split_card.py), its update and add-ons used unless given separately."""
    from games.acnh.tools.split_card import is_combined, split
    if base.suffix.lower() != '.xci' or not is_combined(base):
        return base, update, dlcs
    say('game data: this card image also holds the update or add-ons: splitting it into one package per title '
        '(about a minute)')
    found = split(base, keys, work / 'card', int(TITLE_ID, 16))
    if update is None or update == base:
        update = found['update']
    return found['base'], update, (dlcs if dlcs and dlcs != [base] else found['dlc'])


def fonts_ready(folder):
    return adapter.fonts_ready(folder, ('bfttf_bytes', 'font_bytes'))   # build_host.py reads both sizes


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


def checked_work(work, out):
    return adapter.checked_work(work, out, REPO)


def build(argv=None):
    p = adapter.parser(__doc__)
    adapter.add_inputs(p, game_help='the base game 1.0.0 (NSP or XCI)', keys_help='your prod.keys',
                       update_help='the 1.2.1, 3.0.1 or 3.0.3 update NSP (not needed for a combined card that holds '
                                   'one)', update_required=False)
    p.add_argument('--dlc', type=Path, action='append', help='a pinned add-on NSP (3.0.1, 3.0.3); repeat for each')
    adapter.add_fonts(p)
    fps = p.add_mutually_exclusive_group()
    fps.add_argument('--fps-mod', type=Path, help='a local tmouh/acnh-60fps checkout at the pinned commit')
    fps.add_argument('--no-fps-mod', action='store_true', help='original 30 FPS only; nothing is fetched')
    adapter.add_output(p, APP_NAME, 'acnh')
    args = p.parse_args(argv)

    base, keys, update = adapter.inputs(args)
    out, work = adapter.output_and_work(args, base, APP_NAME, 'acnh', checked_work)

    stage(1, 'Preparing the build tools')
    python = python_for(work)
    if adapter.rerun_in_venv(__file__, python, work, argv, out):   # the port's tools need the venv's packages (cryptography, lz4, capstone)
        return None
    hactool = hactool_for(work, python)
    stage(2, 'Reading and verifying the game data')
    card, dlcs = base, [d.expanduser().resolve() for d in args.dlc or []]
    base, update, dlcs = combined_card(work, base, update, dlcs, keys)
    if update is None:
        raise ValueError('give the 1.2.1, 3.0.1 or 3.0.3 update with --update')
    prepared = prepare_game(work, python, base, update, keys, hactool)
    version = json.loads((prepared / 'preparation.json').read_text())['inputs']['update']['cnmt_version_raw']
    stage(3, 'Preparing the system fonts')
    fonts = prepare_fonts(work, python, keys, card, adapter.given(args.firmware), adapter.given(args.shared_fonts))
    stage(4, 'Preparing the add-ons')
    dlc = prepare_dlc(work, python, keys, dlcs, version)
    stage(5, 'Preparing the frame-rate options')
    fps = None if args.no_fps_mod else fps_source(work, version, adapter.given(args.fps_mod))
    stage(6, 'Building the native host and the shaders')
    native = build_host(work, python, prepared, fonts, fps, dlc)
    finish_app(work, python, prepared, native, fonts, update, keys, out)
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def app_version(app):
    info = adapter.app_info(app)
    if info is None:
        raise ValueError('the app has no readable Info.plist')
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
    return adapter.first_missing([(work / 'venv/bin/python', 'build Python')])


def update(app, work):
    ready = adapter.update_in_venv(__file__, app, work, blocker=blocker, python=python_for, check=checked_work)
    if not ready:
        return None
    app, work, python = ready
    prepared, fonts = work / 'prepared', work / 'fonts'
    stage(6, 'Building the native host and the shaders')
    native = build_host(work, python, prepared, fonts, keep_inputs=True)
    finish_app(work, python, prepared, native, fonts, None, None, app)
    return None


def main(argv=None):
    return adapter.main(build, update, argv)


if __name__ == '__main__':
    raise SystemExit(main())
