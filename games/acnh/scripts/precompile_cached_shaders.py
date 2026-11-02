#!/usr/bin/env python3
"""Compile locally translated Animal Crossing: New Horizons shaders into .metallib before app launch.

The runtime looks up each library by SHA-256 of the exact MSL source bytes
in `shader-libs` (next to the executable in Contents/MacOS and build/shader-libs).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid

GAME_DIR = Path(__file__).resolve().parents[1]
DEFAULT_WORK_DIR = GAME_DIR.parents[2] / "ACNH_build/native-v1.2.1"
if not DEFAULT_WORK_DIR.exists():
    DEFAULT_WORK_DIR = GAME_DIR.parents[2] / "ACNH_build"
ACNH_BUILD = Path(os.environ.get("ACNH_WORK", DEFAULT_WORK_DIR)).resolve()
if (ACNH_BUILD / "native-v1.2.1/build").is_dir():
    BUILD_DIR = ACNH_BUILD / "native-v1.2.1/build"
elif (ACNH_BUILD / "build").is_dir():
    BUILD_DIR = ACNH_BUILD / "build"
else:
    BUILD_DIR = ACNH_BUILD

DEFAULT_CACHE = BUILD_DIR / "shader-cache"
OUTPUT = BUILD_DIR / "shader-libs"
FLAGS = ("-fmetal-math-mode=safe",)


def valid_library(path: Path) -> bool:
    """Reject empty/truncated markers and files that are not Metal libraries."""
    try:
        with path.open("rb") as stream:
            return path.stat().st_size > 4 and stream.read(4) == b"MTLB"
    except OSError:
        return False


def worker_count(value: str) -> int:
    count = int(value)
    if not 1 <= count <= 16:
        raise argparse.ArgumentTypeError("--jobs must be between 1 and 16")
    return count


def find_sources(cache: Path):
    """Find translated shader sources under cache directory."""
    if not cache.exists():
        return []
    # Support both flat and nested caches (e.g. rt-totk-native-v4-unverified)
    sources = sorted(set(cache.rglob("*_b.metal")))
    return sources


def compile_cache(cache: Path, output: Path, compiler: str, identity: str, jobs: int):
    """Stage all results before publication; a failed compile keeps the old cache."""
    sources = find_sources(cache)
    if not sources:
        raise SystemExit(f"No translated *_b.metal shaders found in {cache}")
    output.mkdir(parents=True, exist_ok=True)
    with (output / ".compile.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stamp = output / "compiler.txt"
        same_compiler = stamp.is_file() and stamp.read_text() == identity
        if not same_compiler:
            print("Compiler identity absent or changed; rebuilding the selected libraries.")
        todo = {}
        for source in sources:
            data = source.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if not same_compiler or not valid_library(output / f"{digest}.metallib"):
                todo.setdefault(digest, (source.name, data))
        print(f"Found {len(sources)} local shader sources; compiling {len(todo)} libraries with {jobs} workers...", flush=True)

        with tempfile.TemporaryDirectory(prefix=".compile-", dir=output) as temporary:
            staged = Path(temporary)

            def compile_one(item):
                digest, (name, data) = item
                source = staged / f"{digest}.metal"
                library = staged / f"{digest}.metallib"
                source.write_bytes(data)
                try:
                    result = subprocess.run(
                        [compiler, *FLAGS, "-o", str(library), str(source)],
                        capture_output=True, text=True,
                    )
                except OSError as error:
                    return f"{name}: {error}\n"
                if result.returncode != 0 or not valid_library(library):
                    return f"{name}: invalid compiler output (exit {result.returncode})\n{result.stdout}{result.stderr}\n"
                return None

            with ThreadPoolExecutor(max_workers=jobs) as pool:
                failures = [message for message in pool.map(compile_one, todo.items()) if message]
            if failures:
                (output / "compile-failures.log").write_text("".join(failures))
                raise SystemExit(f"{len(failures)} shader compilations failed; previous libraries and identity retained. See {output / 'compile-failures.log'}")

            if todo or not same_compiler:
                stamp.unlink(missing_ok=True)
                if not same_compiler:
                    old = list(output.glob("*.metallib"))
                    if old:
                        backup = output / "compiler-backups" / uuid.uuid4().hex
                        backup.mkdir(parents=True)
                        for library in old:
                            library.replace(backup / library.name)
                for digest in todo:
                    (staged / f"{digest}.metallib").replace(output / f"{digest}.metallib")
                new_stamp = staged / "compiler.txt"
                new_stamp.write_text(identity)
                new_stamp.replace(stamp)
            (output / "compile-failures.log").unlink(missing_ok=True)
        print(f"Successfully prepared {len(sources)} shader libraries in {output}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=worker_count, default=min(8, os.cpu_count() or 4))
    parser.add_argument(
        "--cache",
        type=Path,
        default=DEFAULT_CACHE,
        help="folder of translated *_b.metal variants (default: ACNH_WORK/build/shader-cache)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=OUTPUT,
        help="folder to write compiled .metallib files",
    )
    args = parser.parse_args(argv)

    found = subprocess.run(["xcrun", "--find", "metal"], text=True, capture_output=True)
    if found.returncode != 0:
        raise SystemExit("Offline Metal compiler unavailable. Check Xcode / MetalToolchain installation.")
    compiler = found.stdout.strip()

    check = subprocess.run([compiler, "--version"], text=True, capture_output=True)
    if check.returncode != 0 or not check.stdout.splitlines():
        raise SystemExit("Offline Metal compiler is not ready.")
    identity = json.dumps({
        "schema": 1, "compiler_version": check.stdout.strip(), "flags": FLAGS,
        "deployment_target": os.environ.get("MACOSX_DEPLOYMENT_TARGET"),
        "sdkroot": os.environ.get("SDKROOT"),
    }, sort_keys=True) + "\n"
    compile_cache(args.cache, args.output, compiler, identity, args.jobs)


if __name__ == "__main__":
    main()
