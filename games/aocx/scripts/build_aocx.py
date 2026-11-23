#!/usr/bin/env python3
"""Prepare and build Hyrule Warriors: Age of Calamity 1.4.1 on the Zelda-line runtime in a work folder (bring-up stage).

    python3 scripts/build_aocx.py --work WORK --base "<base 1.0.0>.nsp" --update "<1.4.1 update>.nsp" --keys prod.keys \\
        [--dlc <folder of add-on NSPs>]

The game files are your own: the base game NSP, its 1.4.1 update NSP (v458752) and, if you own them, the add-on NSPs.
Steps, each skipped when its output is already there:
 1. the update's ExeFS (games/pla/scripts/extract_exefs.py --update), checked against assets/expected-1.4.1.json
 2. the merged RomFS (scripts/merge_nsp_romfs.py: the update's BKTR patch over the base read from its NSP, every block of
    the result checked against the update's six IVFC levels): 11.7 GB, written once, checked against its pin
 3. the add-on content (scripts/extract_dlc.py: extracted/aoc/<index>, pinned in assets/expected-dlc.json)
 4. the ARM64 rewrite of main, subsdk0 and sdk (third_party/d2r/patch_nso.py, native ruleset)
 5. loader tables (scripts/prepare_runtime.py), shader tools and the runtime (make)
 6. the icon of the game's control data (games/pla/scripts/extract_app_icon.py: build/AppIcon.icns) and the shader seed
    for the iPhone / iPad (scripts/prepare_aocx_shader_seed.py: build/shader-seed, from the RDB data files and main);
    ios/tools/build_ipa.py makes the app from them
Run the game from the work folder (README.md lists the environment a test run uses)."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
EXPECTED = json.loads((HERE / 'assets/expected-1.4.1.json').read_text())
MODULES = ('main', 'subsdk0', 'sdk')
PLA_SCRIPTS = ROOT / 'games/pla/scripts'
SPIRV_CROSS_COMMIT = 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'


def run(cmd, env=None, cwd=HERE):
    print('   $ ' + ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=cwd, env={**os.environ, **(env or {})})


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--base', type=Path, required=True, help='the base game NSP (1.0.0)')
    ap.add_argument('--update', type=Path, required=True, help='the 1.4.1 update NSP')
    ap.add_argument('--dlc', type=Path, nargs='*', default=[], help='add-on NSPs or folders of them (optional)')
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--developer-dir', default='/Applications/Xcode.app/Contents/Developer')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    work, py = args.work.resolve(), sys.executable
    local = work / 'extracted/local'

    print('== 1. the 1.4.1 ExeFS', flush=True)
    exefs = local / 'exefs'
    if not exefs.is_dir():
        run([py, PLA_SCRIPTS / 'extract_exefs.py', args.update, '--keys', args.keys, '--title', EXPECTED['title_id'],
             '--update-title', EXPECTED['update_title_id'], '--update', '--output', exefs], cwd=PLA_SCRIPTS)
    for name in (*MODULES, 'main.npdm'):
        if sha(exefs / name) != EXPECTED['exefs_sha256'][name]:
            raise SystemExit(f'{name} is not the 1.4.1 module (assets/expected-1.4.1.json): another version of the game?')

    print('== 2. the merged RomFS (11.7 GB)', flush=True)
    romfs = local / 'romfs'
    if not (romfs / 'index.json').is_file():
        run([py, HERE / 'scripts/merge_nsp_romfs.py', args.base, args.update, '--keys', args.keys, '--output', romfs])
    index = json.loads((romfs / 'index.json').read_text())
    if index['romfs_sha256'] != EXPECTED['romfs_sha256'] or len(index['files']) != EXPECTED['romfs_files']:
        raise SystemExit('the merged RomFS is not 1.4.1\'s (assets/expected-1.4.1.json)')

    print('== 3. add-on content', flush=True)
    if args.dlc:
        run([py, HERE / 'scripts/extract_dlc.py', *args.dlc, '--keys', args.keys, '--work', work])
    else:
        print('   none given: the game runs without its add-ons')

    print('== 4. ARM64 rewrite', flush=True)
    rewritten = work / 'rewritten/local'
    rewritten.mkdir(parents=True, exist_ok=True)
    for name in MODULES:
        if not (rewritten / f'{name}.patched.bin').is_file():
            run([py, HERE / 'third_party/d2r/patch_nso.py', exefs / name, rewritten, name],
                env={'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'}, cwd=HERE / 'third_party/d2r')

    print('== 5. loader tables and the runtime', flush=True)
    run([py, HERE / 'scripts/prepare_runtime.py', '--work', work])
    src = work / 'src/SPIRV-Cross'
    if not (src / '.git').is_dir():
        run(['git', 'clone', '--quiet', 'https://github.com/KhronosGroup/SPIRV-Cross.git', src])
    run(['git', '-C', src, 'checkout', '--quiet', SPIRV_CROSS_COMMIT])
    run(['make', f'-j{args.jobs}', 'runtime', f'WORK={work}', f'PY={py}'], env={'DEVELOPER_DIR': args.developer_dir})
    print('== 6. the icon and the shader seed (iPhone / iPad)', flush=True)
    icon = work / 'build/AppIcon.icns'
    if not icon.is_file():
        run([py, PLA_SCRIPTS / 'extract_app_icon.py', args.update, '--keys', args.keys, '--title', EXPECTED['title_id'],
             '--output', icon], cwd=PLA_SCRIPTS)
    run([py, HERE / 'scripts/prepare_aocx_shader_seed.py', '--work', work])
    print(f'\nBuilt {work}/build/aocx-native. It runs with game data from your own dump: do not share the work folder.')


if __name__ == '__main__':
    main()
