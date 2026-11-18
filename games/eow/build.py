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
and the app packaged from the work folder, without game files or keys. What every port's build.py does the same way (the
venv, the run inside it, the arguments, the work folder, the update's entry) is tools/port_app/adapter.py; this file
keeps Echoes of Wisdom's steps and checks."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

GAME = Path(__file__).resolve().parent
REPO = GAME.parents[1]
APP_NAME = 'The Legend of Zelda Echoes of Wisdom'   # tools/builder_jobs/repo.py GAMES['eow']
BUNDLE_ID = 'org.macnativegames.eow'                # scripts/package_app.py
TITLE = '01008cf01baac000'
STEPS = 7
MODULES = ('sdk', 'subsdk0', 'subsdk1', 'main')
EXPECTED = json.loads((GAME / 'assets/expected-v1.1.0.json').read_text())
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from tools.port_app import adapter  # noqa: E402  MNG Launcher's side every port's build.py shares
from tools.port_app.adapter import metal_env, say, stamp  # noqa: E402
from tools.port_app.make_path import make_path  # noqa: E402  every port's make WORK= (issue #2284)


def stage(number, words):
    adapter.stage(number, STEPS, words)


def run(cmd, work, name, env=None, cwd=GAME):
    """One step, its output in <work>/logs/<name>.log (MNG Launcher shows the logs a build starts as its steps)."""
    adapter.run_logged(cmd, work, name, env, cwd)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            digest.update(chunk)
    return digest.hexdigest()


def python_for(work):
    return adapter.python_for(work, requirements=GAME / 'requirements.txt')


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
    return adapter.fonts_ready(folder)


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
    make = ['make', f'PY={python}', f'WORK={make_path(work, "eow")}']
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


def checked_work(work, out):
    return adapter.checked_work(work, out, REPO)


def build(argv=None):
    p = adapter.parser(__doc__)
    adapter.add_inputs(p, game_help='the base game 1.0.0 (XCI or NSP)',
                       keys_help='your prod.keys (the update needs key generation 17)',
                       update_help='the 1.1.0 update NSP', update_required=True)
    adapter.add_fonts(p)
    adapter.add_output(p, APP_NAME, 'eow')
    args = p.parse_args(argv)

    base, keys, update = adapter.inputs(args)
    out, work = adapter.output_and_work(args, base, APP_NAME, 'eow', checked_work)

    stage(1, 'Preparing the build tools')
    python = python_for(work)
    if adapter.rerun_in_venv(__file__, python, work, argv, out):   # the scripts need the venv's packages (cryptography, lz4, capstone)
        return None
    metal_env()   # before an hour of work: say now when the shaders cannot be compiled
    extract(work, python, base, update, keys)
    prepare_fonts(work, python, keys, base, adapter.given(args.firmware), adapter.given(args.shared_fonts))
    rewrite(work, python)
    host(work, python)
    package(work, python, out, update, keys)
    return out


# --- MNG Launcher's update (update_runtime.py) -----------------------------------------------------------------------

def blocker(app, work):
    """Why an update cannot remake the app from this work folder ('' when it can)."""
    work = Path(work)
    info = adapter.app_info(app)
    if info is None:
        return 'the app has no readable Info.plist: a full build is required'
    if info.get('CFBundleIdentifier') != BUNDLE_ID:
        return 'not an Echoes of Wisdom app of this repository'
    missing = adapter.first_missing([(work / 'venv/bin/python', 'build Python'),
                                     (work / 'extracted/v110/romfs/romfs.bin', 'game data'),
                                     (work / 'build/shader-seed/seed-manifest.json', 'prepared shaders')])
    if missing:
        return missing
    if not fonts_ready(work / 'extracted/firmware/shared-fonts'):
        return 'the work folder has no system fonts: a full build is required'
    for module in MODULES:
        if not (work / f'rewritten/v110/{module}.patched.bin').is_file():
            return 'the work folder has no rewritten code: a full build is required'
    return ''


def update(app, work):
    ready = adapter.update_in_venv(__file__, app, work, blocker=blocker, python=python_for, check=checked_work)
    if not ready:
        return None
    app, work, python = ready
    host(work, python, seed=False)   # make prepare checks every input against the pinned hashes again
    package(work, python, app)
    return None


def main(argv=None):
    return adapter.main(build, update, argv)


if __name__ == '__main__':
    raise SystemExit(main())
