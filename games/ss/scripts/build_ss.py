#!/usr/bin/env python3
"""Build the isolated Skyward Sword HD macOS target from a checked extraction (scripts/extract_ss.py makes one)."""
import argparse
import fcntl
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

from inspect_inputs import EXPECTED, inspect, write_report
from ss_env import environment as clean_environment
from package_ss import PACKAGE, TITLE, package
from verify_ss import verify_cut

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


def verify_prebuilt():
    root = GAME/'prebuilt/macos-arm64'
    expected = set()
    for line in (root/'SHA256SUMS').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        expected.add(rel)
        if hashlib.sha256((root/rel).read_bytes()).hexdigest() != digest:
            raise ValueError(f'shader translator checksum mismatch: {rel}')
    actual = {str(p.relative_to(root)) for d in ('dmsh_xlate', 'spirv-cross') for p in (root/d).rglob('*') if p.is_file()}
    if actual != expected:
        raise ValueError('prebuilt tool file set differs from SHA256SUMS')


def stage(work):
    """Keep all generated code, tables and caches outside the checkout."""
    source = work/'source'
    verify_prebuilt()
    copy_tree(GAME/'runtime', source/'dm')
    copy_tree(GAME/'prebuilt/macos-arm64/dmsh_xlate', source/'dm/gfx/tools/dmsh_xlate_bin')
    copy_tree(GAME/'prebuilt/macos-arm64/spirv-cross', source/'dm/gfx/tools/spirv_cross_bin')   # the build nxnative's lock records (dmsh_chain.SPIRV_CROSS)
    copy_tree(GAME/'assets', source/'assets')
    target = source/'scripts/build_ipa_from_nsp.py'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GAME/'third_party/nxnative/build_ipa_from_nsp.py', target)
    shutil.copy2(GAME/'scripts/inspect_inputs.py', source/'scripts/inspect_inputs.py')
    return source


def environment(work):
    env = clean_environment(work, TITLE)
    dotnet = work/'dependencies/dotnet'
    if dotnet.is_dir():
        env['DOTNET_ROOT'] = str(dotnet)
        env['PATH'] = str(dotnet)+':'+env['PATH']
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, help='extracted root containing ExeFS/ and RomFS/; no keys needed')
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--output', type=Path, help='new .app output path (required for package/all)')
    parser.add_argument('--phase', choices=('inspect', 'rewrite', 'host', 'shaders', 'package', 'all'), default='all')
    parser.add_argument('--language', default='en-US', choices=('en-US', 'en-GB', 'ja', 'fr', 'de', 'es-419', 'es', 'it', 'nl', 'fr-CA', 'ru', 'ko', 'zh-Hant', 'zh-Hans', 'pt-BR'))
    args = parser.parse_args()
    work = args.work_dir.expanduser().resolve()
    original = args.inputs.expanduser().resolve()
    output = args.output.expanduser().resolve() if args.output else None
    checkout = GAME.parents[1]
    for path in (work, output):
        if path and (path == checkout or checkout in path.parents or path == original or original in path.parents or path in original.parents):
            parser.error('work/output must be outside the checkout and original inputs')
    if work in checkout.parents:
        parser.error('work must not contain the checkout')
    if args.phase in ('all', 'package') and not output:
        parser.error('--output is required for package/all')
    if output and (output.exists() or output.suffix != '.app' or work == output or output in work.parents):
        parser.error('--output must be a new .app path outside source inputs')
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
