#!/usr/bin/env python3
"""The Legend of Zelda: Echoes of Wisdom 1.1.0 in one command: the base game and the 1.1.0 update in, a Mac app out.

    python3 games/eow/build.py <base.xci|nsp> -k <prod.keys> --update <1.1.0 update.nsp>
                               [--firmware <NCA folder> | --shared-fonts <fonts folder>] [--work <folder>] [-o <app>]

The README's steps in order, each logging to <work>/logs/<step>.log; what an earlier run in the same work folder made
from the same files is reused:
  1/7 tools    the build's Python (<work>/venv, requirements.txt)
  2/7 code     scripts/extract_code.py: the update's ExeFS
  3/7 data     scripts/merge_romfs.py: base and update RomFS merged and verified (about 8 GB)
  4/7 fonts    the six system fonts: --shared-fonts, --firmware, or the card image's update partition
  5/7 rewrite  third_party/d2r/patch_nso.py for main, sdk, subsdk0, subsdk1
  6/7 host     make prepare runtime shader-tools, then make shader-seed (every shader translated and compiled; needs
               Xcode's Metal compiler) and the app icon
  7/7 app      scripts/package_app.py; the app records this work folder (MNGWorkFolder)

MNG Launcher's update (update_runtime.py) is blocker(app, work) and update(app, work): the runtime is compiled again
and the app packaged from the work folder, without game files or keys."""
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
APP_NAME = 'The Legend of Zelda Echoes of Wisdom'   # tools/builder_jobs/repo.py GAMES['eow']
BUNDLE_ID = 'org.macnativegames.eow'                # scripts/package_app.py
TITLE = '01008cf01baac000'
STEPS = 7
MODULES = ('sdk', 'subsdk0', 'subsdk1', 'main')
EXPECTED = json.loads((GAME / 'assets/expected-v1.1.0.json').read_text())
XCODE = Path('/Applications/Xcode.app/Contents/Developer')
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))


def say(message):
    print(message, flush=True)


def stage(number, words):
    say(f'{number}/{STEPS} {words}')   # tools/builder/progress.py reads the 'i/N' lines as the build's stages


def run(cmd, work, name, env=None, cwd=GAME):
    """One step, its output in <work>/logs/<name>.log (MNG Launcher shows the logs a build starts as its steps)."""
    log = work / 'logs' / f'{name}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        result = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=out, stderr=subprocess.STDOUT,
                                env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', **(env or {})})
    if result.returncode:
        tail = log.read_text(errors='replace').strip().splitlines()[-12:]
        raise ValueError(f'{name} failed (full output: {log})\n' + '\n'.join(tail))


def stamp(path):
    st = Path(path).stat()
    return [str(Path(path).resolve()), st.st_size, st.st_mtime_ns]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            digest.update(chunk)
    return digest.hexdigest()


def python_for(work):
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


def metal_env():
    """The Metal compiler for the shader libraries: the selected developer folder's, else Xcode's."""
    if subprocess.run(['xcrun', '-f', 'metal'], capture_output=True).returncode == 0:
        return {}
    if (XCODE / 'usr/bin').is_dir() and subprocess.run(['xcrun', '-f', 'metal'], capture_output=True,
                                                       env={**os.environ, 'DEVELOPER_DIR': str(XCODE)}).returncode == 0:
        return {'DEVELOPER_DIR': str(XCODE)}
    raise ValueError('the shaders are compiled with the Metal compiler, which is not installed: install Xcode, then '
                     'xcodebuild -downloadComponent MetalToolchain')


def same_inputs(work, name, inputs):
    try:
        return json.loads((work / f'{name}-inputs.json').read_text()) == inputs
    except (OSError, ValueError):
        return False


def remember(work, name, inputs):
    (work / f'{name}-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')


