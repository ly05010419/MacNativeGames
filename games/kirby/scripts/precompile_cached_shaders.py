#!/usr/bin/env python3
"""Compile locally translated Kirby shaders into .metallib before app launch.

The runtime looks up each library by SHA-256 of the exact MSL source bytes
in `shader-libs` (next to the executable in Contents/MacOS and game/build/shader-libs).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess
import sys

BASE = Path("/Users/vinicius/Documents/StreamHelp")
KIRBY_BUILD = Path(os.environ.get("KIRBY_WORK", BASE / "Kirby_build")).resolve()
DEFAULT_CACHE = BASE / "Kirby Data/shader-cache"
OUTPUT = KIRBY_BUILD / "build/shader-libs"
FLAGS = ("-fmetal-math-mode=safe",)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    parser.add_argument(
        "--cache",
        type=Path,
        default=DEFAULT_CACHE,
        help="folder of translated *_b.metal variants (default: Kirby Data/shader-cache)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=OUTPUT,
        help="folder to write compiled .metallib files",
    )
    args = parser.parse_args()

    sources = sorted(args.cache.glob("*_b.metal"))
    if not sources:
        raise SystemExit(f"No translated *_b.metal shaders found in {args.cache}")

    found = subprocess.run(["xcrun", "--find", "metal"], text=True, capture_output=True)
    if found.returncode != 0:
        raise SystemExit("Offline Metal compiler unavailable. Check Xcode / MetalToolchain installation.")
    compiler = found.stdout.strip()

    check = subprocess.run([compiler, "--version"], text=True, capture_output=True)
    if check.returncode != 0 or not check.stdout.splitlines():
        raise SystemExit("Offline Metal compiler is not ready.")
    version = check.stdout.splitlines()[0]
    identity = " ".join((version, *FLAGS))

    args.output.mkdir(parents=True, exist_ok=True)
    stamp = args.output / "compiler.txt"
    if stamp.is_file() and stamp.read_text() != identity:
        print("Notice: Metal compiler identity changed; recompiling libraries...")
    stamp.write_text(identity)

    todo = {}
    for source in sources:
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        library = args.output / f"{digest}.metallib"
        if not library.is_file() or library.stat().st_size < 4:
            todo.setdefault(digest, source)

    print(f"Found {len(sources)} local variants; compiling {len(todo)} missing libraries with {args.jobs} workers...", flush=True)

    def compile_one(item):
        digest, source = item
        partial = args.output / f"{digest}.partial"
        library = args.output / f"{digest}.metallib"
        res = subprocess.run(
            [compiler, *FLAGS, "-o", str(partial), str(source)],
            capture_output=True,
            text=True,
        )
        if res.returncode != 0 or not partial.is_file() or partial.read_bytes()[:4] != b"MTLB":
            partial.unlink(missing_ok=True)
            return f"{source.name}: {res.stdout}{res.stderr}\n"
        partial.replace(library)
        return None

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        failures = [msg for msg in pool.map(compile_one, todo.items()) if msg]

    if failures:
        (args.output / "compile-failures.log").write_text("".join(failures))
        print(f"Compiled with {len(failures)} failures. See {args.output / 'compile-failures.log'}")
        sys.exit(1)

    print(f"Successfully compiled all {len(sources)} shader libraries to {args.output}")


if __name__ == "__main__":
    main()
