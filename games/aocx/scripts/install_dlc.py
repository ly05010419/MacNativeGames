#!/usr/bin/env python3
"""Safely install verified Age of Calamity DLC into the macOS app data directory."""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path, PurePosixPath


GAME_DIR = Path(__file__).resolve().parents[1]
MANIFEST_PATH = GAME_DIR / "assets" / "expected-dlc.json"

DLC_NAMES = {
    1: "Digital Version Preorder Bonus",
    2: "Expansion Pass Purchase Bonus",
    3: "Pulse of the Ancients",
    4: "Guardian of Remembrance DLC",
}


def fail(message):
    raise RuntimeError(message)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(root):
    """Return every regular file as relative path -> (size, SHA-256)."""
    result = {}

    if root.is_symlink() or not root.is_dir():
        fail("Not a regular directory: {}".format(root))

    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            fail("Symbolic links are not allowed: {}".format(path))

        if path.is_dir():
            continue

        if not path.is_file():
            fail("Unsupported filesystem entry: {}".format(path))

        relative = path.relative_to(root).as_posix()
        result[relative] = (path.stat().st_size, sha256_file(path))

    return result


def validate_source(work_dir, index, manifest):
    source = work_dir / "extracted" / "aoc" / str(index)
    report_path = work_dir / "extracted" / "aoc" / "{}.json".format(index)
    expected = manifest["addons"][str(index)]

    if not source.is_dir():
        fail("DLC {} source folder is missing: {}".format(index, source))

    if not (source / "DLCDATA.dat").is_file():
        fail("DLC {} is missing DLCDATA.dat".format(index))

    if not report_path.is_file():
        fail("DLC {} extraction report is missing: {}".format(index, report_path))

    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        fail("Cannot read DLC {} report: {}".format(index, exc))

    if report.get("status") != "pass":
        fail("DLC {} extraction report is not marked pass".format(index))

    if report.get("index") != index:
        fail("DLC {} report has an incorrect index".format(index))

    if report.get("title") != expected["title"]:
        fail("DLC {} Title ID does not match the manifest".format(index))

    if report.get("romfs") != expected["romfs"]:
        fail("DLC {} RomFS verification does not match the manifest".format(index))

    file_list = report.get("file_list")
    if not isinstance(file_list, list) or not file_list:
        fail("DLC {} report has no valid file list".format(index))

    expected_files = {}
    for entry in file_list:
        relative = entry.get("path")
        size = entry.get("size")

        if not isinstance(relative, str) or not isinstance(size, int) or size < 0:
            fail("DLC {} report contains an invalid file entry".format(index))

        rel_path = PurePosixPath(relative)
        if rel_path.is_absolute() or ".." in rel_path.parts:
            fail("DLC {} report contains an unsafe path: {}".format(index, relative))

        if relative in expected_files:
            fail("DLC {} report contains a duplicate path: {}".format(index, relative))

        expected_files[relative] = size

    actual = inventory(source)

    if set(actual) != set(expected_files):
        missing = sorted(set(expected_files) - set(actual))
        extra = sorted(set(actual) - set(expected_files))
        fail(
            "DLC {} file list mismatch. Missing: {}; unexpected: {}".format(
                index, missing[:5], extra[:5]
            )
        )

    for relative, expected_size in expected_files.items():
        actual_size = actual[relative][0]
        if actual_size != expected_size:
            fail(
                "DLC {} file size mismatch: {} (expected {}, got {})".format(
                    index, relative, expected_size, actual_size
                )
            )

    if len(actual) != expected["romfs"]["files"]:
        fail("DLC {} file count does not match the manifest".format(index))

    # RomFS image size is not necessarily the sum of extracted file sizes.
    # The report's RomFS size and SHA-256 were already checked above.

    return source, actual


def main():
    parser = argparse.ArgumentParser(
        description="Install verified Age of Calamity DLC without rebuilding the app."
    )
    parser.add_argument(
        "--work",
        type=Path,
        default=Path.home() / "AoC-Build",
        help="Build work directory (default: ~/AoC-Build)",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=(
            Path.home()
            / "Library"
            / "Application Support"
            / "MacNativeGames"
            / "AOCX"
            / "AddOnContent"
        ),
        help="AddOnContent destination",
    )
    args = parser.parse_args()

    work_dir = args.work.expanduser().resolve()
    destination = args.destination.expanduser()

    if not MANIFEST_PATH.is_file():
        fail("DLC manifest is missing: {}".format(MANIFEST_PATH))

    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    addons = manifest.get("addons", {})

    print("Age of Calamity DLC Installer")
    print("Source:      {}".format(work_dir))
    print("Destination: {}".format(destination))
    print()

    # Validate all four sources before changing the destination.
    sources = {}
    for index in range(1, 5):
        if str(index) not in addons:
            fail("DLC {} is missing from the expected DLC manifest".format(index))

        source, source_inventory = validate_source(work_dir, index, manifest)
        sources[index] = (source, source_inventory)
        print(
            "[OK] DLC {}: {} ({} files)".format(
                index, DLC_NAMES[index], len(source_inventory)
            )
        )

    # Check existing destination content before copying anything.
    if destination.exists() and destination.is_symlink():
        fail("Destination must not be a symbolic link: {}".format(destination))

    for index in range(1, 5):
        target = destination / str(index)

        if target.is_symlink():
            fail("DLC destination is a symbolic link: {}".format(target))

        if target.exists():
            if not target.is_dir():
                fail("DLC destination exists but is not a directory: {}".format(target))

            existing = inventory(target)
            expected = sources[index][1]

            if existing != expected:
                fail(
                    "DLC {} already exists but differs from the verified source. "
                    "Nothing was overwritten: {}".format(index, target)
                )

    destination.mkdir(parents=True, exist_ok=True)

    installed = []
    skipped = []

    for index in range(1, 5):
        source, expected_inventory = sources[index]
        target = destination / str(index)

        if target.exists():
            skipped.append(index)
            print("[SKIP] DLC {}: already installed and verified".format(index))
            continue

        staging_root = Path(
            tempfile.mkdtemp(prefix=".install-dlc-", dir=str(destination))
        )
        staged = staging_root / str(index)

        try:
            shutil.copytree(source, staged)

            copied_inventory = inventory(staged)
            if copied_inventory != expected_inventory:
                fail("DLC {} copy verification failed".format(index))

            # Rename the verified staging directory into its final location.
            os.rename(str(staged), str(target))
            installed.append(index)
            print("[INSTALLED] DLC {}: {}".format(index, DLC_NAMES[index]))

        finally:
            shutil.rmtree(staging_root, ignore_errors=True)

    print()
    print("Installation summary")
    print("--------------------")
    print("Installed: {}".format(", ".join(map(str, installed)) or "None"))
    print("Already installed: {}".format(", ".join(map(str, skipped)) or "None"))
    print("DLC destination: {}".format(destination))
    print("All four DLC folders are present and verified.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, KeyError, ValueError) as exc:
        print("\nERROR: {}".format(exc), file=sys.stderr)
        sys.exit(1)
