#!/usr/bin/env python3
"""Prepare and build Mario Kart 8 Deluxe 4.0.1 on the Zelda-line runtime in a work folder (bring-up stage).

    python3 scripts/build_mk8dx.py --work WORK --extracted EXTRACTED --keys prod.keys --firmware FIRMWARE \
        [--aoc AOC_IMAGE_OR_FOLDER] [--shared-fonts FOLDER]

EXTRACTED holds the game as it comes out of your own base game + 4.0.1 update: `ExeFS/` (main, sdk, subsdk0, main.npdm)
and the merged RomFS as one image `RomFS.img` (a Nintendo RomFS, or the flat DMROMFS1 image another build of this game
on this Mac left; scripts/index_romfs_image.py reads both). Extraction straight from the NSPs (base + update BKTR, as
games/odyssey's merge does) is the next step; see CHANGELOG.md.

Steps, each skipped when its output is already there:
 1. ExeFS copied and checked against assets/expected-4.0.1.json; the RomFS image linked (not copied: 12 GB) and indexed
 2. add-on content (Booster Course Pass) files, when given, into <work>/extracted/aoc/1
 3. the console's Mii parts (scripts/extract_mii_model.py) and system fonts (games/totk/scripts/extract_shared_fonts.py),
    from your own firmware
 4. the ARM64 rewrite (third_party/d2r/patch_nso.py, native ruleset)
 5. loader tables (scripts/prepare_runtime.py), shader tools and the runtime (make)
Run the game from the work folder: `make boot WORK=<work>` (README.md lists the environment a test run uses)."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
EXPECTED = json.loads((HERE / 'assets/expected-4.0.1.json').read_text())
MODULES = ('main', 'sdk', 'subsdk0')


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
    ap.add_argument('--extracted', type=Path, required=True, help='folder with ExeFS/ and RomFS.img of base + 4.0.1')
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--firmware', type=Path, help='your own firmware NCA folder (Mii parts, system fonts)')
    ap.add_argument('--shared-fonts', type=Path, help='a shared-fonts folder of an earlier build instead of --firmware')
    ap.add_argument('--aoc', type=Path, help='the Booster Course Pass data: an image (RomFS / DMROMFS1) or a folder of its files')
    ap.add_argument('--developer-dir', default='/Applications/Xcode.app/Contents/Developer')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    work, py = args.work.resolve(), sys.executable
    ext = work / 'extracted'

    print('== 1. game files', flush=True)
    exefs = ext / 'local/exefs'
    exefs.mkdir(parents=True, exist_ok=True)
    for name in (*MODULES, 'main.npdm'):
        src, dst = args.extracted / 'ExeFS' / name, exefs / name
        if not dst.is_file():
            shutil.copyfile(src, dst)
        if sha(dst) != EXPECTED['exefs_sha256'][name]:
            raise SystemExit(f'{name} is not the 4.0.1 module (assets/expected-4.0.1.json)')
    romfs = ext / 'local/romfs'
    romfs.mkdir(parents=True, exist_ok=True)
    if not (romfs / 'romfs.bin').exists():
        os.symlink((args.extracted / 'RomFS.img').resolve(), romfs / 'romfs.bin')
    if not (romfs / 'index.json').is_file():
        run([py, HERE / 'scripts/index_romfs_image.py', romfs])

    print('== 2. add-on content', flush=True)
    aoc = ext / 'aoc/1'
    if aoc.is_dir() or not args.aoc:
        print('   ' + ('already there' if aoc.is_dir() else 'none given: the game runs without the Booster Course Pass'))
    elif args.aoc.is_dir():
        shutil.copytree(args.aoc, aoc)
    else:
        image = ext / 'aoc-image'
        image.mkdir(parents=True, exist_ok=True)
        if not (image / 'romfs.bin').exists():
            os.symlink(args.aoc.resolve(), image / 'romfs.bin')
        run([py, HERE / 'scripts/index_romfs_image.py', image, '--extract', aoc])

    print('== 3. Mii parts and system fonts (your own firmware)', flush=True)
    mii = ext / 'firmware/mii-model'
    if not mii.is_dir():
        if not args.firmware:
            raise SystemExit('the Mii parts come from your own firmware: pass --firmware')
        run([py, HERE / 'scripts/extract_mii_model.py', '--keys', args.keys, '--firmware', args.firmware, '--output', mii])
    fonts = ext / 'firmware/shared-fonts'
    if not fonts.is_dir():
        if args.shared_fonts:
            shutil.copytree(args.shared_fonts, fonts)
        elif args.firmware:
            run([py, ROOT / 'games/totk/scripts/extract_shared_fonts.py', '--keys', args.keys, '--firmware', args.firmware,
                 '--output', fonts], cwd=ROOT / 'games/totk/scripts')
        else:
            raise SystemExit('the system fonts come from your own firmware: pass --firmware or --shared-fonts')

    print('== 4. ARM64 rewrite', flush=True)
    rewritten = work / 'rewritten/local'
    rewritten.mkdir(parents=True, exist_ok=True)
    for name in MODULES:
        if not (rewritten / f'{name}.patched.bin').is_file():
            run([py, HERE / 'third_party/d2r/patch_nso.py', exefs / name, rewritten, name],
                env={'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'})

    print('== 5. loader tables and the runtime', flush=True)
    run([py, HERE / 'scripts/prepare_runtime.py', '--work', work])
    src = work / 'src/SPIRV-Cross'
    if not (src / '.git').is_dir():
        run(['git', 'clone', '--quiet', 'https://github.com/KhronosGroup/SPIRV-Cross.git', src])
    run(['git', '-C', src, 'checkout', '--quiet', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'])
    run(['make', f'-j{args.jobs}', 'runtime', f'WORK={work}', f'PY={py}'], env={'DEVELOPER_DIR': args.developer_dir})
    print(f'\nBuilt {work}/build/mk8dx-native. It runs with game data from your own dump: do not share the work folder.')


if __name__ == '__main__':
    main()
