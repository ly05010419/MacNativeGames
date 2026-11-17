#!/usr/bin/env python3
"""Put a built work folder into "The Legend of Zelda Echoes of Wisdom.app" (app/launcher.c says the layout).

Everything the app carries is checked first: the rewritten modules and the RomFS against assets/expected-v1.1.0.json,
the fonts against their manifest, the translator against its SHA-256, the shader seed and libraries with
build_shader_libs.py --check. Large files are APFS clones of the work folder's (no second copy on the same volume).
The app is assembled beside the output and replaces it only when complete, then signed ad hoc with its bundle ID as the
designated requirement (macOS then keeps a removable-volume answer across builds) and asked --check.

    python3 scripts/package_app.py --work <work> --output "<folder>/The Legend of Zelda Echoes of Wisdom.app"
           [--icon <AppIcon.icns>] [--record-work <work>]"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
REPO = GAME.parents[1]
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
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '-cRp', str(source), str(target)], check=True)


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
    stage = output.with_name(output.name + '.partial')
    if stage.exists():
        shutil.rmtree(stage)
    contents, macos = stage / 'Contents', stage / 'Contents/MacOS'
    game = contents / 'Resources/game'
    try:
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
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(GAME / 'app/launcher.c'),
                        '-framework', 'CoreFoundation', '-o', str(macos / EXECUTABLE)], check=True)
        info = {
            'CFBundleDevelopmentRegion': 'en',
            'CFBundleDisplayName': 'The Legend of Zelda: Echoes of Wisdom',
            'CFBundleExecutable': EXECUTABLE,
            'CFBundleIdentifier': BUNDLE_ID,
            'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleName': 'Echoes of Wisdom',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': EXPECTED['display_version'],
            'CFBundleVersion': '1',
            'LSApplicationCategoryType': 'public.app-category.games',
            'LSMinimumSystemVersion': os.environ.get('MACOSX_DEPLOYMENT_TARGET', '26.0'),
            'NSHighResolutionCapable': True,
        }
        if icon and Path(icon).is_file():
            clone(Path(icon), contents / 'Resources/AppIcon.icns')
            info['CFBundleIconFile'] = 'AppIcon'
        commit = subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True)
        if commit.returncode == 0:
            info['MNGSourceCommit'] = commit.stdout.strip()
        info.update(record or {})
        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
        subprocess.run(['codesign', '--force', '--deep', '--sign', '-', f'-r=designated => identifier "{BUNDLE_ID}"',
                        str(stage)], check=True, capture_output=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(stage)], check=True)
        subprocess.run([str(macos / EXECUTABLE), '--check'], check=True)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    if output.exists():
        shutil.rmtree(output)
    stage.rename(output)
    lsregister = ('/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/'
                  'lsregister')
    subprocess.run([lsregister, '-f', str(output)], check=False)
    print(f'Packaged: {output}')
    return output


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--icon', type=Path)
    ap.add_argument('--record-work', type=Path, help="recorded in Info.plist (an update reuses this work folder)")
    args = ap.parse_args()
    record = None
    if args.record_work:   # the keys build_app.record_update_source writes (update_runtime.py reads them)
        record = {'MNGSourceRepo': str(REPO), 'MNGWorkFolder': str(args.record_work.resolve()),
                  'MNGBuildPython': sys.executable}
    try:
        package(args.work, args.output, args.icon, record)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: packaging failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
