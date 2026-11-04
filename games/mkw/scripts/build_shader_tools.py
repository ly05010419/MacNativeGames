#!/usr/bin/env python3
"""Build Mario Kart World's shader translation chain from pinned sources, into this folder's build/:

    python3 scripts/build_shader_tools.py          (or: make shader-tools)

 1. build/spirv-cross/spirv-cross: SPIRV-Cross at the commit nxnative's chain uses (vulkan-sdk-1.4.363.0), fetched into
    build/src/SPIRV-Cross and compiled with clang (the sources and flags of its CMake Release build).
 2. build/dmsh_xlate/dmsh_xlate: the vendored nxnative SM86 frontend (third_party/nxnative, MIT).
 3. build/shader-service/ampere_service: tools/ampere_service, the service the runtime starts.

2 and 3 are published self-contained for osx-arm64, so the game needs no installed .NET runtime. Building them needs the
.NET 10 SDK (`dotnet` on PATH, or the default per-user install at ~/.dotnet) and network access for its NuGet restore and
the SPIRV-Cross fetch. The source fingerprint and all runtime output hashes must match before reusing tools; --force rebuilds all three."""
import argparse
import hashlib
import json
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7')   # vulkan-sdk-1.4.363.0
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')


def say(text):
    print(text, flush=True)


def run(cmd, log, cwd=GAME_DIR):
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w') as f:
        p = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-30:]
        raise SystemExit(f'error: {" ".join(cmd[:3])} ... failed (exit {p.returncode}); {log}:\n' + '\n'.join(tail))


def dotnet():
    found = shutil.which('dotnet') or (str(Path.home() / '.dotnet/dotnet') if (Path.home() / '.dotnet/dotnet').is_file() else None)
    if not found:
        raise SystemExit('error: the .NET 10 SDK is needed to build the shader translator (https://dotnet.microsoft.com/download)')
    return found


def spirv_cross(build, logs, jobs=2):
    out = build / 'spirv-cross/spirv-cross'
    if out.is_file():
        return
    url, commit = SPIRV_CROSS
    src = build / 'src/SPIRV-Cross'
    if not ((src / '.git').is_dir() and subprocess.run(['git', '-C', src, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit):
        say(f'1. fetching SPIRV-Cross {commit[:12]}')
        shutil.rmtree(src, ignore_errors=True)
        src.mkdir(parents=True)
        run(['git', '-C', src, 'init', '-q'], logs / 'spirv-cross-fetch.log')
        run(['git', '-C', src, 'fetch', '-q', '--depth', '1', url, commit], logs / 'spirv-cross-fetch.log')
        run(['git', '-C', src, 'checkout', '-q', 'FETCH_HEAD'], logs / 'spirv-cross-checkout.log')
    say('1. building SPIRV-Cross with clang')
    objs = out.parent / 'obj'
    objs.mkdir(parents=True, exist_ok=True)

    def compile_one(name):
        run(['xcrun', 'clang++', *SPIRV_CROSS_FLAGS, '-c', src / name, '-o', objs / (name + '.o')], logs / 'spirv-cross' / (name + '.log'))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        list(pool.map(compile_one, SPIRV_CROSS_SOURCES))
    run(['xcrun', 'clang++', '-arch', 'arm64', *[objs / (n + '.o') for n in SPIRV_CROSS_SOURCES], '-o', out], logs / 'spirv-cross/link.log')
    for name in ('LICENSE',):
        shutil.copy2(src / name, out.parent / name)


def publish(step, project, out, name, logs):
    if (out / name).is_file():
        return
    say(f'{step}. building {name} (dotnet publish, self-contained osx-arm64)')
    shutil.rmtree(out, ignore_errors=True)
    # --artifacts-path keeps every project's bin/ and obj/ under build/ (not beside the sources in third_party/ and tools/)
    run([dotnet(), 'publish', project, '-c', 'Release', '-r', 'osx-arm64', '--self-contained', 'true', '-o', out,
         '--artifacts-path', GAME_DIR / 'build/dotnet-artifacts', '-p:UseAppHost=true', '-nologo'], logs / f'{name}.log')
    if not (out / name).is_file():
        raise SystemExit(f'error: dotnet publish did not write {out / name}')


def source_fingerprint():
    digest = hashlib.sha256()
    inputs = [Path(__file__).resolve(), GAME_DIR / 'tools/wrap_mesh_stage.py', GAME_DIR / 'tools/dmsh_meshstage.py', GAME_DIR / 'tools/prepare_ampere_jobs.py']
    for directory in ('third_party/nxnative', 'tools/ampere_service'):
        inputs.extend(p for p in sorted((GAME_DIR / directory).rglob('*'))
                      if p.is_file() and not any(part in ('obj', 'bin', '__pycache__') for part in p.parts))
    for path in inputs:
        digest.update(str(path.relative_to(GAME_DIR)).encode() + b'\0')
        digest.update(path.read_bytes())
    return digest.hexdigest()


def output_fingerprints(build):
    outputs = {}
    for name in ('spirv-cross', 'dmsh_xlate', 'shader-service'):
        for path in sorted((build / name).rglob('*')):
            if path.is_file() and 'obj' not in path.relative_to(build).parts:
                outputs[str(path.relative_to(build))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return outputs


def tools_current(build, fingerprint):
    try:
        record = json.loads((build / 'shader-tools.json').read_text())
        return (record['source_sha256'] == fingerprint and
                record['outputs'] == output_fingerprints(build) and
                all((build / p).is_file() for p in ('spirv-cross/spirv-cross', 'dmsh_xlate/dmsh_xlate', 'shader-service/ampere_service')))
    except (OSError, ValueError, KeyError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--force', action='store_true', help='rebuild all three tools')
    ap.add_argument('--jobs', type=int, default=2, help='parallel C++ compile jobs')
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error('--jobs must be positive')
    if sys.platform != 'darwin':
        raise SystemExit('error: macOS only')
    build, logs = GAME_DIR / 'build', GAME_DIR / 'build/logs/shader-tools'
    fingerprint = source_fingerprint()
    if args.force or not tools_current(build, fingerprint):
        for d in ('spirv-cross', 'dmsh_xlate', 'shader-service', 'dotnet-artifacts'):
            shutil.rmtree(build / d, ignore_errors=True)
    spirv_cross(build, logs, args.jobs)
    publish(2, GAME_DIR / 'third_party/nxnative/dmsh_xlate/dmsh_xlate.csproj', build / 'dmsh_xlate', 'dmsh_xlate', logs)
    publish(3, GAME_DIR / 'tools/ampere_service/ampere_service.csproj', build / 'shader-service', 'ampere_service', logs)
    outputs = output_fingerprints(build)
    record = dict(source_sha256=fingerprint, outputs=outputs)
    (build / 'shader-tools.json').write_text(json.dumps(record, indent=2) + '\n')
    chain = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()[:16]
    header = build / 'mkw_shader_chain.h'
    contents = f'#define DMSH_CHAIN "mkw-sm86-{chain}"\n'
    if not header.exists() or header.read_text() != contents:
        header.write_text(contents)
    say('shader tools ready: build/spirv-cross/spirv-cross, build/dmsh_xlate/dmsh_xlate, build/shader-service/ampere_service')


if __name__ == '__main__':
    main()
