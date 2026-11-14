#!/usr/bin/env python3
"""Build TMNT's shader helpers from pinned public sources.

Uses the shared pinned SPIRV-Cross CLI and the TMNT translation service.
"""
import argparse
import shutil
import sys
import subprocess
from pathlib import Path

from rewrite_modules import REPO

sys.path.insert(0, str(REPO))
from tools.deps import program


def build(out, dotnet, only_spirv_cross=False):
    if out == REPO or out.is_relative_to(REPO) or out.exists():
        raise ValueError('choose a fresh shader output outside the repository')
    cross = out / 'spirv-cross/spirv-cross'
    cross.parent.mkdir(parents=True)
    shutil.copy2(program('spirv-cross'), cross)
    if only_spirv_cross:
        return cross, None
    service = Path(__file__).resolve().parents[1] / 'tools/totk_xlate/publish_native.sh'
    subprocess.run([str(service), dotnet or str(program('dotnet')), str(out / 'shader-service')], check=True)
    return cross, out / 'shader-service/totk_xlate'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--dotnet')
    parser.add_argument('--only-spirv-cross', action='store_true')
    args = parser.parse_args()
    print(*(str(x) for x in build(args.out.resolve(), args.dotnet,
                                 args.only_spirv_cross) if x is not None), sep='\n')


if __name__ == '__main__':
    main()
