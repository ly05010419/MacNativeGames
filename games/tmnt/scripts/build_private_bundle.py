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
from rewrite_modules import REPO, sha256, verify_exefs

GAME = Path(__file__).resolve().parents[1]


def call(*argv):
    subprocess.run([str(a) for a in argv], check=True)


def script(name, *argv):
    call(sys.executable, GAME / 'scripts' / name, *argv)


def _build(args):
    work, out = args.work.resolve(), args.out.resolve()
    for path in (work, out):
        if path == REPO or path.is_relative_to(REPO):
            raise ValueError('private output must stay outside the source checkout')
    if work == out or work.is_relative_to(out) or out.is_relative_to(work):
        raise ValueError('build work and bundle output must be separate')
    fonts = ['--fonts', args.fonts] if args.fonts else []
    for path in (args.exefs, args.romfs, *([args.fonts] if args.fonts else [])):
        if not path.is_dir():
            raise ValueError(f'input directory missing: {path}')
    verify_exefs(args.exefs)
    check_supported_romfs(sha256(args.romfs / 'romfs.bin'))
    check_supported_romfs(json.loads((args.romfs / 'index.json').read_text())['romfs_sha256'])
    work.mkdir(parents=True, exist_ok=True)
    generated = work / 'build'
    generated.mkdir(exist_ok=True)
    rewritten = work / 'rewritten'
    prepared_nro = work / 'nro-prepared'
    rewritten_nro = work / 'nro-rewritten'
    resources = work / 'resources'
    shader_tools = work / 'shader-tools'

    from tmnt_work import Steps, inventory, source_identity
    steps = Steps(work)
    sources = source_identity(GAME)
    def cached(name, inputs, outputs, action, ignore=()):
        return steps.run(name, {'sources': sources, 'inputs': inputs}, outputs, action, ignore)
    cached('rewrite-modules', inventory(args.exefs), [rewritten],
           lambda: script('rewrite_modules.py', '--exefs', args.exefs, '--out', rewritten), ('nro',))
    cached('metadata', inventory(rewritten, ('nro',)), [work / 'metadata.json'],
           lambda: script('module_metadata.py', '--exefs', args.exefs, '--rewritten', rewritten,
                          '--out', work / 'metadata.json'))
    cached('host-tables', inventory(work / 'metadata.json'), [generated / 'runtime_tables.h'],
           lambda: script('prepare_host_tables.py', '--exefs', args.exefs, '--rewritten', rewritten,
                          '--metadata', work / 'metadata.json', '--out', generated / 'runtime_tables.h'))
    cached('resources', [inventory(args.romfs), inventory(args.fonts) if args.fonts else None], [resources],
           lambda: script('prepare_resources.py', '--romfs', args.romfs, *fonts, '--out', resources))
    for name in ('romfs_config.h', 'shared_fonts_config.h'):
        shutil.copy2(resources / name, generated / name)
    cached('prepare-nro', inventory(args.romfs), [prepared_nro],
           lambda: script('prepare_nro.py', '--romfs', args.romfs, '--out', prepared_nro))
    cached('rewrite-nro', inventory(prepared_nro), [rewritten_nro],
           lambda: script('rewrite_nro.py', '--prepared', prepared_nro, '--out', rewritten_nro))
    cached('nro-digest', [inventory(prepared_nro), inventory(rewritten_nro)], [generated / 'tmnt_nro_digest.h'],
           lambda: script('prepare_nro_digest.py', '--prepared', prepared_nro,
                          '--rewritten', rewritten_nro, '--out', generated / 'tmnt_nro_digest.h'))
    cached('shader-tools', {'dotnet': args.dotnet}, [shader_tools],
           lambda: script('build_shader_tools.py', '--out', shader_tools,
                          *(['--dotnet', args.dotnet] if args.dotnet else [])))
    call('make', '-C', GAME, 'runtime', f'WORK={work}', f'PY={sys.executable}')
    # The loader checks the registered NRO even in link-only mode. Stage it
    # beside the modules before validation, rather than relying on packaging.
    nro_stage = rewritten / 'nro'
    nro_stage.mkdir(exist_ok=True)
    nro_id = sha256(prepared_nro / 'module.nro')
    for suffix in ('patched.bin', 'fixups.tsv', 'gates.tsv'):
        shutil.copy2(rewritten_nro / f'nro.{suffix}', nro_stage / f'{nro_id}.{suffix}')
    # Link validation runs in the work tree, before an App is assembled.
    call(generated / 'tmnt-native', rewritten, 'link')
    script('package_private.py', '--native', generated / 'tmnt-native',
           '--rewritten', rewritten, '--nro-prepared', prepared_nro,
           '--nro-rewritten', rewritten_nro, '--romfs', args.romfs,
           '--resources', resources, *fonts,
           '--shader-service', shader_tools / 'shader-service/totk_xlate',
           '--spirv-cross', shader_tools / 'spirv-cross/spirv-cross',
           '--out', out, '--work', getattr(args, 'record_work', None) or work, *(['--link-romfs'] if args.link_romfs else []),
           *(['--app'] if args.app else []))
    print(f'Private bundle built and linked: {out}')


def build(args):
    from tmnt_work import build_lock, protect_inputs
    work = args.work.resolve()
    if work == REPO or work.is_relative_to(REPO):
        raise ValueError('private output must stay outside the source checkout')
    protect_inputs([args.exefs, args.romfs, args.fonts], [work, args.out])
    with build_lock(work):
        return _build(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exefs', 'romfs', 'work', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--fonts', type=Path, help='optional verified shared-font directory')
    parser.add_argument('--record-work', type=Path, help='common work root recorded by the container builder')
    parser.add_argument('--dotnet',
                        help='path to a .NET 10 SDK executable')
    parser.add_argument('--link-romfs', action='store_true',
                        help='save disk space; the bundle then depends on the input RomFS path')
    parser.add_argument('--app', action='store_true', help='build a private macOS .app bundle')
    args = parser.parse_args()
    build(args)


if __name__ == '__main__':
    main()
