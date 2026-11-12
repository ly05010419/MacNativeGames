#!/usr/bin/env python3
"""Build a private TMNT Mac bundle from the player's verified extracted files.

Generated game bytes, tools, saves and logs remain outside the checkout. This
builds the host and tools; the separate boot/visible gameplay checks still
determine whether the resulting bundle works on a particular Mac.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from extract_exefs import validate_private_output
from expected_inputs import check_supported_romfs
from rewrite_modules import REPO

GAME = Path(__file__).resolve().parents[1]


def call(*argv):
    subprocess.run([str(a) for a in argv], check=True)


def script(name, *argv):
    call(sys.executable, GAME / 'scripts' / name, *argv)


def build(args):
    work, out = args.work.resolve(), args.out.resolve()
    for path in (work, out):
        validate_private_output(path)
        if path == REPO or path.is_relative_to(REPO):
            raise ValueError('private output must stay outside the source checkout')
        if path.exists():
            raise ValueError(f'choose a fresh output directory: {path}')
    if work == out or work.is_relative_to(out) or out.is_relative_to(work):
        raise ValueError('build work and bundle output must be separate')
    for path in (args.exefs, args.romfs, args.fonts):
        if not path.is_dir():
            raise ValueError(f'input directory missing: {path}')
    check_supported_romfs(json.loads((args.romfs / 'index.json').read_text())['romfs_sha256'])
    work.mkdir(parents=True)
    generated = work / 'build'
    generated.mkdir()
    rewritten = work / 'rewritten'
    prepared_nro = work / 'nro-prepared'
    rewritten_nro = work / 'nro-rewritten'
    resources = work / 'resources'
    shader_tools = work / 'shader-tools'

    # Each step validates the original-input identity and writes its report.
    script('rewrite_modules.py', '--exefs', args.exefs, '--out', rewritten)
    script('module_metadata.py', '--exefs', args.exefs, '--rewritten', rewritten,
           '--out', work / 'metadata.json')
    script('prepare_host_tables.py', '--exefs', args.exefs, '--rewritten', rewritten,
           '--metadata', work / 'metadata.json', '--out', generated / 'runtime_tables.h')
    script('prepare_resources.py', '--romfs', args.romfs, '--fonts', args.fonts,
           '--out', resources)
    for name in ('romfs_config.h', 'shared_fonts_config.h'):
        shutil.copy2(resources / name, generated / name)
    script('prepare_nro.py', '--romfs', args.romfs, '--out', prepared_nro)
    script('rewrite_nro.py', '--prepared', prepared_nro, '--out', rewritten_nro)
    script('prepare_nro_digest.py', '--prepared', prepared_nro,
           '--rewritten', rewritten_nro, '--out', generated / 'tmnt_nro_digest.h')
    script('build_shader_tools.py', '--out', shader_tools, '--dotnet', args.dotnet)
    call('make', '-C', GAME, 'runtime', f'WORK={work}', f'PY={sys.executable}')
    script('package_private.py', '--native', generated / 'tmnt-native',
           '--rewritten', rewritten, '--nro-prepared', prepared_nro,
           '--nro-rewritten', rewritten_nro, '--romfs', args.romfs,
           '--resources', resources, '--fonts', args.fonts,
           '--shader-service', shader_tools / 'shader-service/totk_xlate',
           '--spirv-cross', shader_tools / 'spirv-cross/spirv-cross',
           '--out', out, *(['--link-romfs'] if args.link_romfs else []),
           *(['--app'] if args.app else []))
    root = out / 'Contents/Resources/game' if args.app else out
    call(root / 'build/tmnt-native', root / 'rewritten', 'link')
    print(f'Private bundle built and linked: {out}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exefs', 'romfs', 'fonts', 'work', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--dotnet', default='dotnet',
                        help='path to a .NET 10 SDK executable')
    parser.add_argument('--link-romfs', action='store_true',
                        help='save disk space; the bundle then depends on the input RomFS path')
    parser.add_argument('--app', action='store_true', help='build a private macOS .app bundle')
    args = parser.parse_args()
    build(args)


if __name__ == '__main__':
    main()
