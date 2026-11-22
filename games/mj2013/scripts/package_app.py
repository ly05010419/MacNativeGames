#!/usr/bin/env python3
"""Put a prepared work folder into 明星三缺一.app (app/launcher.c says the layout).

What the app carries is checked first: the rewritten main and the RomFS image against assets/expected.json, the runtime
present. Large files are APFS clones of the work folder's (no second copy on the same volume). The app is assembled
beside the output and replaces it only when complete, then signed ad hoc with its bundle ID as the designated
requirement and asked --check. The common steps are tools/port_app/bundle.py (as games/eow/scripts/package_app.py);
what the app carries and how it is checked are this file's.

    python3 scripts/package_app.py --work <work> --output "<folder>/明星三缺一.app" [--icon <AppIcon.icns or image>]
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
EXPECTED = json.loads((GAME / 'assets/expected.json').read_text())
BUNDLE_ID = 'org.macnativegames.mj2013'
EXECUTABLE = 'mj2013'                 # Contents/MacOS: ASCII; the names people see are Chinese
NAME, DISPLAY_NAME = '明星三缺一', '明星三缺一 2013'
VERSION = '1.0'


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
    """The three files the app carries: (runtime, main, RomFS image)."""
    runtime = work / 'build/mj2013-native'
    require(runtime.is_file() and os.access(runtime, os.X_OK), f'no runtime: run make runtime ({runtime})')
    main = work / 'rewritten/v0/main.patched.bin'
    require(main.is_file() and sha256(main) == EXPECTED['rewritten_sha256'], f'unverified rewrite: {main} (run make prepare)')
    image = work / 'extracted/v0/romfs/romfs.bin'
    require(image.is_file() and image.stat().st_size == EXPECTED['romfs_size'] and sha256(image) == EXPECTED['romfs_sha256'],
            f'RomFS image missing or not the expected one: {image}')
    return runtime, main, image


def package(work, output, icon=None, record=None):
    work, output = work.resolve(), output.resolve()
    require(output.suffix == '.app', '--output must end in .app')
    require(work not in output.parents and output not in work.parents, 'the app and the work folder must be separate')
    files = verify(work)
    with bundle.staged(output) as stage:   # assembled beside the output, which it replaces only when complete
        assemble(stage, files, icon, record)
    bundle.register(output)
    print(f'Packaged: {output}')
    return output


def assemble(stage, files, icon, record):
    """The bundle at stage, signed, verified and asked --check."""
    runtime, main, image = files
    contents, macos = stage / 'Contents', stage / 'Contents/MacOS'
    game = contents / 'Resources/game'
    macos.mkdir(parents=True)
    bundle.clone_file(runtime, game / 'build/mj2013-native')
    bundle.clone_file(main, game / 'rewritten/v0/main.patched.bin')
    bundle.clone_file(image, game / 'romfs/romfs.bin')
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