def extract(work, python, base, update, keys):
    """Code and data, reused while the files are the same and their hashes still the pinned ones."""
    exefs, romfs = work / 'extracted/v110/exefs', work / 'extracted/v110/romfs'
    stage(2, 'Extracting the game code')
    inputs = {'update': stamp(update), 'keys': hashlib.sha256(keys.read_bytes()).hexdigest()}
    if same_inputs(work, 'code', inputs) and all(
            (exefs / name).is_file() and sha256(exefs / name) == digest for name, digest in EXPECTED['exefs_sha256'].items()):
        say('code: reusing the verified code of the last build')
    else:
        shutil.rmtree(exefs, ignore_errors=True)
        run([python, 'scripts/extract_code.py', update, '--keys', keys, '--output', exefs], work, 'extract-code')
        remember(work, 'code', inputs)
    stage(3, 'Merging and verifying the game data')
    inputs = {'base': stamp(base), **inputs}
    image = romfs / 'romfs.bin'
    if same_inputs(work, 'romfs', inputs) and image.is_file() and image.stat().st_size == EXPECTED['romfs']['size']:
        say('game data: reusing the verified RomFS of the last build')
    else:
        shutil.rmtree(romfs, ignore_errors=True)
        run([python, 'scripts/merge_romfs.py', base, update, '--keys', keys, '--output', romfs], work, 'merge-romfs')
        remember(work, 'romfs', inputs)


def fonts_ready(folder):
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6)) and all(
            (folder / f'{f["type"]}-{f["name"]}.bfttf').is_file() for f in manifest['fonts'])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def prepare_fonts(work, python, keys, base, firmware=None, shared=None):
    stage(4, 'Preparing the system fonts')
    fonts = work / 'extracted/firmware/shared-fonts'
    if shared:
        if not fonts_ready(shared):
            raise ValueError(f'{shared} is not a shared-fonts folder of an earlier build (six .bfttf files and manifest.json)')
        shutil.rmtree(fonts, ignore_errors=True)
        shutil.copytree(shared, fonts)
    elif firmware or not fonts_ready(fonts):
        shutil.rmtree(fonts, ignore_errors=True)
        source = ['--firmware', firmware] if firmware else ['--xci', base] if base.suffix.lower() == '.xci' else None
        if not source:
            raise ValueError('an NSP holds no system fonts: give --firmware <unpacked firmware NCA folder> or '
                             '--shared-fonts <the shared-fonts folder of an earlier build>')
        try:
            run([python, 'scripts/extract_shared_fonts.py', '--keys', keys, *source, '--output', fonts], work, 'fonts')
        except ValueError as error:
            raise ValueError(f'{error}\n(a card image without its system update has no fonts: give --firmware or '
                             '--shared-fonts)') from None
    else:
        say('fonts: reusing the verified fonts of the last build')


def rewrite(work, python):
    stage(5, 'Rewriting the code for Apple silicon')
    out = work / 'rewritten/v110'
    if all((out / f'{m}.patched.bin').is_file() and sha256(out / f'{m}.patched.bin') == EXPECTED['rewritten_sha256'][f'{m}.patched.bin']
           for m in MODULES):
        say('rewrite: reusing the verified rewrite of the last build')
        return
    env = {'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'}
    for module in MODULES:
        run([python, 'third_party/d2r/patch_nso.py', work / f'extracted/v110/exefs/{module}', out, module], work,
            f'rewrite-{module}', env)


def host(work, python, seed=True):
    """make prepare (every input against the pinned hashes) runtime shader-tools, then the shader seed."""
    stage(6, 'Building the native host and the shaders')
    make = ['make', f'PY={python}', f'WORK={work}']
    env = {'MACOSX_DEPLOYMENT_TARGET': os.environ.get('MACOSX_DEPLOYMENT_TARGET', '26.0')}
    run([*make, 'prepare', 'runtime', 'shader-tools'], work, 'make', env)
    if seed:
        run([*make, 'shader-seed'], work, 'shader-seed', {**env, **metal_env()})


def package(work, python, out, update=None, keys=None):
    if update and keys:
        run([python, 'scripts/extract_app_icon.py', update, '--keys', keys, '--title', TITLE,
             '--output', work / 'build/AppIcon.icns'], work, 'icon')
    stage(7, 'Packaging the app')
    icon = work / 'build/AppIcon.icns'
    run([python, 'scripts/package_app.py', '--work', work, '--output', out, '--record-work', work,
         *(['--icon', icon] if icon.is_file() else [])], work, 'package')
    say(f'Built: {out}')


