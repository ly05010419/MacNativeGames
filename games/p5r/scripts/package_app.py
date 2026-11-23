#!/usr/bin/env python3
"""Put a prepared work folder into Persona 5 Royal.app (app/launcher.c says the layout).

What the app carries is checked first: the three rewritten modules against assets/expected-1.0.2.json, the RomFS image
with its index, the runtime and the shader translator with SPIRV-Cross present. Large files are APFS clones of the work
folder's (no second copy on the same volume). The app is assembled beside the output and replaces it only when complete,
then signed ad hoc with its bundle ID as the designated requirement and asked --check. The common steps are
tools/port_app/bundle.py (as games/mj2013/scripts/package_app.py and games/eow/scripts/package_app.py); what the app
carries and how it is checked are this file's.

    python3 scripts/package_app.py --work <work> --output "<folder>/Persona 5 Royal.app" [--icon <AppIcon.icns or image>]
           [--record-work <work>]"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
REPO = GAME.parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from tools.port_app import bundle  # noqa: E402
EXPECTED = json.loads((GAME / 'assets/expected-1.0.2.json').read_text())
BUNDLE_ID = 'org.macnativegames.p5r'   # runtime/game_info.plist
EXECUTABLE = 'P5R'                     # Contents/MacOS: the player's data folder name (runtime/game_version.h GAME_FOLDER)
NAME = DISPLAY_NAME = 'Persona 5 Royal'
VERSION = EXPECTED['version']
ROMFS_FILES = ('index.json', 'runtime-index.bin', 'romfs.bin')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify(work):
    """What the app carries, checked: {name: path}."""
    files = {
        'runtime': work / 'build/p5r-native',
        'translator': work / 'build/shader-service/totk_xlate',
        'spirv-cross': work / 'build/spirv-cross/spirv-cross',
    }
    for name, path in files.items():
        require(path.is_file() and os.access(path, os.X_OK), f'no {name}: run make runtime ({path})')
    modules = work / 'rewritten/local'
    for module, want in EXPECTED['rewritten_sha256'].items():
        path = modules / module   # main.patched.bin, sdk.patched.bin, subsdk0.patched.bin
        require(path.is_file() and sha256(path) == want, f'unverified rewrite: {path} (run make prepare)')
    files['modules'] = modules
    romfs = work / 'extracted/local/romfs'
    for name in ROMFS_FILES:
        require((romfs / name).is_file(), f'RomFS {name} missing: {romfs} (run make prepare)')
    require((romfs / 'romfs.bin').stat().st_size > 1 << 30, f'RomFS image too small: {romfs / "romfs.bin"}')
    files['romfs'] = romfs
    return files


def package(work, output, icon=None, record=None):
    work, output = work.resolve(), output.resolve()
    require(output.suffix == '.app', '--output must end in .app')
    require(work not in output.parents and output not in work.parents, 'the app and the work folder must be separate')
    files = verify(work)
    if icon is None and (work / 'build/AppIcon.icns').is_file():
        icon = work / 'build/AppIcon.icns'
    with bundle.staged(output) as stage:   # assembled beside the output, which it replaces only when complete
        assemble(stage, files, icon, record)
    bundle.register(output)
    print(f'Packaged: {output}')
    return output


def assemble(stage, files, icon, record):
    """The bundle at stage, signed, verified and asked --check."""
    contents, macos = stage / 'Contents', stage / 'Contents/MacOS'
    game = contents / 'Resources/game'
    macos.mkdir(parents=True)
    bundle.clone_file(files['runtime'], game / 'build/p5r-native')
    bundle.clone_file(files['translator'], game / 'build/shader-service/totk_xlate')
    bundle.clone_file(files['spirv-cross'], game / 'build/spirv-cross/spirv-cross')
    bundle.clone_tree(files['modules'], game / 'rewritten/local')
    for name in ROMFS_FILES:
        bundle.clone_file(files['romfs'] / name, game / 'romfs' / name)
    (game / 'fonts').mkdir(parents=True)   # the game asks for no system font
    bundle.compile_launcher(GAME / 'app/launcher.c', macos / EXECUTABLE)
    icon_file = bundle.icon(icon, contents / 'Resources', 'AppIcon')
    info = bundle.bundle_info(executable=EXECUTABLE, bundle_id=BUNDLE_ID, name=NAME, display_name=DISPLAY_NAME,
                              version=VERSION, build='1', minimum_system=bundle.deployment_target(), icon_file=icon_file)
    commit = bundle.source_commit(REPO)
    bundle.write_info(stage, {**info, **({'MNGSourceCommit': commit} if commit else {}), **(record or {})})
    bundle.sign(stage, BUNDLE_ID, deep=True)
    bundle.verify_signature(stage)
    bundle.check_launcher(macos / EXECUTABLE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--icon', type=Path)
    ap.add_argument('--record-work', type=Path, help='recorded in Info.plist (an update reuses this work folder)')
    args = ap.parse_args()
    record = bundle.update_record(args.record_work.resolve(), REPO) if args.record_work else None
    try:
        package(args.work, args.output, args.icon, record)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: packaging failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
