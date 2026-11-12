#!/usr/bin/env python3
"""Assemble a PRIVATE local TMNT host from verified, user-owned inputs.

The bundle contains game data. Never commit, upload or distribute its output.
This step does not launch the game or certify that gameplay works.
"""
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import stat
from pathlib import Path

from extract_exefs import validate_private_output
from expected_inputs import check_supported_romfs
from rewrite_modules import REPO
from rewrite_nro import verify_nro_report

MODULES = ('main', 'subsdk0', 'subsdk2', 'subsdk1')
SUFFIXES = ('patched.bin', 'fixups.tsv', 'gates.tsv')

APP_ID = 'org.macnativegames.tmnt.private'


def write_app_entry(app, runtime_sha256):
    if len(runtime_sha256) != 64 or any(c not in '0123456789abcdef' for c in runtime_sha256):
        raise ValueError('invalid runtime SHA-256 for profile identity')
    contents = app / 'Contents'
    executable = contents / 'MacOS/PlayTMNT'
    executable.parent.mkdir(parents=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': 'PlayTMNT',
        'CFBundleIdentifier': APP_ID, 'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': "TMNT Shredder's Revenge", 'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': '1.0.0', 'CFBundleVersion': '1',
        'NSHighResolutionCapable': True,
    }))
    executable.write_text('#!/bin/sh\nset -eu\n'
        'CONTENTS=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\n'
        'ROOT="$CONTENTS/Resources/game"\n'
        f'PROFILE="${{HOME}}/Library/Application Support/MacNativeGames/TMNT-{runtime_sha256[:12]}"\n'
        'mkdir -p "$PROFILE"\n'
        'cd "$ROOT"\n'
        'export TOTK_ROMFS="$ROOT/romfs"\n'
        'export TOTK_PROFILE_DIR="$PROFILE"\n'
        'export TOTK_GAME_SLOT_DIR="$HOME/Library/Application Support/MacNativeGames/game-slot"\n'
        'export TOTK_SHARED_FONTS="$ROOT/shared-fonts"\n'
        'export TOTK_SHADER_CACHE="$PROFILE/shader-cache"\n'
        'export TOTK_SHADER_SEED=off TOTK_SHADER_SERVICE=native\n'
        'export TOTK_ROMFS_MODS=0 TOTK_ROMFS_PATCHES=0\n'
        'export TOTK_GPU_ALLOW_SUBMIT=1 TMNT_SPLIT_RECORDING=1\n'
        'export TMNT_DRAW_TEXTURE_INVERT=1 TMNT_STRICT_FRAME_DEPENDENCIES=1\n'
        'exec "$ROOT/build/tmnt-native" "$ROOT/rewritten" boot\n')
    executable.chmod(executable.stat().st_mode | stat.S_IXUSR)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pack(args):
    bundle = args.out.resolve()
    validate_private_output(bundle)
    if bundle == REPO or bundle.is_relative_to(REPO):
        raise ValueError('private bundle must stay outside the repository')
    if args.app and bundle.suffix != '.app':
        raise ValueError('app output must end in .app')
    output = bundle / 'Contents/Resources/game' if args.app else bundle
    report = json.loads((args.rewritten / 'report.json').read_text())
    if report.get('options') != {'ruleset': 'native', 'private_gates': False,
                                  'tmnt_graphics_gates': True}:
        raise ValueError('ExeFS rewrite options do not match TMNT host')
    for name in ('main', 'sdk', 'subsdk0', 'subsdk2', 'subsdk1'):
        entry = report['modules'][name]
        if entry['exit_code'] or any(digest(args.rewritten / f'{name}.{suffix}') != sha
                                     for suffix, sha in entry['outputs'].items()):
            raise ValueError(f'ExeFS rewrite artifact mismatch: {name}')
    verify_nro_report(args.nro_prepared, args.nro_rewritten)
    romfs_manifest = json.loads((args.romfs / 'index.json').read_text())
    check_supported_romfs(romfs_manifest['romfs_sha256'])
    if (romfs_manifest.get('status') != 'pass'
            or digest(args.romfs / 'romfs.bin') != romfs_manifest['romfs_sha256']):
        raise ValueError('RomFS differs from verified extraction')
    index_hash = digest(args.resources / 'runtime-index.bin')
    config = (args.resources / 'romfs_config.h').read_text()
    if f'"{index_hash}"' not in config or f'"{romfs_manifest["romfs_sha256"]}"' not in config:
        raise ValueError('runtime resource configuration does not match RomFS')
    if args.fonts:
        font_manifest = json.loads((args.fonts / 'manifest.json').read_text())
        if font_manifest.get('status') != 'pass' or len(font_manifest['fonts']) != 6:
            raise ValueError('font manifest missing or incomplete')
        for row in font_manifest['fonts']:
            source = args.fonts / f"{row['type']}-{row['name']}.bfttf"
            if digest(source) != row['sha256'] or source.stat().st_size != row['bfttf_bytes']:
                raise ValueError('shared font mismatch')
    for tool in (args.native, args.shader_service, args.spirv_cross):
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise ValueError(f'executable dependency missing: {tool}')
    # Validate everything before creating output. All source paths are explicit.
    output.mkdir(parents=True)
    (output / 'build/shader-service').mkdir(parents=True)
    (output / 'build/spirv-cross').mkdir(parents=True)
    (output / 'rewritten/nro').mkdir(parents=True)
    (output / 'romfs').mkdir()
    (output / 'shared-fonts').mkdir()
    if not args.app:
        (output / 'user-data').mkdir()
    shutil.copy2(args.native, output / 'build/tmnt-native')
    shutil.copy2(args.shader_service, output / 'build/shader-service/totk_xlate')
    shutil.copy2(args.spirv_cross, output / 'build/spirv-cross/spirv-cross')
    for module in MODULES:
        for suffix in SUFFIXES:
            shutil.copy2(args.rewritten / f'{module}.{suffix}', output / 'rewritten' / f'{module}.{suffix}')
    nro_id = digest(args.nro_prepared / 'module.nro')
    for suffix in SUFFIXES:
        shutil.copy2(args.nro_rewritten / f'nro.{suffix}', output / 'rewritten/nro' / f'{nro_id}.{suffix}')
    shutil.copy2(args.resources / 'runtime-index.bin', output / 'romfs/runtime-index.bin')
    if args.link_romfs:
        (output / 'romfs/romfs.bin').symlink_to((args.romfs / 'romfs.bin').resolve())
    else:
        shutil.copy2(args.romfs / 'romfs.bin', output / 'romfs/romfs.bin')
    if args.fonts:
        for row in font_manifest['fonts']:
            name = f"{row['type']}-{row['name']}.bfttf"
            shutil.copy2(args.fonts / name, output / 'shared-fonts' / name)
    if args.app:
        write_app_entry(bundle, digest(args.native))
    else:
        launch = output / 'Play TMNT.command'
        launch.write_text('#!/bin/sh\nset -eu\n'
        'cd "$(dirname "$0")"\n'
        'export TOTK_ROMFS="$PWD/romfs"\n'
        'export TOTK_PROFILE_DIR="$PWD/user-data"\n'
        'export TOTK_SHARED_FONTS="$PWD/shared-fonts"\n'
        'export TOTK_SHADER_CACHE="$PWD/user-data/shader-cache"\n'
        'export TOTK_SHADER_SEED=off\n'
        'export TOTK_SHADER_SERVICE=native\n'
        'export TOTK_ROMFS_MODS=0\n'
        'export TOTK_ROMFS_PATCHES=0\n'
        'export TOTK_GPU_ALLOW_SUBMIT=1\n'
        'export TMNT_SPLIT_RECORDING=1\n'
        'export TMNT_DRAW_TEXTURE_INVERT=1\n'
        'export TMNT_STRICT_FRAME_DEPENDENCIES=1\n'
            'exec "$PWD/build/tmnt-native" "$PWD/rewritten" boot\n')
        launch.chmod(launch.stat().st_mode | stat.S_IXUSR)
    (output / 'BUILD-MANIFEST.json').write_text(json.dumps({
        'private': True, 'playability': 'not yet validated', 'bundle': 'app' if args.app else 'directory',
        'runtime_sha256': digest(args.native),
        'shader_service_sha256': digest(args.shader_service),
        'spirv_cross_sha256': digest(args.spirv_cross),
        'romfs_sha256': romfs_manifest['romfs_sha256'],
        'index_sha256': index_hash, 'nro_sha256': nro_id,
        'romfs_linked': args.link_romfs, 'fonts': bool(args.fonts),
    }, indent=2) + '\n')
    return bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native', 'rewritten', 'nro-prepared', 'nro-rewritten', 'romfs',
                 'resources', 'shader-service', 'spirv-cross', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--fonts', type=Path)
    parser.add_argument('--link-romfs', action='store_true',
                        help='save disk space; resulting bundle depends on the original RomFS path')
    parser.add_argument('--app', action='store_true', help='create a private macOS .app bundle')
    args = parser.parse_args()
    for name in ('native', 'rewritten', 'nro_prepared', 'nro_rewritten', 'romfs',
                 'resources', 'shader_service', 'spirv_cross', 'fonts'):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    print(pack(args))


if __name__ == '__main__':
    main()