def in_venv(work):
    return Path(sys.prefix).resolve() == (work / 'venv').resolve()


def reexec(python, argv, *extra):
    result = subprocess.run([str(python), str(Path(__file__).resolve()), *extra, *map(str, argv)])
    if result.returncode:
        raise ValueError('the build stopped (its ERROR line above says why)')


def checked_work(work, out):
    if work == REPO or REPO in work.parents or work in REPO.parents or work == out or out in work.parents:
        raise ValueError('the work folder must be outside the checkout and outside the app')
    return work


def build(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path, help='the base game 1.0.0 (XCI or NSP)')
    p.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys (the update needs key generation 17)')
    p.add_argument('--update', type=Path, required=True, help='the 1.1.0 update NSP')
    fonts = p.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder with the system fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='the shared-fonts folder of an earlier build')
    p.add_argument('--work', type=Path, help='work folder (default: the one the app at -o records, else MacNativeGames-work/eow)')
    p.add_argument('-o', '--out', '--output', dest='out', type=Path, help=f'the app to write (default: {APP_NAME}.app next to the game file)')
    args = p.parse_args(argv)

    base, keys, update = (path.expanduser().resolve() for path in (args.game, args.keys, args.update))
    for path in (base, keys, update):
        if not path.is_file():
            raise ValueError(f'no such file: {path}')
    out = (args.out.expanduser() if args.out else base.parent / f'{APP_NAME}.app').resolve()
    if out.suffix != '.app':
        raise ValueError(f'the output must end in .app: {out}')
    if args.work:
        work, why = args.work.expanduser().resolve(), 'given with --work'
    else:
        import build_app
        work, why = build_app.default_work(out, 'eow')
    work = checked_work(work, out)
    say(f'work folder {work} ({why})')
    work.mkdir(parents=True, exist_ok=True)

    stage(1, 'Preparing the build tools')
    python = python_for(work)
    if not in_venv(work):   # the scripts need the venv's packages (cryptography, lz4, capstone)
        raw = list(sys.argv[1:] if argv is None else argv)
        return reexec(python, [*raw, '--work', str(work), '-o', str(out)])
    metal_env()   # before an hour of work: say now when the shaders cannot be compiled
    extract(work, python, base, update, keys)
    prepare_fonts(work, python, keys, base, args.firmware.expanduser().resolve() if args.firmware else None,
                  args.shared_fonts.expanduser().resolve() if args.shared_fonts else None)
    rewrite(work, python)
    host(work, python)
    package(work, python, out, update, keys)
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def blocker(app, work):
    """Why an update cannot remake the app from this work folder ('' when it can)."""
    work = Path(work)
    try:
        info = plistlib.loads((Path(app) / 'Contents/Info.plist').read_bytes())
    except (OSError, ValueError):
        return 'the app has no readable Info.plist: a full build is required'
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        return 'not an Echoes of Wisdom app of this repository'
    for path, what in ((work / 'venv/bin/python', 'build Python'),
                       (work / 'extracted/v110/romfs/romfs.bin', 'game data'),
                       (work / 'build/shader-seed/seed-manifest.json', 'prepared shaders')):
        if not path.is_file():
            return f'the work folder has no {what}: a full build is required'
    if not fonts_ready(work / 'extracted/firmware/shared-fonts'):
        return 'the work folder has no system fonts: a full build is required'
    for module in MODULES:
        if not (work / f'rewritten/v110/{module}.patched.bin').is_file():
            return 'the work folder has no rewritten code: a full build is required'
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
    host(work, python, seed=False)   # make prepare checks every input against the pinned hashes again
    package(work, python, app)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv[:1] == ['--update-app']:
            update(Path(argv[1]), Path(argv[2]))
        else:
            build(argv)
    except ValueError as error:
        print(f'ERROR: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
