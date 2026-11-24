#!/usr/bin/env python3
"""Prepare and build Persona 5 Royal 1.0.2 on the Zelda-line runtime in a work folder (bring-up stage).

    python3 scripts/build_p5r.py --work WORK --base "<base 1.0.0>.nsp" --update "<1.0.2 update>.nsp" --keys prod.keys

The game files are your own NSPs: the base game (01004B10157F2000 v0, or the American 01005CA01580E000) and its 1.0.2
update (v65536). Steps, each skipped when its output is already there:
 1. the update's ExeFS (games/pla/scripts/extract_exefs.py --update), checked against assets/expected-1.0.2.json
 2. the merged RomFS (games/eow/scripts/merge_romfs.py: the update's BKTR patch over the base NSP's RomFS, every block
    of the result checked against the update's six IVFC levels): 13.7 GB, written once
 3. the ARM64 rewrite of main, subsdk0 and sdk (third_party/d2r/patch_nso.py, native ruleset)
 4. loader tables (scripts/prepare_runtime.py), shader tools and the runtime (make)
 5. the icon of the game's control data (games/pla/scripts/extract_app_icon.py: build/AppIcon.icns) and the shader seed
    for the iPhone / iPad (scripts/prepare_p5r_shader_seed.py: build/shader-seed, from main and the CPK's GSC files);
    ios/tools/build_ipa.py makes the app from them
Run the game from the work folder: `make boot WORK=<work>` (README.md lists the environment a test run uses)."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
EXPECTED = json.loads((HERE / 'assets/expected-1.0.2.json').read_text())
MODULES = ('main', 'subsdk0', 'sdk')
PLA_SCRIPTS = ROOT / 'games/pla/scripts'
EOW_SCRIPTS = ROOT / 'games/eow/scripts'
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


def check_exefs(exefs):
    """Every module and the NPDM must be 1.0.2's (the pins); a missing file is reported, not traced."""
    for name in (*MODULES, 'main.npdm'):
        path = exefs / name
        if not path.is_file():
            raise SystemExit(f'{path} is missing: extract the 1.0.2 update again (remove {exefs})')
        if sha(path) != EXPECTED['exefs_sha256'][name]:
            raise SystemExit(f'{name} is not the 1.0.2 module (assets/expected-1.0.2.json): another version of the game?')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--base', type=Path, required=True, help='the base game NSP (1.0.0)')
    ap.add_argument('--update', type=Path, required=True, help='the 1.0.2 update NSP (v65536)')
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--title', default=EXPECTED['title_id'], help='base title ID (the American release: 01005CA01580E000)')
    ap.add_argument('--update-title', default=EXPECTED['update_title_id'])
    ap.add_argument('--developer-dir', default='/Applications/Xcode.app/Contents/Developer')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    for path in (args.base, args.update, args.keys):
        if not path.is_file():
            raise SystemExit(f'not found: {path}')
    work, py = args.work.resolve(), sys.executable
    local = work / 'extracted/local'

    print('== 1. the 1.0.2 ExeFS', flush=True)
    exefs = local / 'exefs'
    if not exefs.is_dir():
        run([py, PLA_SCRIPTS / 'extract_exefs.py', args.update, '--keys', args.keys, '--title', args.title,
             '--update-title', args.update_title, '--update', '--output', exefs], cwd=PLA_SCRIPTS)
    check_exefs(exefs)

    print('== 2. the merged RomFS (13.7 GB)', flush=True)
    romfs = local / 'romfs'
    if not (romfs / 'index.json').is_file():
        run([py, EOW_SCRIPTS / 'merge_romfs.py', args.base, args.update, '--keys', args.keys, '--title', args.title,
             '--output', romfs], cwd=EOW_SCRIPTS)

    print('== 3. ARM64 rewrite', flush=True)
    rewritten = work / 'rewritten/local'
    rewritten.mkdir(parents=True, exist_ok=True)
    for name in MODULES:
        if not (rewritten / f'{name}.patched.bin').is_file():
            run([py, HERE / 'third_party/d2r/patch_nso.py', exefs / name, rewritten, name],
                env={'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'}, cwd=HERE / 'third_party/d2r')

    print('== 4. loader tables and the runtime', flush=True)
    run([py, HERE / 'scripts/prepare_runtime.py', '--work', work])
    src = work / 'src/SPIRV-Cross'
    if not (src / '.git').is_dir():
        run(['git', 'clone', '--quiet', 'https://github.com/KhronosGroup/SPIRV-Cross.git', src])
    run(['git', '-C', src, 'checkout', '--quiet', SPIRV_CROSS_COMMIT])
    run(['make', f'-j{args.jobs}', 'runtime', f'WORK={work}', f'PY={py}'], env={'DEVELOPER_DIR': args.developer_dir})

    print('== 5. the icon and the shader seed (iPhone / iPad)', flush=True)
    icon = work / 'build/AppIcon.icns'
    if not icon.is_file():
        run([py, PLA_SCRIPTS / 'extract_app_icon.py', args.base, '--keys', args.keys, '--title', args.title,
             '--output', icon], cwd=PLA_SCRIPTS)
    run([py, HERE / 'scripts/prepare_p5r_shader_seed.py', '--work', work])
    print(f'\nBuilt {work}/build/p5r-native. It runs with game data from your own dump: do not share the work folder.')


if __name__ == '__main__':
    main()
