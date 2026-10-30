#!/usr/bin/env python3
"""Build the isolated AoC macOS target from original local inputs."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
TITLE = '01002B00111A2000'
APP_NAME = 'Hyrule Warriors: Age of Calamity'
PROFILE_NAME = 'Hyrule Warriors Age of Calamity'
LIBHAC_SHA256 = '1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19'


def copy_tree(source, destination):
    """Copy source files without removing generated tables in the build workspace."""
    for src in sorted(source.rglob('*')):
        if (not src.is_file() or any(part in ('__pycache__', 'gen', 'obj', 'bin')
                                     or part.endswith('.dSYM') for part in src.relative_to(source).parts)):
            continue
        dst = destination / src.relative_to(source)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.is_file() or src.read_bytes() != dst.read_bytes():
            shutil.copy2(src, dst)


def verify_prebuilt():
    root = GAME / 'prebuilt/macos-arm64'
    expected = set()
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        expected.add(rel)
        if hashlib.sha256((root / rel).read_bytes()).hexdigest() != digest:
            raise ValueError(f'shader translator checksum mismatch: {rel}')
    actual = {str(p.relative_to(root)) for p in (root / 'dmsh_xlate').rglob('*') if p.is_file()}
    if actual != expected:
        raise ValueError('shader translator file set differs from SHA256SUMS')


def prepare_offline_dependencies(work, package):
    """Accept an explicit local prerequisite; never download or use a global cache."""
    package = package.expanduser().resolve()
    if not package.is_file():
        raise ValueError('local LibHac 0.19.0 package is missing; see README.md')
    if hashlib.sha256(package.read_bytes()).hexdigest() != LIBHAC_SHA256:
        raise ValueError('LibHac 0.19.0 package SHA-256 mismatch')
    feed = work / 'dependencies/nuget'
    feed.mkdir(parents=True, exist_ok=True)
    shutil.copy2(package, feed / 'libhac.0.19.0.nupkg')
    return feed


def stage(work):
    """Keep all generated code, tables and caches outside the checkout."""
    source = work / 'source'
    verify_prebuilt()
    # Prune only obsolete managed tooling when upgrading an existing workspace.
    # Otherwise copy_tree would retain removed translator sources/resources.
    for rel in ('dm/third_party/ryujinx', 'dm/gfx/tools/dmsh_xlate', 'dm/gfx/tools/dmsh_xlate_bin'):
        old = source / rel
        if old.exists():
            shutil.rmtree(old)
    copy_tree(GAME / 'runtime', source / 'dm')
    copy_tree(GAME / 'prebuilt/macos-arm64/dmsh_xlate', source / 'dm/gfx/tools/dmsh_xlate_bin')
    copy_tree(GAME / 'assets', source / 'assets')
    target = source / 'scripts/build_ipa_from_nsp.py'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GAME / 'third_party/nxnative/build_ipa_from_nsp.py', target)
    shutil.copy2(GAME / 'scripts/inspect_inputs.py', source / 'scripts/inspect_inputs.py')
    return source


def verify_originals(build):
    pins = json.loads((GAME / 'assets/expected-1.3.0.json').read_text())
    for name, pin in pins['modules'].items():
        data = (build / 'extract/v327680/ExeFS' / name).read_bytes()
        if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise ValueError(f'original ExeFS hash mismatch: {name}')
    return pins


def package_notices(resources):
    """Retain runtime copyright/license texts without bundling build tools."""
    paths = [GAME / 'THIRD_PARTY_NOTICES.md', GAME / 'third_party/COPIES.md',
             GAME / 'third_party/nxnative/LICENSE', GAME / 'runtime/gfx/nvn/include/nvn.h']
    paths += [p for p in (GAME / 'third_party/licenses').rglob('*') if p.is_file()]
    paths += [p for p in (GAME / 'runtime/third_party').rglob('*')
              if p.is_file() and (p.name.startswith('LICENSE') or p.name in ('PATENTS', 'README.md'))]
    for path in paths:
        destination = resources / path.relative_to(GAME)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)


def package(source, build, output, language):
    """Package only after the source build succeeds. Never rewrite a patched binary."""
    if output.suffix != '.app':
        raise ValueError('--output must end in .app')
    if output.exists():
        raise ValueError(f'output exists; choose a new path or move it aside: {output}')
    pins = verify_originals(build)
    from inspect_inputs import inspect
    inspect(build / 'extract/v327680', GAME / 'assets/expected-1.3.0.json')
    rewrite = verify_rewrite(source, build / 'patched_v327680', pins)
    profile = source / f'dm/games/{TITLE}'
    # Prevent packaging a cache from a different chain or profile.
    env = dict(os.environ, DM_GAME=TITLE)
    chain_tool = source / 'dm/gfx/tools/dmsh_chain.py'
    chain = subprocess.check_output([sys.executable, str(chain_tool)], env=env, text=True).strip()
    inputs = subprocess.check_output([sys.executable, str(chain_tool), '--inputs'], env=env, text=True).strip()
    libraries = build / 'shader/cache' / f'b-{chain}'
    stamp = dict(line.split('\t', 1) for line in (libraries / 'inputs.tsv').read_text().splitlines() if '\t' in line and not line.startswith('#'))
    if stamp != {'game': TITLE, 'inputs': inputs}:
        raise ValueError('shader library stamp differs from the source profile')
    shader_stats = verify_libraries(libraries, profile / 'shader_map.tsv')
    required = [build / 'tools/dmhost', build / 'patched_v327680/build_src.sha256',
                build / 'font/nintendo_ext.ttf', profile / 'c1_full.tsv', profile / 'cb_extent.tsv']
    for path in required:
        if not path.is_file():
            raise ValueError(f'incomplete build: {path}')
    verify_binary(build/'tools/dmhost')
    for name in ('c1_full.tsv', 'cb_extent.tsv'):
        if not (profile / name).read_text().splitlines()[0].startswith(f'# chain b-{chain} inputs {inputs} '):
            raise ValueError(f'stale shader table: {name}')
    partial = output.with_name(output.stem + '.partial.app')
    if partial.exists():
        raise ValueError(f'partial output exists; inspect or remove it first: {partial}')
    resources = partial / 'Contents/Resources/game'
    macos = partial / 'Contents/MacOS'
    resources.mkdir(parents=True)
    macos.mkdir(parents=True)
    try:
        for src, name in [(build / 'extract/v327680/RomFS', 'RomFS'),
                          (build / 'patched_v327680', 'patched'), (build / 'font', 'font'),
                          (profile, 'profile')]:
            shutil.copytree(src, resources / name)
        (resources / 'shader-cache').mkdir()
        shutil.copytree(libraries, resources / 'shader-cache' / libraries.name)
        shutil.copy2(build / 'tools/dmhost', resources / 'dmhost')
        (resources / 'language.txt').write_text(language + '\n')
        package_notices(resources)
        with (partial / 'Contents/Info.plist').open('wb') as stream:
            plistlib.dump({'CFBundleExecutable': 'AgeOfCalamity', 'CFBundleIdentifier': 'org.macnativegames.aoc',
                          'CFBundleName': APP_NAME, 'CFBundleDisplayName': APP_NAME,
                          'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.3.0',
                          'CFBundleVersion': '327680', 'LSMinimumSystemVersion': '26.0',
                          'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.games'}, stream)
        subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-O2', '-Wall', '-Wextra', '-Werror', str(GAME / 'app/launcher.c'),
                        '-o', str(macos / 'AgeOfCalamity')], check=True)
        verify_binary(macos/'AgeOfCalamity')
        # Sign the nested executable before recording its final content hash.
        subprocess.run(['codesign', '--force', '--sign', '-', str(resources / 'dmhost')], check=True)
        # Preserve the rewriter's rule hashes; add portable build provenance for reviewers.
        report = {'game': pins, 'chain': chain, 'shader_inputs': inputs, 'language': language,
                  'host_sha256': hashlib.sha256((resources / 'dmhost').read_bytes()).hexdigest(),
                  'compiler': subprocess.check_output(['xcrun','-sdk','macosx','metal','--version'],text=True).strip(),
                  'rewrite_rules': (build / 'patched_v327680/build_src.sha256').read_text()}
        report['repository_head'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=GAME,text=True).strip()
        # Identify the actual isolated sources including uncommitted AoC changes.
        report['source_files'] = {p.relative_to(GAME).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in sorted(GAME.rglob('*')) if p.is_file()
                                  and '__pycache__' not in p.parts and p.name != '.DS_Store'}
        report['rewrite'] = rewrite
        report['shader_inventory'] = shader_stats
        from inspect_inputs import sha256
        report['outputs'] = {p.relative_to(resources).as_posix():sha256(p)
                             for p in sorted(resources.rglob('*')) if p.is_file()}
        (resources / 'build-info.json').write_text(json.dumps(report, indent=2) + '\n')
        # Sign nested Mach-O files explicitly, then the bundle, without touching game data.
        subprocess.run(['codesign', '--force', '--sign', '-', str(partial)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(partial)], check=True)
        publish(partial, output)
    except Exception:
        # Retain a failed package for inspection; never overwrite a working app.
        raise
    print(f'Built {output}\nSave: ~/Library/Application Support/{PROFILE_NAME}/save')


def publish(partial, output):
    """Atomic publication that cannot replace an app created during the build."""
    rename = ctypes.CDLL(None,use_errno=True).renamex_np
    rename.argtypes = [ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(os.fsencode(partial),os.fsencode(output),4):  # RENAME_EXCL
        error=ctypes.get_errno()
        raise OSError(error,os.strerror(error),str(output))


def verify_binary(binary):
    if subprocess.check_output(['lipo','-archs',str(binary)],text=True).strip() != 'arm64':
        raise ValueError(f'expected an arm64 executable: {binary.name}')
    dependencies=subprocess.check_output(['otool','-L',str(binary)],text=True).splitlines()[1:]
    for line in dependencies:
        path=line.strip().split(' (',1)[0]
        if not path.startswith(('/System/Library/','/usr/lib/')):
            raise ValueError(f'external runtime dependency: {path}')


def verify_rewrite(source, patched, pins):
    from inspect_inputs import sha256
    for line in (patched/'build_src.sha256').read_text().splitlines():
        digest, rel = line.split('  ',1)
        if sha256(source/'dm/tools'/rel) != digest:
            raise ValueError(f'stale rewrite rule: {rel}')
    proof = json.loads((patched/'rewrite-info.json').read_text())
    for name,digest in proof['originals'].items():
        if digest != pins['modules'][name]['sha256']:
            raise ValueError(f'rewrite input mismatch: {name}')
    for name,digest in proof['outputs'].items():
        if sha256(patched/name) != digest:
            raise ValueError(f'rewrite output changed: {name}')
    return proof


def verify_libraries(libraries, shader_map):
    """An offline package requires a real library and reflection for every key."""
    keys = {c[3] for line in shader_map.read_text().splitlines()
            if len(c := line.split('\t')) == 4 and c[0].endswith('.bin')}
    if not keys:
        raise ValueError('empty shader map')
    for library in libraries.glob('*.metallib'):
        with library.open('rb') as stream:
            if stream.read(4) != b'MTLB':
                raise ValueError(f'not an offline Metal library: {library.name}')
    folded = []
    for key in sorted(keys):
        if not (libraries / f'{key}_b.json').is_file():
            raise ValueError(f'incomplete offline shader key: {key}')
        reflection = json.loads((libraries/f'{key}_b.json').read_text())
        if reflection.get('chain') != libraries.name.removeprefix('b-') or reflection.get('dm_ctl') != key:
            raise ValueError(f'stale shader reflection: {key}')
        slot_kinds = {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf',
                      'ubo_ptr', 'sbuf_ubo', 'tex_ubo', 'samp_ubo', 'samp_id'}
        for binding in reflection.get('bindings', []):
            if binding.get('kind') not in slot_kinds | {'geometry', 'support', 'c1', 'drvcb', 'tex_c2a', 'samp_c2a'}:
                raise ValueError(f'unknown shader binding: {key} {binding.get("msl_name")}')
            if binding.get('kind') in slot_kinds and (type(binding.get('reg')) is not int or binding['reg'] < 0):
                raise ValueError(f'unresolved shader binding: {key} {binding.get("msl_name")}')
        if reflection.get('drv_unfillable'):
            raise ValueError(f'unfillable driver reads: {key}')
        if reflection.get('stage') == 'gs' and reflection.get('gs_slice'):
            # Exact layer-only geometry lowering runs in the paired vertex
            # library; it has no standalone Metal geometry function/library.
            layer = reflection['gs_slice']
            if layer.get('loc') not in range(32) or layer.get('comp') not in ('x','y','z','w'):
                raise ValueError(f'invalid folded geometry reflection: {key}')
            folded.append(key)
            continue
        if not (libraries / f'{key}_b.metallib').is_file():
            raise ValueError(f'incomplete offline shader key: {key}')
        for field,suffix in (('fetch','_bf'),('vin','_bv')):
            if reflection.get(field) and not (libraries/f'{key}{suffix}.metallib').is_file():
                raise ValueError(f'missing {field} library: {key}')
        if reflection.get('stage') == 'gs':
            for suffix in ('_bg0','_bg1','_bg2','_bg3','_bg4','_bgv'):
                if not (libraries/f'{key}{suffix}.metallib').is_file():
                    raise ValueError(f'missing geometry variant: {key}{suffix}')
    return {'keys':len(keys),'folded_layer_geometry':folded,
            'metallib_files':sum(1 for _ in libraries.glob('*.metallib'))}


def environment(work):
    env = dict(os.environ, DM_GAME=TITLE, DM_METAL_RUNTIME='0',
               TMPDIR=str(work / 'tmp'), NUGET_PACKAGES=str(work / 'cache/nuget'),
               DOTNET_CLI_HOME=str(work / 'cache/dotnet'), PYTHONDONTWRITEBYTECODE='1',
               DM_NUGET_SOURCE=str(work / 'dependencies/nuget'),
               DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
               DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true',
               MACOSX_DEPLOYMENT_TARGET='26.0')
    dotnet = work / 'dependencies/dotnet'
    if dotnet.is_dir():
        env['DOTNET_ROOT'] = str(dotnet)
        env['PATH'] = str(dotnet) + os.pathsep + env.get('PATH','/usr/bin:/bin')
    for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env


def main():
    import fcntl
    from inspect_inputs import inspect, write_report
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, help='extracted root containing ExeFS/ and RomFS/; no keys needed')
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--output', type=Path, help='new .app output path (required for package/all)')
    parser.add_argument('--phase', choices=('inspect','rewrite','host','shaders','package','all'), default='all')
    parser.add_argument('--language', default='en-US', choices=('en-US','en-GB','ja','fr','de','es-419','es','it','nl','fr-CA','ru','ko','zh-Hant','zh-Hans','pt-BR'))
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
    if args.phase in ('all','package') and not output:
        parser.error('--output is required for package/all')
    if output and (output.exists() or output.suffix != '.app' or work == output or output in work.parents):
        parser.error('--output must be a new .app path outside source inputs')
    work.mkdir(parents=True, exist_ok=True)
    with (work / 'build.lock').open('a') as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise ValueError('another build owns this work directory')
        report = inspect(original, GAME / 'assets/expected-1.3.0.json')
        write_report(report, work / 'input-report.json')
        print(f"Verified {report['romfs']['files']} RomFS files; container integrity unavailable", flush=True)
        if args.phase == 'inspect': return
        source = stage(work)
        build = work / 'build'
        destination = build / 'extract/v327680'
        # APFS clones preserve originals and avoid hard links to writable game files.
        if not destination.exists():
            partial = destination.with_name('v327680.partial')
            if partial.exists(): raise ValueError(f'inspect/remove interrupted input copy: {partial}')
            partial.mkdir(parents=True)
            for name in ('ExeFS','RomFS'):
                subprocess.run(['cp','-cR',str(original/name),str(partial/name)],check=True)
            inspect(partial, GAME / 'assets/expected-1.3.0.json')
            partial.rename(destination)
        else:
            inspect(destination, GAME / 'assets/expected-1.3.0.json')
        env = environment(work)
        if args.phase != 'package':
            subprocess.run([sys.executable, str(source/'scripts/build_ipa_from_nsp.py'),
                            '--extracted', str(destination), '--work-dir', str(build),
                            '--phase', args.phase, '--platform','mac','--package','v327680',
                            '--language',args.language,'--progress','lines'],env=env,check=True)
        if args.phase in ('all','package'):
            package(source,build,output,args.language)


if __name__ == '__main__':
    try: main()
    except (ValueError,OSError,subprocess.CalledProcessError) as error: sys.exit(f'error: {error}')
