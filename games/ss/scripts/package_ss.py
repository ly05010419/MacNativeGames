#!/usr/bin/env python3
"""Package the build as a signed app, only after every check passes. Never rewrites a patched binary."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

from verify_ss import verify_cut, verify_libraries, set_digest

GAME = Path(__file__).resolve().parents[1]
TITLE = '01002DA013484000'
PACKAGE = 'v65536'
APP_NAME = "The Legend of Zelda: Skyward Sword HD"
PROFILE_NAME = "The Legend of Zelda Skyward Sword HD"
RULES_OUTSIDE_TOOLS = set()   # no patches.py: the profile has no rewrite rules of its own


def verify_originals(build):
    pins = json.loads((GAME/'assets/expected-1.0.1.json').read_text()).get('observed')
    if not pins:
        raise ValueError('no observed pins: run scripts/pin_inputs.py first')
    for name, pin in pins['modules'].items():
        data = (build/f'extract/{PACKAGE}/ExeFS'/name).read_bytes()
        if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise ValueError(f'original ExeFS hash mismatch: {name}')
    return pins


def package_notices(resources):
    """Retain runtime copyright/license texts without bundling build tools."""
    paths = [GAME/'THIRD_PARTY_NOTICES.md', GAME/'third_party/COPIES.md', GAME/'third_party/nxnative/LICENSE', GAME/'runtime/gfx/nvn/include/nvn.h']
    paths += [p for p in (GAME/'third_party/licenses').rglob('*') if p.is_file()]
    paths += [p for p in (GAME/'runtime/third_party').rglob('*') if p.is_file() and (p.name.startswith('LICENSE') or p.name in ('PATENTS', 'README.md'))]
    for path in paths:
        destination = resources/path.relative_to(GAME)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)


def publish(partial, output):
    """Atomic publication that cannot replace an app created during the build."""
    rename = ctypes.CDLL(None, use_errno=True).renamex_np
    rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(os.fsencode(partial), os.fsencode(output), 4):  # RENAME_EXCL
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(output))


def verify_binary(binary):
    if subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip() != 'arm64':
        raise ValueError(f'expected an arm64 executable: {binary.name}')
    for line in subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]:
        path = line.strip().split(' (', 1)[0]
        if not path.startswith(('/System/Library/', '/usr/lib/')):
            raise ValueError(f'external runtime dependency: {path}')


def verify_seed(profile):
    """The warm-up seed the app ships is the repository's own: the staging copy never removes a file, so a seed left in the work folder could outlive its source."""
    seed = profile/'pso_manifest.tsv'
    if not seed.is_file() or seed.read_bytes() != (GAME/f'runtime/games/{TITLE}/pso_manifest.tsv').read_bytes():
        raise ValueError('warm-up seed differs from runtime/games')


def verify_rewrite(source, patched, pins):
    from inspect_inputs import sha256
    for line in (patched/'build_src.sha256').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        if rel.startswith(('/', '..')) and rel not in RULES_OUTSIDE_TOOLS:
            raise ValueError(f'rewrite rule outside this target: {rel}')
        if sha256(source/'dm/tools'/rel) != digest:
            raise ValueError(f'stale rewrite rule: {rel}')
    proof = json.loads((patched/'rewrite-info.json').read_text())
    for name, digest in proof['originals'].items():
        if digest != pins['modules'][name]['sha256']:
            raise ValueError(f'rewrite input mismatch: {name}')
    for name, digest in proof['outputs'].items():
        if sha256(patched/name) != digest:
            raise ValueError(f'rewrite output changed: {name}')
    return proof


