#!/usr/bin/env python3
"""Build the isolated Worldend Syndrome macOS target from original local inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
from worldend_bundle import Digests, check_output, signed_bundle, link_data, publish, record_update_source, clone
from worldend_pipelines import mac_pipelines, check_archive_lists

GAME = Path(__file__).resolve().parents[1]
TITLE = '010048900CF64000'
APP_NAME = 'Worldend Syndrome'
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


def prepare_offline_dependencies(work, package=None):
    """Accept an explicit local prerequisite; never download or use a global cache."""
    if package is None:
        from tools.deps import package as dependency
        package = dependency('libhac')
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
    return source


def link_inputs(inputs, work):
    destination = work / 'input'
    destination.mkdir(parents=True, exist_ok=True)
    wanted = {p.name: p.resolve() for p in inputs.iterdir() if p.suffix.lower() in ('.nsp', '.xci')}
    if not wanted:
        raise ValueError('input folder contains no .nsp or .xci files')
    for old in destination.iterdir():
        if old.is_symlink() and old.suffix.lower() in ('.nsp', '.xci') and old.name not in wanted:
            old.unlink()
    for name, src in wanted.items():
        dst = destination / name
        if dst.is_symlink() and dst.resolve() == src:
            continue
        if dst.exists() or dst.is_symlink():
            raise ValueError(f'refusing to replace an existing input: {dst}')
        dst.symlink_to(src)
    return destination


def verify_originals(build):
    pins = json.loads((GAME / 'assets/expected-1.0.0.json').read_text())
    for name, pin in pins['modules'].items():
        data = (build / 'extract/v0/ExeFS' / name).read_bytes()
        if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise ValueError(f'original ExeFS hash mismatch: {name}')
    return pins


def translation_files():
    """Only translator/tool/profile inputs; runtime code and warm lists do not invalidate libraries."""
    files = list((GAME/'runtime/gfx/tools').rglob('*')) + list((GAME/f'runtime/games/{TITLE}').glob('*'))
    files += list((GAME/'prebuilt/macos-arm64').rglob('*'))
    return {p.relative_to(GAME).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)
            if p.is_file() and '__pycache__' not in p.parts and p.name not in ('pso_manifest.tsv', 'pso_title.tsv')}


def package(source, build, output, language, env=None):
    """Verify the game's identity, reuse sealed data, and publish in place."""
    check_output(output)
    pins = verify_originals(build)
    env = env or dict(os.environ, DM_GAME=TITLE)
    profile = source / f'dm/games/{TITLE}'
    chain_tool = source / 'dm/gfx/tools/dmsh_chain.py'
    chain = subprocess.check_output([sys.executable, str(chain_tool)], env=env, text=True).strip()
    inputs = subprocess.check_output([sys.executable, str(chain_tool), '--inputs'], env=env, text=True).strip()
    libraries = build / 'shader/cache' / f'b-{chain}'
    stamp = dict(line.split('\t', 1) for line in (libraries / 'inputs.tsv').read_text().splitlines() if '\t' in line and not line.startswith('#'))
    if stamp != {'game': TITLE, 'inputs': inputs}:
        raise ValueError('shader library stamp differs from the source profile')
    required = [build/'tools/dmhost', build/'patched_v0/build_src.sha256', build/'font/nintendo_ext.ttf',
                profile/'c1_full.tsv', profile/'cb_extent.tsv']
    for path in required:
        if not path.is_file():
            raise ValueError(f'incomplete build: {path}')
    for name in ('c1_full.tsv', 'cb_extent.tsv'):
        if not (profile/name).read_text().splitlines()[0].startswith(f'# chain b-{chain} inputs {inputs} '):
            raise ValueError(f'stale shader table: {name}')
    # The generated profile is the exact build/ directory the runtime will read.
    # Include every translated library; measured R/C rows remain unchanged.
    rows = set(line for line in (GAME/f'runtime/games/{TITLE}/pso_manifest.tsv').read_text().splitlines()
               if line and not line.startswith('#'))
    for line in (profile/'shader_map.tsv').read_text().splitlines():
        if line and not line.startswith(('#', 'package')):
            fields = line.split('\t')
            stage = {'vertex': 0, 'pixel': 1, 'geometry': 2, 'compute': 5}[fields[2]]
            rows.add(f'S\t{fields[3]}_b\t{stage}')
    (profile/'pso_manifest.tsv').write_text('# Worldend: libraries and recorded pipeline descriptors\n'+'\n'.join(sorted(rows))+'\n')
    archive = mac_pipelines(build/'tools/dmhost', profile, libraries, build, env)
    digests = Digests(build/'verified-digests.json')
    partial = output.with_name(output.stem+'.partial.app')
    if partial.exists():
        raise ValueError(f'partial output exists; inspect it first: {partial}')
    resources, macos = partial/'Contents/Resources/game', partial/'Contents/MacOS'
    resources.mkdir(parents=True); macos.mkdir(parents=True)
    data = signed_bundle(partial, output, 'GameData', {'original': build/'extract/v0', 'patched': build/'patched_v0', 'font': build/'font'}, digests, env)
    link_data(resources, 'RomFS', data, 'original/RomFS')
    for name in ('patched', 'font'):
        link_data(resources, name, data)
    shaders = signed_bundle(partial, output, 'ShaderData', {libraries.name: libraries}, digests, env)
    link_data(resources, 'shader-cache', shaders, '.')
    shutil.copytree(profile, resources/'build')
    (resources/'profile').symlink_to('build')
    shutil.copytree(archive, resources/'pipeline-archive')
    check_archive_lists(resources/'pipeline-archive', resources/'build')
    clone(build/'tools/dmhost', resources/'dmhost')
    (resources/'language.txt').write_text(language+'\n')
    info = {'CFBundleExecutable': 'WorldendSyndrome', 'CFBundleIdentifier': 'org.macnativegames.worldend',
            'CFBundleName': APP_NAME, 'CFBundleDisplayName': APP_NAME, 'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': '1.0.0', 'CFBundleVersion': '0', 'LSMinimumSystemVersion': '26.0',
            'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.games'}
    record_update_source(info, build.parent.parent)
    (partial/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    launcher = build/'tools/WorldendSyndrome'
    subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(GAME/'app/launcher.c'), '-o', str(launcher)], check=True, env=env)
    clone(launcher, macos/'WorldendSyndrome')
    subprocess.run(['codesign', '--force', '--sign', '-', str(resources/'dmhost')], check=True, env=env)
    report = {'game': pins, 'chain': chain, 'shader_inputs': inputs, 'language': language,
              'rewrite_rules': (build/'patched_v0/build_src.sha256').read_text(), 'translation_files': translation_files(),
              'patched': digests.files(build/'patched_v0'), 'host_sha256': digests.sha256(resources/'dmhost')}
    (resources/'build-info.json').write_text(json.dumps(report, indent=2)+'\n')
    shutil.copy2(GAME/'THIRD_PARTY_NOTICES.md', resources/'THIRD_PARTY_NOTICES.md')
    shutil.copytree(GAME/'third_party/licenses', resources/'third_party/licenses')
    subprocess.run(['codesign', '--force', '--sign', '-', str(partial)], check=True, env=env)
    subprocess.run(['codesign', '--verify', '--strict', str(partial)], check=True, env=env)
    publish(partial, output); digests.save()
    print(f'Built {output}')


