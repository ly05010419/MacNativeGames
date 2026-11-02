#!/usr/bin/env python3
"""CI build of the production iOS project with synthetic embedded modules.

Requires Xcode and XcodeGen; GAME (or DM_GAME) selects the host profile.
No game files, keys, shaders, signing or device are needed. The resulting app
is a compile/link fixture, not a runnable game. All Xcode output stays under
--output, which must name a new directory; keep it on a scratch drive locally.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new scratch directory for inputs and build products')
    args = parser.parse_args()
    profile = game.load(os.environ.get('GAME') or os.environ.get('DM_GAME'))
    root = args.output.resolve()
    root.mkdir(parents=True)  # Never overwrite an existing project or build.
    ios = REPO / 'dm/ios'
    project = root / 'project'
    gen = project / 'Gen'
    tables = gen / 'dm'
    tables.mkdir(parents=True)
    for name in ('Sources', 'tools'):
        (project / name).symlink_to(ios / name, target_is_directory=True)
    shutil.copyfile(ios / 'project.yml', project / 'project.yml')
    assets = gen / 'Assets.xcassets'
    assets.mkdir()
    (assets / 'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))

    # One original text page, two writable pages (including the key page),
    # and one patch page exercise the production Mach-O segment/link flags.
    patched = root / 'patched'
    patched.mkdir()
    modules = profile.words('modules')
    for name in modules:
        (patched / f'{name}.patched.bin').write_bytes(bytes(0x10000))
        fixups = patched / f'{name}.fixups.tsv'
        fixups.write_text('# TEXT_END 0x4000\n# PATCH_OFF 0xc000\n'
                          '# KEY_PAGE_OFF 0x8000\n# KEY_PAGE_SIZE 0x4000\n')
        shutil.copyfile(fixups, tables / fixups.name)

    env = dict(os.environ, GAME=profile.id, DM_GAME=profile.id,
               DM_BUNDLE_ID='org.nxnative.ci', DM_DISPLAY_NAME='CI build fixture',
               DM_APP_VERSION='1.0', DM_APP_CATEGORY=profile.get('app_category'),
               DM_TEAM_ID='', DM_SCHEME_NAME='DmIOS', DM_APPICON='',
               DM_SHOW_FPS='NO', DM_LANG='en-US')

    def run(*command, capture=False):
        return subprocess.run([str(c) for c in command], cwd=project, env=env,
                              check=True, text=True, stdout=subprocess.PIPE if capture else None)

    config = run(sys.executable, ios / 'tools/gen_build.py', REPO / 'dm/host', gen, capture=True).stdout
    flags = run(sys.executable, ios / 'tools/gen_embed.py', patched, gen / 'embed.S', *modules, capture=True).stdout.strip()
    (gen / 'dm.xcconfig').write_text(config + f'OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS) {flags}\n')
    run('xcodegen', 'generate')

    # Exercise the real resource-copy phase with tiny, clearly synthetic data.
    romfs = root / 'romfs'
    shaders = root / 'shaders'
    romfs.mkdir()
    shaders.mkdir()
    marker = 'ci-fixture.txt'
    (romfs / marker).write_text('Synthetic CI resource; no game data.\n')
    (shaders / marker).write_text('Synthetic CI resource; no shader library.\n')
    chain_h = REPO / 'dm/gfx/nvn/dmsh_chain.h'
    chain = next(line.split('"')[1] for line in chain_h.read_text().splitlines()
                 if line.startswith('#define DMSH_CHAIN "'))
    derived = root / 'derived'
    run(sys.executable, ios / 'tools/gen_resources.py', project, romfs, marker,
        shaders, f'b-{chain}', '', derived, chain_h)
    run('xcodebuild', '-project', project / 'DmIOS.xcodeproj', '-scheme', 'DmIOS',
        '-configuration', 'Release', '-destination', 'generic/platform=iOS',
        '-derivedDataPath', derived, '-jobs', '3',
        'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY=',
        f'DM_XCODE_OUTPUT_ROOT={derived}', f'SYMROOT={derived}/Build/Products',
        f'OBJROOT={derived}/Build/Intermediates.noindex', 'build')

    app = derived / 'Build/Products/Release-iphoneos/DmIOS.app'
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    executable = app / info['CFBundleExecutable']
    if not executable.is_file():
        raise RuntimeError(f'No linked executable: {executable}')
    for source, packed in ((romfs / marker, app / 'romfs' / marker),
                           (shaders / marker, app / 'msl' / f'b-{chain}' / marker)):
        if source.read_bytes() != packed.read_bytes():
            raise RuntimeError(f'Resource staging failed: {packed}')
    if (app / '_CodeSignature').exists() or (app / 'embedded.mobileprovision').exists():
        raise RuntimeError('The smoke-test app must be unsigned')
    print(f'iOS smoke build passed: {executable}', flush=True)


if __name__ == '__main__':
    main()
