#!/usr/bin/env python3
"""Import/export an unpacked TOTK save without changing its contents.

Use the folder opened by Ryujinx, Yuzu or Eden's Open Save Data Location.
This deliberately does not search emulator NANDs or choose between users.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import struct
import tempfile
import sys

DATA = Path.home() / "Library/Application Support/MacNativeGames/TOTK"
SLOT = re.compile(r"slot_0[0-5]\Z")


def canonical_path(path):
    path = Path(path).expanduser().absolute()
    if path.is_symlink():
        raise ValueError("Symbolic links are not supported: " + str(path))
    # macOS /var and /tmp are links. Canonicalize ancestors before overlap
    # checks and mutations, while refusing a linked source/destination itself.
    return path.resolve()


def checked_tree(root):
    """Reject linked roots, linked children and special files."""
    root = canonical_path(root)
    if not root.is_dir():
        raise ValueError("Not a directory: " + str(root))
    for path in root.rglob("*"):
        mode = path.lstat().st_mode
        if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise ValueError("Link or special file in save/profile: " + str(path))
    return root


def validate_save(source):
    source = checked_tree(source)
    complete = []
    for path in sorted(source.iterdir()):
        if not path.is_dir() or not SLOT.fullmatch(path.name):
            continue
        progress, caption = path / "progress.sav", path / "caption.sav"
        if not any(path.iterdir()):  # empty unused slots are harmless
            continue
        if not all(p.is_file() and p.stat().st_size > 0 for p in (progress, caption)):
            raise ValueError("Incomplete TOTK save slot: " + str(path))
        complete.append(path.name)
    if not complete:
        raise ValueError("Select the TOTK save root containing slot_00 … slot_05, "
                         "with progress.sav and caption.sav inside a populated slot")
    return source, complete


def unrelated(a, b):
    a, b = a.resolve(), b.resolve()
    if a == b or a in b.parents or b in a.parents:
        raise ValueError("Source and destination must not overlap")


def validate_lock_location(slot_dir, *trees):
    slot_dir = canonical_path(slot_dir)
    for tree in trees:
        tree = canonical_path(tree)
        if tree == slot_dir or tree in slot_dir.parents:
            raise ValueError("The game-slot lock must be outside the profile and copied save folders")
    return slot_dir


def exchange_directories(first, second):
    """Darwin's atomic directory exchange keeps the live profile present."""
    if sys.platform != 'darwin':
        raise ValueError("Replacing an existing profile requires macOS atomic directory exchange")
    exchange = ctypes.CDLL(None, use_errno=True).renamex_np
    exchange.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    exchange.restype = ctypes.c_int
    if exchange(os.fsencode(first), os.fsencode(second), 0x00000002):  # RENAME_SWAP, <stdio.h>
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(second))


def account_folder(profile):
    raw = (profile / "account.uid").read_bytes()
    if len(raw) != 16 or not any(raw):
        raise ValueError("Invalid account.uid; launch the app once to create a profile")
    lo, hi = struct.unpack("<QQ", raw)
    return "%016x%016x" % (hi, lo)


def tree_manifest(root):
    """Hash the whole unpacked save, including options and empty directories."""
    root = checked_tree(root)
    manifest = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        if path.is_dir():
            manifest[name + '/'] = None
        else:
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            manifest[name] = digest.hexdigest()
    return manifest


@contextmanager
def game_lock(slot_dir):
    """Use the same flock target as runtime/game_slot.c, held through the swap."""
    slot_dir = canonical_path(slot_dir)
    slot_dir.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(slot_dir / "lock"), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("The game is running; quit it before migrating saves") from None
        yield
    finally:
        os.close(fd)


