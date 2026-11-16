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
import subprocess
import tempfile
from tmnt_bundle import Digests, check_output, signed_bundle, link_data, install_app, record_update_source
from pathlib import Path

from extract_exefs import validate_private_output
from expected_inputs import check_supported_romfs
from rewrite_modules import REPO
from rewrite_nro import verify_nro_report
from language_catalogue import language_patch

MODULES = ('main', 'subsdk0', 'subsdk2', 'subsdk1')
SUFFIXES = ('patched.bin', 'fixups.tsv', 'gates.tsv')

APP_ID = 'org.macnativegames.tmnt.private'


def write_app_entry(app, runtime_sha256):
    if len(runtime_sha256) != 64 or any(c not in '0123456789abcdef' for c in runtime_sha256):
        raise ValueError('invalid runtime SHA-256')
    contents = app / 'Contents'
    executable = contents / 'MacOS/PlayTMNT'
    executable.parent.mkdir(parents=True, exist_ok=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleDevelopmentRegion': 'en', 'CFBundleExecutable': 'PlayTMNT',
        'CFBundleIdentifier': APP_ID, 'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': "TMNT Shredder's Revenge", 'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': '1.0.0', 'CFBundleVersion': '1',
        'NSHighResolutionCapable': True,
    }))
    executable.write_text('#!/bin/sh\nset -eu\n'
        'CONTENTS=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\n'
        'ROOT="$CONTENTS/Resources/game"\n' +
        (Path(__file__).with_name('profile_launcher.sh').read_text()) + '\n' +
        'cd "$ROOT"\n'
        'export TOTK_ROMFS="$ROOT/romfs"\n'
        'export TOTK_PROFILE_DIR="$PROFILE"\n'
        'export TOTK_ROMFS_PROOF_DIR="$PROFILE"\n'
        'export TOTK_GAME_SLOT_DIR="$HOME/Library/Application Support/MacNativeGames/game-slot"\n'
        'export TOTK_SHARED_FONTS="$ROOT/shared-fonts"\n'
        'export TOTK_SHADER_CACHE="$PROFILE/shader-cache/tmnt-native-v1"\n'
        'export TMNT_PIPELINE_ARCHIVE="$ROOT/build/pipeline-archive"\n'
        'export DM_NVN_SHMAP="$ROOT/build/shader-map.tsv"\n'
        'export TOTK_SHADER_SEED=on TOTK_SHADER_SERVICE=native TOTK_WARM_BEFORE_TITLE=1\n'
        'export TOTK_ROMFS_MODS=0 TOTK_ROMFS_PATCHES="$ROOT/romfs-patches"\n'
        'export TOTK_GPU_ALLOW_SUBMIT=1 TMNT_SPLIT_RECORDING=1\n'
        'export TMNT_DRAW_TEXTURE_INVERT=1 TMNT_STRICT_FRAME_DEPENDENCIES=1\n'
        'exec "$ROOT/build/tmnt-native" "$ROOT/rewritten" boot\n')
    executable.chmod(executable.stat().st_mode | stat.S_IXUSR)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def assemble(args):
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
    patches = language_patch(args.romfs / 'romfs.bin', romfs_manifest['files'])
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
    (output / 'romfs-patches').mkdir()
    for name, data in patches.items():
        (output / 'romfs-patches' / name).write_bytes(data)
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
        'export TOTK_ROMFS_PATCHES="$PWD/romfs-patches"\n'
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
        'language_patches': {name: hashlib.sha256(data).hexdigest() for name, data in patches.items()},
        'romfs_linked': args.link_romfs, 'fonts': bool(args.fonts),
    }, indent=2) + '\n')
    return bundle


