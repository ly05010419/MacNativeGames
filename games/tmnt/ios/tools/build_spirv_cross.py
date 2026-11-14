#!/usr/bin/env python3
"""Build TMNT's pinned SPIRV-Cross as an arm64 iPad static archive."""
import argparse
import subprocess
import sys
from pathlib import Path

SOURCES = ('spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp',
           'spirv_cross_parsed_ir.cpp', 'spirv_cross_util.cpp',
           'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp',
           'spirv_parser.cpp', 'spirv_reflect.cpp')


def build(sdk, output, cxx, ar):
    game = Path(__file__).resolve().parents[2]
    if output == game or game in output.parents:
        raise ValueError('build output must stay outside the repository')
    sys.path.insert(0, str(game.parents[1]))
    from tools.deps import spirv_cross_source
    source = spirv_cross_source()
    wrapper = game / 'tools/totk_xlate/spirv_cross_inproc.cpp'
    if not sdk.is_dir() or not wrapper.is_file():
        raise ValueError('iPhoneOS SDK or in-process wrapper is missing')
    objects = output / 'objects'
    objects.mkdir(parents=True, exist_ok=True)
    common = [cxx, '-target', 'arm64-apple-ios26.0', '-isysroot', str(sdk),
              '-std=c++17', '-fPIC', '-O2', '-DNDEBUG', '-I', str(source), '-c']
    built = []
    for item in (*SOURCES, 'spirv_cross_inproc.cpp'):
        path = wrapper if item == 'spirv_cross_inproc.cpp' else source / item
        target = objects / (Path(item).stem + '.o')
        subprocess.run([*common, str(path), '-o', str(target)], check=True)
        built.append(str(target))
    archive = output / 'libtmnt_spirv_cross.a'
    subprocess.run([ar, 'rcs', str(archive), *built], check=True)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--ar', default='ar')
    args = parser.parse_args()
    print(build(args.sdk.resolve(), args.out.resolve(), args.cxx, args.ar))


if __name__ == '__main__':
    main()
