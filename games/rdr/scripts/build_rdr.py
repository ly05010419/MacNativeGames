#!/usr/bin/env python3
"""Prepare and build Red Dead Redemption 1.0.4 on the Zelda-line runtime in a work folder (bring-up stage).

    python3 scripts/build_rdr.py --work WORK --game "<base + 1.0.4 update>.xci" --keys prod.keys

The game file is your own card image holding the base game and the 1.0.4 update (v262144) together ("1G+1U"). Steps,
each skipped when its output is already there:
 1. the update's ExeFS (games/pla/scripts/extract_exefs.py --update), checked against assets/expected-1.0.4.json
 2. the merged RomFS (scripts/merge_card_romfs.py: the update's BKTR patch over the base, the base read from the card,
    every block of the result checked against the update's six IVFC levels): 11.2 GB, written once
 3. the ARM64 rewrite of main and sdk (third_party/d2r/patch_nso.py, native ruleset; main's patch area goes before the
    module, as Metroid Dread's: its 1.2 GiB .bss)
 4. loader tables (scripts/prepare_runtime.py), shader tools and the runtime (make)
 5. the icon of the game's control data (games/pla/scripts/extract_app_icon.py: build/AppIcon.icns) and the shader seed
    for the iPhone / iPad (scripts/prepare_rdr_shader_seed.py: build/shader-seed, from shaders.rpf and main);
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
if str(ROOT) not in sys.path:   # the repository root, after this folder's own modules
    sys.path.append(str(ROOT))
from tools.port_app.make_path import make_path  # noqa: E402  every port's make WORK= (issue #2284)
EXPECTED = json.loads((HERE / 'assets/expected-1.0.4.json').read_text())
MODULES = ('main', 'sdk')
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
    ap.add_argument('--game', type=Path, required=True, help='the card image with the base game and the 1.0.4 update')
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--developer-dir', default='/Applications/Xcode.app/Contents/Developer')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    work, py = args.work.resolve(), sys.executable
    local = work / 'extracted/local'

    print('== 1. the 1.0.4 ExeFS', flush=True)
    exefs = local / 'exefs'
    if not exefs.is_dir():
        run([py, PLA_SCRIPTS / 'extract_exefs.py', args.game, '--keys', args.keys, '--title', EXPECTED['title_id'],
             '--update-title', EXPECTED['update_title_id'], '--update', '--output', exefs], cwd=PLA_SCRIPTS)
    for name in (*MODULES, 'main.npdm'):
        if sha(exefs / name) != EXPECTED['exefs_sha256'][name]:
            raise SystemExit(f'{name} is not the 1.0.4 module (assets/expected-1.0.4.json): another version of the game?')

    print('== 2. the merged RomFS (11.2 GB)', flush=True)
    romfs = local / 'romfs'
    if not (romfs / 'index.json').is_file():
        run([py, HERE / 'scripts/merge_card_romfs.py', args.game, '--keys', args.keys, '--output', romfs])

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
    run(['make', f'-j{args.jobs}', 'runtime', f'WORK={make_path(work, "rdr")}', f'PY={py}'], env={'DEVELOPER_DIR': args.developer_dir})
    print('== 5. the icon and the shader seed (iPhone / iPad)', flush=True)
    icon = work / 'build/AppIcon.icns'
    if not icon.is_file():
        run([py, PLA_SCRIPTS / 'extract_app_icon.py', args.game, '--keys', args.keys, '--title', EXPECTED['title_id'],
             '--output', icon], cwd=PLA_SCRIPTS)
    run([py, HERE / 'scripts/prepare_rdr_shader_seed.py', '--work', work])
    print(f'\nBuilt {work}/build/rdr-native. It runs with game data from your own dump: do not share the work folder.')


if __name__ == '__main__':
    main()
