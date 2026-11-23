#!/usr/bin/env python3
"""Stage TMNT's shader helpers: the shared pinned SPIRV-Cross CLI and the translation service.

The service is prebuilt/macos-arm64/totk_xlate, built with NativeAOT from this game's sources
(tools/totk_xlate/publish_native.sh: third_party/ryujinx, the patches, Service.cs) like the other games' translators,
so a build needs no .NET SDK (user 2026-11-22: "对 做吧"; a player's Launcher carries no SDK). A file whose SHA-256 is
not totk_xlate.sha256 is refused; a rebuild is checked by replaying shaders (prebuilt/macos-arm64/README.md).
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

from rewrite_modules import REPO

sys.path.insert(0, str(REPO))
from tools.deps import program

PREBUILT = Path(__file__).resolve().parents[1] / 'prebuilt/macos-arm64'


def service_digest():
    """The pinned SHA-256 of the prebuilt service (it names the shader-tools step's inputs too)."""
    return (PREBUILT / 'totk_xlate.sha256').read_text().split()[0]


def build(out, only_spirv_cross=False):
    if out == REPO or out.is_relative_to(REPO) or out.exists():
        raise ValueError('choose a fresh shader output outside the repository')
    cross = out / 'spirv-cross/spirv-cross'
    cross.parent.mkdir(parents=True)
    shutil.copy2(program('spirv-cross'), cross)
    if only_spirv_cross:
        return cross, None
    service = out / 'shader-service/totk_xlate'
    service.parent.mkdir(parents=True)
    shutil.copy2(PREBUILT / 'totk_xlate', service)
    if hashlib.sha256(service.read_bytes()).hexdigest() != service_digest():
        raise ValueError(f'{PREBUILT / "totk_xlate"} is not the pinned translator (totk_xlate.sha256): check it out again')
    shutil.copy2(PREBUILT / 'totk_xlate.sha256', service.parent / 'totk_xlate.sha256')
    return cross, service


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--only-spirv-cross', action='store_true')
    args = parser.parse_args()
    print(*(str(x) for x in build(args.out.resolve(), args.only_spirv_cross) if x is not None), sep='\n')


if __name__ == '__main__':
    main()
