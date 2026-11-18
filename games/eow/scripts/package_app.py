#!/usr/bin/env python3
"""Put a built work folder into "The Legend of Zelda Echoes of Wisdom.app" (app/launcher.c says the layout).

Everything the app carries is checked first: the rewritten modules and the RomFS against assets/expected-v1.1.0.json,
the fonts against their manifest, the translator against its SHA-256, the shader seed and libraries with
build_shader_libs.py --check. Large files are APFS clones of the work folder's (no second copy on the same volume).
The app is assembled beside the output and replaces it only when complete, then signed ad hoc with its bundle ID as the
designated requirement (macOS then keeps a removable-volume answer across builds) and asked --check. Those common steps
are tools/port_app/bundle.py; what the app carries and how it is checked are this file's.

    python3 scripts/package_app.py --work <work> --output "<folder>/The Legend of Zelda Echoes of Wisdom.app"
           [--icon <AppIcon.icns>] [--record-work <work>]"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
REPO = GAME.parents[1]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from tools.port_app import bundle  # noqa: E402
EXPECTED = json.loads((GAME / 'assets/expected-v1.1.0.json').read_text())
BUNDLE_ID = 'org.macnativegames.eow'
EXECUTABLE = 'Echoes of Wisdom'
MODULES = ('main', 'sdk', 'subsdk0', 'subsdk1')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            digest.update(chunk)
    return digest.hexdigest()


def clone(source, target):
    """An APFS clone when source and target share a volume, else a copy (cp -c falls back by itself)."""
    bundle.clone_tree(source, target)


def verify(work):
    for name in MODULES:
        path = work / f'rewritten/v110/{name}.patched.bin'
        require(path.is_file() and sha256(path) == EXPECTED['rewritten_sha256'][f'{name}.patched.bin'],
                f'unverified rewrite: {path}')
    romfs = work / 'extracted/v110/romfs'
    image = romfs / 'romfs.bin'
    require(image.is_file() and image.stat().st_size == EXPECTED['romfs']['size'], f'RomFS missing or not the expected size: {image}')
    require((romfs / 'runtime-index.bin').is_file(), 'no RomFS index: run make prepare')
    fonts = work / 'extracted/firmware/shared-fonts'
    manifest = json.loads((fonts / 'manifest.json').read_text())
    require(manifest.get('status') == 'pass' and len(manifest.get('fonts', ())) == 6, f'no verified fonts in {fonts}')
    for row in manifest['fonts']:
        require(sha256(fonts / f'{row["type"]}-{row["name"]}.bfttf') == row['sha256'], f'font {row["name"]} changed')
    runtime = work / 'build/eow-native'
    require(runtime.is_file() and os.access(runtime, os.X_OK), 'no runtime: run make runtime')
    translator = work / 'build/shader-service/totk_xlate'
    pinned = (GAME / 'prebuilt/macos-arm64/totk_xlate.sha256').read_text().split()[0]
    require(translator.is_file() and sha256(translator) == pinned, 'the shader translator is missing or not the pinned one')
    seed = work / 'build/shader-seed/seed-manifest.json'
    require(seed.is_file(), 'no prepared shaders: run make shader-seed')
    check = subprocess.run([sys.executable, str(GAME / 'scripts/build_shader_libs.py'), '--work', str(work), '--check'],
                           capture_output=True, text=True)
    require(check.returncode == 0, 'the shader libraries do not serve the prepared seed (make shader-seed): '
            + (check.stdout + check.stderr).strip()[-300:])
    return romfs, fonts


def package(work, output, icon=None, record=None):
    work, output = work.resolve(), output.resolve()
    require(output.suffix == '.app', '--output must end in .app')
    romfs, fonts = verify(work)
    with bundle.staged(output) as stage:   # assembled beside the output, which it replaces only when complete
        assemble(stage, work, romfs, fonts, icon, record)
    bundle.register(output)
    print(f'Packaged: {output}')
    return output


def assemble(stage, work, romfs, fonts, icon, record):
    """The bundle at stage, signed, verified and asked --check."""
    contents, macos = stage / 'Contents', stage / 'Contents/MacOS'
    game = contents / 'Resources/game'
    macos.mkdir(parents=True)
    build = game / 'build'
    for name in ('eow-native', 'shader-service', 'shader-seed', 'shader-libs'):
        clone(work / 'build' / name, build / name)
    if (work / 'build/spirv-cross/spirv-cross').is_file():
        clone(work / 'build/spirv-cross/spirv-cross', build / 'spirv-cross/spirv-cross')
    shutil.rmtree(build / 'shader-libs/src', ignore_errors=True)   # sources kept for offline builds, not played
    for name in MODULES:
        clone(work / f'rewritten/v110/{name}.patched.bin', game / f'rewritten/v110/{name}.patched.bin')
    for name in ('romfs.bin', 'runtime-index.bin'):
        clone(romfs / name, game / 'romfs' / name)
    clone(fonts, game / 'fonts')
    bundle.compile_launcher(GAME / 'app/launcher.c', macos / EXECUTABLE, frameworks=['CoreFoundation'])
    icon_file = None
    if icon and Path(icon).is_file():
        clone(Path(icon), contents / 'Resources/AppIcon.icns')
        icon_file = 'AppIcon'
    info = bundle.bundle_info(executable=EXECUTABLE, bundle_id=BUNDLE_ID, name='Echoes of Wisdom',
                              display_name='The Legend of Zelda: Echoes of Wisdom', version=EXPECTED['display_version'],
                              build='1', minimum_system=bundle.deployment_target(), icon_file=icon_file)
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
    ap.add_argument('--record-work', type=Path, help="recorded in Info.plist (an update reuses this work folder)")
    args = ap.parse_args()
    record = None
    if args.record_work:   # the keys build_app.record_update_source writes (update_runtime.py reads them)
        record = bundle.update_record(args.record_work.resolve(), REPO)
    try:
        package(args.work, args.output, args.icon, record)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: packaging failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