def import_save(source, profile, slot_dir, dry_run=False):
    source, slots = validate_save(source)
    profile = canonical_path(profile)
    unrelated(source, profile)
    slot_dir = validate_lock_location(slot_dir, source, profile)
    # Validate before creating anything, including a new profile's ancestors.
    exists = profile.exists()
    if exists:
        checked_tree(profile)
        account_folder(profile)  # an existing identity is never regenerated
    report = {"action": "import", "source": str(source), "profile": str(profile),
              "slots": slots, "dry_run": dry_run}
    if dry_run:
        return report
    with game_lock(slot_dir):
        # Recheck after taking the lock, before inspecting account or copying saves.
        exists = profile.exists()
        if exists:
            checked_tree(profile)
            account_folder(profile)
        profile.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".totk-save-import-", dir=str(profile.parent)))
        backup = None
        try:
            before = tree_manifest(source)
            if exists:
                shutil.copytree(profile, staging, dirs_exist_ok=True)
            else:
                raw = bytes(16)
                while not any(raw):
                    raw = secrets.token_bytes(16)
                identity = staging / "account.uid"
                identity.write_bytes(raw)
                identity.chmod(0o600)
            destination = staging / "save" / account_folder(staging)
            if destination.exists():
                shutil.rmtree(destination)
            shutil.copytree(source, destination)
            validate_save(destination)
            if tree_manifest(destination) != before or tree_manifest(source) != before:
                raise ValueError("Source save changed or was not copied completely; quit the emulator and retry")
            if exists:
                stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
                backup = profile.with_name(profile.name + "-before-import-" + stamp + "-" + secrets.token_hex(4))
                # Publish a verified backup before atomically replacing the
                # live profile. It remains valid if this process is interrupted.
                old = tree_manifest(profile)
                backup_copy = Path(tempfile.mkdtemp(prefix='.totk-save-backup-', dir=str(profile.parent)))
                try:
                    shutil.copytree(profile, backup_copy, dirs_exist_ok=True)
                    if tree_manifest(backup_copy) != old or tree_manifest(profile) != old:
                        raise ValueError("Profile changed while it was backed up; import was cancelled")
                    backup_copy.rename(backup)
                finally:
                    if backup_copy.exists():
                        shutil.rmtree(backup_copy)
                exchange_directories(staging, profile)
            else:
                staging.rename(profile)
            report["backup"] = str(backup) if backup else None
            report["destination"] = str(profile / "save" / account_folder(profile))
        finally:
            if staging.exists():
                shutil.rmtree(staging)
    return report


def export_save(destination, profile, slot_dir, dry_run=False):
    profile = checked_tree(profile)
    destination = canonical_path(destination)
    unrelated(profile, destination)
    slot_dir = validate_lock_location(slot_dir, profile, destination)
    if destination.exists():
        raise ValueError("Export destination must be a new directory")
    report = {"action": "export", "destination": str(destination), "dry_run": dry_run}
    if dry_run:
        source, slots = validate_save(profile / "save" / account_folder(profile))
        report.update(source=str(source), slots=slots)
        return report
    with game_lock(slot_dir):
        source, slots = validate_save(profile / "save" / account_folder(profile))
        before = tree_manifest(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".totk-save-export-", dir=str(destination.parent)))
        try:
            shutil.copytree(source, staging, dirs_exist_ok=True)
            validate_save(staging)
            if tree_manifest(staging) != before or tree_manifest(source) != before:
                raise ValueError("Save changed or was not copied completely; export was cancelled")
            # Do not replace a destination created since the preflight check.
            destination.mkdir()
            try:
                staging.rename(destination)
            except BaseException:
                destination.rmdir()
                raise
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        report.update(source=str(source), slots=slots)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("import", "export"))
    parser.add_argument("folder", type=Path, help="emulator save root to import, or new export folder")
    parser.add_argument("--profile", type=Path, default=Path(os.environ.get("TOTK_PROFILE_DIR", DATA / "profile")))
    parser.add_argument("--slot-dir", type=Path, default=Path(os.environ.get("TOTK_GAME_SLOT_DIR", DATA / "game-slot")),
                        help="must match the app's game-slot directory")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        operation = import_save if args.action == "import" else export_save
        report = operation(args.folder, args.profile, args.slot_dir, args.dry_run)
    except (OSError, ValueError) as error:
        parser.exit(1, "Save migration failed: %s\n" % error)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
