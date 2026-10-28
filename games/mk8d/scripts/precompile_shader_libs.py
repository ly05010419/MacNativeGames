#!/usr/bin/env python3
"""Compile the shader seed's Metal libraries ahead of time (adapted from Odyssey's precompile_cached_shaders.py).

    .venv/bin/python scripts/precompile_shader_libs.py --work WORK

Without them the runtime compiles each library from its MSL the first time a scene shows it. Each library is named by the
SHA-256 of its exact MSL source (nvn_mtl.m shader_lib_prebuilt_path) and compiled with the runtime's math mode, so only an
exact match is ever used; one that fails to compile is simply compiled by the runtime at first use. Libraries already
built are kept; ones no seed shader names any more are removed. Output: <work>/build/shader-libs, beside the runtime
(make run) and linked into the app (scripts/package_mk8d_app.py). Game-derived: never share or commit it.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess

FLAGS = ("-fmetal-math-mode=safe",)   # the runtime's totk_precise_opts; TOTK_FAST_MATH=1 skips these libraries


def compiler(env):
    found = subprocess.run(["xcrun", "--find", "metal"], env=env, text=True, capture_output=True)
    if found.returncode:
        raise SystemExit("Xcode's Metal compiler is missing: install the Metal Toolchain component (Xcode > Settings > Components)")
    path = found.stdout.strip()
    version = subprocess.run([path, "--version"], env=env, text=True, capture_output=True).stdout.splitlines()
    if not version:
        raise SystemExit("Xcode's Metal compiler does not run; finish installing the Metal Toolchain component")
    return path, " ".join((version[0], *FLAGS))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", type=Path, required=True, help="the build folder (scripts/build_mk8d.py --work)")
    p.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    p.add_argument("--developer-dir", default=os.environ.get("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer"))
    a = p.parse_args()
    work = a.work.expanduser().resolve()
    seed, output = work / "build/shader-seed", work / "build/shader-libs"
    sources = sorted(seed.glob("*_b.metal"))
    if not (seed / "seed-manifest.json").is_file() or not sources:
        raise SystemExit(f"no shader seed in {seed}; run scripts/prepare_mk8d_shader_seed.py first")
    env = dict(os.environ, DEVELOPER_DIR=a.developer_dir)
    metal, ident = compiler(env)
    output.mkdir(parents=True, exist_ok=True)
    stamp = output / "compiler.txt"
    if stamp.is_file() and stamp.read_text() != ident:   # another compiler or options: every library again
        for old in output.glob("*.metallib"):
            old.unlink()
    stamp.write_text(ident)
    wanted = {}
    for source in sources:
        wanted.setdefault(hashlib.sha256(source.read_bytes()).hexdigest(), source)
    stale = [lib for lib in output.glob("*.metallib") if lib.stem not in wanted]
    for lib in stale:
        lib.unlink()
    todo = [(digest, source) for digest, source in wanted.items()
            if not (output / f"{digest}.metallib").is_file() or (output / f"{digest}.metallib").stat().st_size < 4]
    print(f"{len(wanted)} libraries for {len(sources)} seed variants: {len(wanted) - len(todo)} already built, "
          f"compiling {len(todo)} with {a.jobs} jobs; {len(stale)} stale removed", flush=True)

    def build(item):
        digest, source = item
        partial, library = output / f"{digest}.partial", output / f"{digest}.metallib"
        result = subprocess.run([metal, *FLAGS, "-o", str(partial), str(source)], env=env, text=True, capture_output=True)
        if result.returncode or not partial.is_file() or partial.read_bytes()[:4] != b"MTLB":
            partial.unlink(missing_ok=True)
            return f"{source.name}: {(result.stdout + result.stderr).strip()[:2000]}\n"
        partial.replace(library)
        return None

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        failures = [message for message in pool.map(build, todo) if message]
    (output / "compile-failures.log").write_text("".join(failures))
    print(f"compiled {len(todo) - len(failures)} libraries; {len(failures)} failed and are compiled at first use "
          f"({output / 'compile-failures.log'})", flush=True)


if __name__ == "__main__":
    main()