def pack(args):
    """Assemble off to the side, seal, then publish while retaining the installed App directory."""
    if not args.app:
        return assemble(args)
    output = args.out.resolve()
    check_output(output)
    work = getattr(args, 'work', None) or args.native.resolve().parent.parent
    work = Path(work).resolve()
    if output == REPO or output.is_relative_to(REPO) or work == REPO or work.is_relative_to(REPO):
        raise ValueError('private App and work must stay outside the checkout')
    from tmnt_shaders import prepare
    from tmnt_pipelines import mac_pipelines, check_archive_lists
    generated = args.native.parent
    prepare(args.romfs, args.rewritten, generated / 'shader-seed', args.shader_service, args.spirv_cross)
    mac_pipelines(args.native, args.rewritten, generated)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.tmnt-package-', dir=output.parent) as temporary:
        from argparse import Namespace
        partial = Path(temporary) / output.name
        staged = Namespace(**vars(args)); staged.out = partial
        assemble(staged)
        resources = partial / 'Contents/Resources/game'
        digests = Digests(work / 'package-digests.json')
        shader_bundle = signed_bundle(partial, output, 'ShaderData', {'shader-seed': generated / 'shader-seed'}, digests, os.environ)
        link_data(resources / 'build', 'shader-seed', shader_bundle)
        shutil.copytree(generated / 'pipeline-archive', resources / 'build/pipeline-archive')
        shutil.copy2(generated / 'pso_manifest.tsv', resources / 'build/pso_manifest.tsv')
        shutil.copy2(generated / 'shader-map.tsv', resources / 'build/shader-map.tsv')
        check_archive_lists(resources / 'build/pipeline-archive', resources / 'build')
        digests.save()
        # Keep the generated headers and rewrite reports needed to rebuild only the host.
        update = resources / 'runtime-update'
        update.mkdir()
        for name in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h', 'tmnt_nro_digest.h'):
            shutil.copy2(args.native.parent / name, update / name)
        shutil.copy2(args.rewritten / 'report.json', update / 'rewrite-report.json')
        identity = {str(p.relative_to(resources)): digest(p) for p in sorted((resources / 'rewritten').rglob('*')) if p.is_file()}
        identity.update({str(p.relative_to(resources)): digest(p) for p in update.iterdir() if p.is_file()})
        (update / 'inputs.json').write_text(json.dumps(identity, sort_keys=True) + '\n')
        info_path = partial / 'Contents/Info.plist'
        info = plistlib.loads(info_path.read_bytes()); record_update_source(info, work)
        info_path.write_bytes(plistlib.dumps(info))
        notices = resources / 'licenses'; notices.mkdir()
        for source in (REPO / 'LICENSE', REPO / 'THIRD_PARTY_NOTICES.md', GAME / 'third_party/COPIES.md', GAME / 'tools/totk_xlate/LICENSE', GAME / 'third_party/d2r/LICENSE'):
            if source.is_file(): shutil.copy2(source, notices / (source.parent.name + '-' + source.name))
        for source in sorted((GAME / 'third_party').rglob('*')):
            if source.is_file() and source.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
                relative = source.relative_to(GAME / 'third_party')
                target = notices / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        from tools.deps import source as dependency_source
        for dependency in ('opus', 'libvpx'):
            dependency_root = dependency_source(dependency)
            for name in ('COPYING', 'LICENSE', 'PATENTS'):
                if (dependency_root / name).is_file(): shutil.copy2(dependency_root / name, notices / (dependency + '-' + name))
        from tools.deps import spirv_cross_source
        spv = spirv_cross_source()
        shutil.copy2(spv / 'LICENSE', notices / 'SPIRV-Cross-LICENSE')
        if (spv / 'LICENSES').is_dir(): shutil.copytree(spv / 'LICENSES', notices / 'SPIRV-Cross-LICENSES')
        for binary in (resources / 'build/tmnt-native', resources / 'build/shader-service/totk_xlate', resources / 'build/spirv-cross/spirv-cross'):
            subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
        manifest_path = resources / 'BUILD-MANIFEST.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['runtime_sha256'] = digest(resources / 'build/tmnt-native')
        manifest_path.write_text(json.dumps(manifest, indent=2)+'\n')
        subprocess.run(['codesign', '--force', '--sign', '-', str(partial)], check=True)
        from build_app import wait_until_closed
        wait_until_closed(output)
        install_app(partial, output)
    return output


GAME = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('native', 'rewritten', 'nro-prepared', 'nro-rewritten', 'romfs',
                 'resources', 'shader-service', 'spirv-cross', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--work', type=Path, help='recorded build work directory')
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