def environment(work):
    from worldend_env import environment as clean_environment
    return clean_environment(work, TITLE)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, help='folder with the base v0 NSP or XCI')
    parser.add_argument('--keys', required=True, type=Path, help='your local prod.keys (never copied into the app)')
    parser.add_argument('--libhac-package', type=Path,
                        help='optional pinned LibHac package; otherwise tools/deps supplies it')
    parser.add_argument('--work-dir', required=True, type=Path, help='private build directory outside the checkout')
    parser.add_argument('--output', required=True, type=Path, help='new .app output path')
    parser.add_argument('--language', default='en-US', choices=('en-US', 'en-GB'), help='game language: en-US or en-GB')
    args = parser.parse_args(argv)
    work, output = args.work_dir.expanduser().resolve(), args.output.expanduser().resolve()
    check_output(output)
    checkout = GAME.parents[1]
    if any(path == checkout or checkout in path.parents for path in (work, output)):
        parser.error('--work-dir and --output must be outside the checkout to keep game content out of Git')
    if not args.inputs.expanduser().is_dir() or not args.keys.expanduser().is_file():
        parser.error('inputs folder and keys must exist')
    work.mkdir(parents=True, exist_ok=True)
    from worldend_python import build_python
    python = build_python(work)
    if Path(sys.prefix).resolve() != (work/'venv').resolve():
        subprocess.run([str(python), str(__file__), *(argv if argv is not None else sys.argv[1:])], check=True)
        return
    prepare_offline_dependencies(work, args.libhac_package)
    source = stage(work)
    inputs = link_inputs(args.inputs.expanduser().resolve(), work)
    env = environment(work)
    from worldend_bundle import adopt_inputs
    adopt_inputs(output, inputs/f'{TITLE}_build', env)
    subprocess.run([sys.executable, str(source / 'scripts/build_ipa_from_nsp.py'), str(inputs),
                    '--keys', str(args.keys.expanduser().resolve()), '--package', 'v0',
                    '--platform', 'mac', '--language', args.language, '--progress', 'lines'], env=env, check=True)
    package(source, inputs / f'{TITLE}_build', output, args.language, env)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f'error: {error}')
