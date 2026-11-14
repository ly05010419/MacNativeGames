#!/usr/bin/env python3
"""Build the isolated Skyward Sword HD macOS target from a checked extraction (scripts/extract_ss.py makes one)."""
import argparse
import fcntl
from pathlib import Path
import shutil
import subprocess
import sys

from inspect_inputs import EXPECTED, inspect, write_report
from ss_env import environment as clean_environment
from package_ss import PACKAGE, TITLE, package
from verify_ss import verify_cut
from build_shader_tools import build_translator
from ss_bundle import check_output, adopt_inputs

GAME = Path(__file__).resolve().parents[1]


def copy_tree(source, destination):
    """Copy source files without removing generated tables in the build workspace."""
    for src in sorted(source.rglob('*')):
        if (not src.is_file() or any(part in ('__pycache__', 'gen', 'obj', 'bin') or part.endswith('.dSYM') for part in src.relative_to(source).parts)):
            continue
        dst = destination/src.relative_to(source)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.is_file() or src.read_bytes() != dst.read_bytes():
            shutil.copy2(src, dst)


def stage(work, env=None, *, translator=True):
    """Keep all generated code, tables and caches outside the checkout. The shader translator is built from
    third_party/nxnative into the staged tree (scripts/build_shader_tools.py), or reused when nothing changed."""
    source = work/'source'
    # Makefile wildcards must not compile a file left by another branch. Generated
    # tables, embedded headers and translator outputs remain in their own folders.
    for path in (source/'dm').rglob('*'):
        rel = path.relative_to(source/'dm')
        if any(p in ('gen', 'obj', 'bin', '__pycache__', 'dmsh_xlate_bin') for p in rel.parts):
            continue
        if path.is_file() and path.suffix in ('.c', '.m', '.h', '.S', '.inc', '.py', '.cs') and not (GAME/'runtime'/rel).is_file():
            print(f'sources: removing {rel}, no longer in this checkout', flush=True)
            path.unlink()
    copy_tree(GAME/'runtime', source/'dm')
    copy_tree(GAME/'assets', source/'assets')
    target = source/'scripts/build_ipa_from_nsp.py'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GAME/'third_party/nxnative/build_ipa_from_nsp.py', target)
    shutil.copy2(GAME/'scripts/inspect_inputs.py', source/'scripts/inspect_inputs.py')
    if translator:
        build_translator(source, env or environment(work))
    return source


def environment(work):
    """ss_env's: zstd, spirv-cross, the .NET SDK, libvpx and dav1d from MacNativeGames' tools/deps."""
    return clean_environment(work, TITLE)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, help='extracted root containing ExeFS/ and RomFS/; no keys needed')
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--output', type=Path, help='new .app output path (required for package/all)')
    parser.add_argument('--phase', choices=('inspect', 'rewrite', 'host', 'shaders', 'package', 'all'), default='all')
    parser.add_argument('--language', default='en-US', choices=('en-US', 'en-GB', 'ja', 'fr', 'de', 'es-419', 'es', 'it', 'nl', 'fr-CA', 'ru', 'ko', 'zh-Hant', 'zh-Hans', 'pt-BR'))
    args = parser.parse_args(argv)
    work = args.work_dir.expanduser().resolve()
    original = args.inputs.expanduser().resolve()
    output = args.output.expanduser().resolve() if args.output else None
    checkout = GAME.parents[1]
    for path in (work, output):
        managed_input = path == work and original == work/'inputs'
        if path and (path == checkout or checkout in path.parents or path == original or original in path.parents or (path in original.parents and not managed_input)):
            parser.error('work/output must be outside the checkout and original inputs')
    if work in checkout.parents:
        parser.error('work must not contain the checkout')
    if args.phase in ('all', 'package') and not output:
        parser.error('--output is required for package/all')
    if output:
        check_output(output)
        if work == output or output in work.parents:
            parser.error('--output must be outside the work directory ancestry')
    work.mkdir(parents=True, exist_ok=True)
    with (work/'build.lock').open('a') as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise ValueError('another build owns this work directory')
        report = inspect(original, EXPECTED)
        write_report(report, work/'input-report.json')
        print(f"Verified {report['romfs']['files']} RomFS files", flush=True)
        if args.phase == 'inspect': return
        source = stage(work)
        build = work/'build'
        destination = build/f'extract/{PACKAGE}'
        # APFS clones preserve originals and avoid hard links to writable game files.
        if not destination.exists():
            partial = destination.with_name(f'{PACKAGE}.partial')
            if partial.exists(): raise ValueError(f'inspect/remove interrupted input copy: {partial}')
            partial.mkdir(parents=True)
            for name in ('ExeFS', 'RomFS'):
                subprocess.run(['cp', '-cR', str(original/name), str(partial/name)], check=True)
            inspect(partial, EXPECTED)
            partial.rename(destination)
        else:
            inspect(destination, EXPECTED)
        env = environment(work)
        if args.phase != 'package':
            subprocess.run([sys.executable, str(source/'scripts/build_ipa_from_nsp.py'), '--extracted', str(destination), '--work-dir', str(build),
                            '--phase', args.phase, '--platform', 'mac', '--package', PACKAGE, '--language', args.language, '--progress', 'lines'], env=env, check=True)
        if args.phase in ('all', 'rewrite'):
            verify_cut(destination/'ExeFS/main', build/f'patched_{PACKAGE}')
        if args.phase in ('all', 'package'):
            package(source, build, output, args.language, env)


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error: sys.exit(f'error: {error}')
