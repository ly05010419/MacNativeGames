#!/usr/bin/env python3
"""Build TMNT's shader helpers from pinned public sources.

SPIRV-Cross is bundled under TMNT. The translator uses this repository's
TMNT's bundled third_party/ryujinx plus its patch set and needs .NET 10. Build output remains
outside the source checkout. --only-spirv-cross allows independent verification
when the .NET SDK is unavailable.
"""
import argparse
import subprocess
from pathlib import Path

from rewrite_modules import REPO

SPIRV_CROSS_COMMIT = 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165'
SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp',
           'spirv_cross_parsed_ir.cpp', 'spirv_cross_util.cpp', 'spirv_glsl.cpp',
           'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp', 'spirv_reflect.cpp')


def build(source, out, dotnet, only_spirv_cross=False):
    if out == REPO or out.is_relative_to(REPO):
        raise ValueError('shader build output must stay outside the repository')
    bundled = Path(__file__).resolve().parents[1] / 'third_party/spirv-cross'
    if source != bundled:
        commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        if commit != SPIRV_CROSS_COMMIT or subprocess.check_output(
                ['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
            raise ValueError('SPIRV-Cross checkout must be clean at the pinned commit')
    for name in SOURCES:
        if not (source / name).is_file():
            raise ValueError(f'SPIRV-Cross source missing: {name}')
    if out.exists():
        raise ValueError('choose a fresh private build output directory')
    obj = out / 'spirv-cross/obj'
    obj.mkdir(parents=True)
    objects = []
    for name in SOURCES:
        target = obj / (name + '.o')
        subprocess.run(['xcrun', 'clang++', '-O3', '-DNDEBUG', '-std=c++11',
                        '-arch', 'arm64', '-Wno-deprecated-declarations', '-c',
                        str(source / name), '-o', str(target)], check=True)
        objects.append(target)
    cross = out / 'spirv-cross/spirv-cross'
    subprocess.run(['xcrun', 'clang++', '-arch', 'arm64', *(str(o) for o in objects),
                    '-o', str(cross)], check=True)
    if only_spirv_cross:
        return cross, None
    service = Path(__file__).resolve().parents[1] / 'tools/totk_xlate/publish_native.sh'
    subprocess.run([str(service), dotnet, str(out / 'shader-service')], check=True)
    return cross, out / 'shader-service/totk_xlate'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spirv-cross-source', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'third_party/spirv-cross')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--dotnet', default='dotnet')
    parser.add_argument('--only-spirv-cross', action='store_true')
    args = parser.parse_args()
    print(*(str(x) for x in build(args.spirv_cross_source.resolve(),
                                 args.out.resolve(), args.dotnet,
                                 args.only_spirv_cross) if x is not None), sep='\n')


if __name__ == '__main__':
    main()