def package(source, build, output, language, env):
    if output.suffix != '.app':
        raise ValueError('--output must end in .app')
    if output.exists():
        raise ValueError(f'output exists; choose a new path or move it aside: {output}')
    pins = verify_originals(build)
    from inspect_inputs import EXPECTED, EXTERNAL, inspect, sha256
    extract, patched = build/f'extract/{PACKAGE}', build/f'patched_{PACKAGE}'
    inspect(extract, EXPECTED)
    rewrite = verify_rewrite(source, patched, pins)
    cut = verify_cut(extract/'ExeFS/main', patched)
    profile = source/f'dm/games/{TITLE}'
    # Prevent packaging a cache from a different chain or profile.
    chain_tool = source/'dm/gfx/tools/dmsh_chain.py'
    chain = subprocess.check_output([sys.executable, str(chain_tool)], env=env, text=True).strip()
    inputs = subprocess.check_output([sys.executable, str(chain_tool), '--inputs'], env=env, text=True).strip()
    libraries = build/'shader/cache'/f'b-{chain}'
    stamp = dict(line.split('\t', 1) for line in (libraries/'inputs.tsv').read_text().splitlines() if '\t' in line and not line.startswith('#'))
    if stamp != {'game': TITLE, 'inputs': inputs}:
        raise ValueError('shader library stamp differs from the source profile')
    digest, keys = set_digest(profile/'shader_map.tsv')
    if digest != EXTERNAL['shader_stage_key_sha256'] or keys != EXTERNAL['shader_keys']:
        raise ValueError(f'shader map differs from nxnative: {keys} keys')
    exceptions = json.loads((GAME/'assets/shader-exceptions.json').read_text())['shaders']
    shader_stats = verify_libraries(libraries, profile/'shader_map.tsv', exceptions, extract/'RomFS')
    required = [build/'tools/dmhost', patched/'build_src.sha256', build/'font/nintendo_ext.ttf', profile/'c1_full.tsv', profile/'cb_extent.tsv']
    for path in required:
        if not path.is_file():
            raise ValueError(f'incomplete build: {path}')
    verify_binary(build/'tools/dmhost')
    for name in ('c1_full.tsv', 'cb_extent.tsv'):
        if not (profile/name).read_text().splitlines()[0].startswith(f'# chain b-{chain} inputs {inputs} '):
            raise ValueError(f'stale shader table: {name}')
    verify_seed(profile)
    partial = output.with_name(output.stem+'.partial.app')
    if partial.exists():
        raise ValueError(f'partial output exists; inspect or remove it first: {partial}')
    resources, macos = partial/'Contents/Resources/game', partial/'Contents/MacOS'
    resources.mkdir(parents=True)
    macos.mkdir(parents=True)
    for src, name in [(extract/'RomFS', 'RomFS'), (patched, 'patched'), (build/'font', 'font'), (profile, 'profile')]:
        shutil.copytree(src, resources/name)
    (resources/'shader-cache').mkdir()
    shutil.copytree(libraries, resources/'shader-cache'/libraries.name)
    shutil.copy2(build/'tools/dmhost', resources/'dmhost')
    (resources/'language.txt').write_text(language+'\n')
    package_notices(resources)
    with (partial/'Contents/Info.plist').open('wb') as stream:
        plistlib.dump({'CFBundleExecutable': 'SkywardSwordHD', 'CFBundleIdentifier': 'org.macnativegames.ss', 'CFBundleName': APP_NAME, 'CFBundleDisplayName': APP_NAME,
                       'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0.1', 'CFBundleVersion': '65536', 'LSMinimumSystemVersion': '26.0',
                       'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.adventure-games'}, stream)
    subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=26.0', '-O2', '-Wall', '-Wextra', '-Werror', str(GAME/'app/launcher.c'),
                    '-o', str(macos/'SkywardSwordHD')], check=True, env=env)
    verify_binary(macos/'SkywardSwordHD')
    # Sign the nested executable before recording its final content hash.
    subprocess.run(['codesign', '--force', '--sign', '-', str(resources/'dmhost')], check=True, env=env)
    report = {'game': pins, 'chain': chain, 'shader_inputs': inputs, 'language': language, 'host_sha256': sha256(resources/'dmhost'),
              'compiler': subprocess.check_output(['xcrun', '-sdk', 'macosx', 'metal', '--version'], text=True, env=env).strip(),
              'rewrite_rules': (patched/'build_src.sha256').read_text(), 'rewrite': rewrite, 'cuts': cut, 'shader_inventory': shader_stats,
              'repository_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=GAME, text=True, env=env).strip()}
    # Identify the actual isolated sources including uncommitted changes.
    report['source_files'] = {p.relative_to(GAME).as_posix(): sha256(p) for p in sorted(GAME.rglob('*')) if p.is_file() and '__pycache__' not in p.parts and p.name != '.DS_Store'}
    report['outputs'] = {p.relative_to(resources).as_posix(): sha256(p) for p in sorted(resources.rglob('*')) if p.is_file()}
    (resources/'build-info.json').write_text(json.dumps(report, indent=2)+'\n')
    subprocess.run(['codesign', '--force', '--sign', '-', str(partial)], check=True, env=env)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(partial)], check=True, env=env)
    publish(partial, output)
    print(f'Built {output}\nSave: ~/Library/Application Support/{PROFILE_NAME}/save')
