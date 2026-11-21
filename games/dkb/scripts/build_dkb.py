#!/usr/bin/env python3
"""Build Donkey Kong Bananza (Switch 2) into a native Apple silicon Mac app from your own dump folder, with the nxnative copy in games/dkb (Q150).

    build_dkb.py <dump folder> --work-dir <folder outside the checkout> --output "<path>/Donkey Kong Bananza.app"

The dump folder holds the game's files as nxnative's scripts/pack_dump.py lists them: base/, updates/<version>/, dlc/<id>/ (no prod.keys
needed). Steps:
  1. stage: runtime/ -> <work>/source/dm, third_party/nxnative/scripts -> <work>/source/scripts (generated code, tables and caches stay
     outside the checkout);
  2. the SPIR-V to Metal converter (carried as nxnative ships it) must match the fingerprint dmsh_chain.lock records, or the build stops:
     another converter would be another shader chain, every shader translated again. Only when it is missing is it built from nxnative's
     pinned build script, and that build must match too (with Xcode 27.2 it does not: 098403a9 against e37945d8);
  3. nxnative's NSP script builds the app (--platform mac-app). Its work folder is <dump folder>/040074A01BF12000_build, as for a build from an
     nxnative checkout of the same version: the shaders it already translated there are reused;
  4. codesign --verify on the app.
Saves stay where nxnative keeps them, ~/Library/Application Support/nxnative/040074A01BF12000/, whichever build made the app.
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
TITLE = '040074A01BF12000'
APP_NAME = 'Donkey Kong Bananza'
BUNDLE_ID = 'org.macnativegames.dkb'
PY_MODULES = ('lz4', 'capstone', 'fontTools', 'nsz', 'zbic')   # what the NSP script and its tools import beyond the standard library
SPIRV = 'dm/gfx/tools/spirv_cross_bin'


def copy_tree(src, dst):
    for f in sorted(src.rglob('*')):
        rel = f.relative_to(src)
        if not f.is_file() or any(p == '__pycache__' or p.endswith('.dSYM') for p in rel.parts):
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        if not out.exists() or out.read_bytes() != f.read_bytes():   # unchanged files keep their time: the build's own caches see no change
            shutil.copy2(f, out)


def stage(work):
    source = work / 'source'
    copy_tree(GAME / 'runtime', source / 'dm')
    copy_tree(GAME / 'third_party/nxnative/scripts', source / 'scripts')
    shutil.copy2(GAME / 'third_party/nxnative/LICENSE', source / 'LICENSE')
    for exe in (source / 'dm/gfx/tools/dmsh_xlate_bin').glob('dmsh_xlate*'):
        if exe.suffix == '':
            exe.chmod(0o755)
    return source


def locked_fingerprint(source, name):
    for line in (source / 'dm/gfx/tools/dmsh_chain.lock').read_text().splitlines():
        parts = line.split('\t')
        if len(parts) == 2 and parts[0] == name:
            return parts[1]
    raise ValueError(f'dmsh_chain.lock has no {name} line')


def build_spirv_cross(source, work):
    exe = source / SPIRV / 'spirv-cross'
    want = locked_fingerprint(source, 'spirv-cross')
    have = lambda: hashlib.sha256(exe.read_bytes()).hexdigest()[:len(want)] if exe.is_file() else ''
    if have() != want:
        if not shutil.which('cmake'):
            raise ValueError('cmake is needed once to build the SPIR-V to Metal converter (brew install cmake)')
        subprocess.run(['sh', str(source / SPIRV / 'build.sh'), str(work / 'spirv-cross')], check=True, stdin=subprocess.DEVNULL)
    if have() != want:
        raise ValueError(f'the SPIR-V to Metal converter built here is {have() or "missing"}, dmsh_chain.lock expects {want}: '
                         'another Xcode gives other bytes, which would be another shader chain; nothing was built')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', type=Path, help='folder with base/, updates/<version>/ and dlc/ of your own dump')
    ap.add_argument('--work-dir', required=True, type=Path, help='private staging folder outside the checkout')
    ap.add_argument('--output', required=True, type=Path, help='the .app to write (must not exist yet)')
    a = ap.parse_args()
    dump, work, output = (p.expanduser().resolve() for p in (a.dump, a.work_dir, a.output))
    checkout = GAME.parents[1]
    if output.suffix != '.app':
        ap.error('--output must end in .app')
    if output.exists():
        ap.error(f'output already exists: {output}')
    if any(p == checkout or checkout in p.parents for p in (dump, work, output)):
        ap.error('the dump, --work-dir and --output must be outside the checkout, so no game content gets into git')
    if not (dump / 'base').is_dir():
        ap.error(f'{dump} has no base/ folder (layout: base/, updates/<version>/, dlc/<id>/)')
    missing = [m for m in PY_MODULES if importlib.util.find_spec(m) is None]
    if missing:
        ap.error(f'this Python ({sys.executable}) lacks {", ".join(missing)}: pip install lz4 capstone fonttools nsz zbic')
    work.mkdir(parents=True, exist_ok=True)
    source = stage(work)
    build_spirv_cross(source, work)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    subprocess.run([sys.executable, str(source / 'scripts/build_ipa_from_nsp.py'), str(dump), '--platform', 'mac-app',
                    '--out', str(output), '--app-name', APP_NAME, '--bundle-id', BUNDLE_ID, '--progress', 'lines'],
                   env=env, check=True, stdin=subprocess.DEVNULL)
    if not output.is_dir():
        raise ValueError(f'the build finished without an app: {output}')
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(output)], check=True)
    print(f'Built {output}\nSaves: ~/Library/Application Support/nxnative/{TITLE}/  Log: ~/Library/Logs/nxnative/{TITLE}.log')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f'error: {error}')
