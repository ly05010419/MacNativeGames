"""The two caches the builder shows and cleans: the build's work folder and the game's shader cache.

The work folder is build_app.py's: by default build_app.default_work (the folder the app records, else
MacNativeGames-work/<game> beside the folder of the app), or the folder the build was given with --work; MNG Launcher's
Storage page (tools/builder_jobs/tab_storage.py) finds it. The shader cache is the game launcher's: <Application Support>/
MacNativeGames/<TOTK|BotW>/shader-cache, holding one rt-<chain> folder per Metal pipeline chain, and
the launcher's TOTK_SHADER_CACHE environment variable moves it (games/totk/app/launcher.m; Breath of
the Wild's launcher is the same program and reads the same name). The builder does not move it: the
launcher reads no folder chosen here.

Everything here is read-only or explicitly confirmed by the caller: measure() only walks, clean()
removes a folder's contents (never the folder, never a game, save or app) and move() moves one
folder's contents. The guards in refuse_reason() are the safety rules; a caller that wants to
delete something shows the reason instead. This module has no window: the Storage page calls it and shows the
result.
"""
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import stat

# Environment variables the game's launcher reads for the shader cache, in the order it checks them.
SHADER_ENV = {'totk': ('TOTK_SHADER_CACHE',), 'botw': ('TOTK_SHADER_CACHE',), 'oot': ('TOTK_SHADER_CACHE',),
              'acnh': ('TOTK_SHADER_CACHE',), 'eow': ('TOTK_SHADER_CACHE',),
              'odyssey': ('TOTK_SHADER_CACHE',)}   # games/acnh/scripts/acnh_app_launcher.c, games/eow/app/launcher.c   # OOT: games/oot/app/launcher.m

# Top-level folders a cache never lives under, so a bad path can never clean the system.
SYSTEM = frozenset({'System', 'Library', 'Applications', 'usr', 'bin', 'sbin', 'etc', 'private', 'var', 'dev'})


@dataclass(frozen=True)
class Size:
    """A recursive count: bytes and files under a folder, and the paths that could not be read. own: the bytes on disk
    that nothing outside the folder shares, which removing it would free (None where the disk cannot tell): an APFS clone
    shares its blocks (2026-11-18: Ocarina of Time's work folder held 79.9 GB of files, 1.7 GB of them its own; the game
    data was the same blocks as the app's)."""

    bytes: int = 0
    files: int = 0
    errors: tuple = ()
    own: int | None = None

    @property
    def clean(self) -> bool:
        return not self.errors


def measure(path, skip=None):
    """Walk a folder without following symlinks; a missing folder counts as empty. Errors are kept,
    never raised: one unreadable file must not hide the rest of the count. skip(folder path) -> True leaves a folder out
    (a work folder's trash, which a background delete is emptying). own counts each file's private bytes (private_size)
    once per inode, and a hard-linked file only when all its links are in the folder."""
    path = Path(path)
    total, count, errors = 0, 0, []
    own, linked = 0, {}   # linked: (dev, inode) -> [links, links seen here, private bytes]
    if not path.is_dir():
        return Size(own=0)
    for root, folders, files in os.walk(path, followlinks=False):
        folders[:] = [f for f in folders if not Path(root, f).is_symlink() and not (skip and skip(Path(root, f)))]
        for name in files:
            item = Path(root, name)
            try:
                st = item.lstat()
                if not stat.S_ISREG(st.st_mode):
                    continue
                total += st.st_size
                count += 1
            except OSError as error:
                errors.append(f'{item}: {error.strerror or error}')
                continue
            if own is None:
                continue
            mine = private_size(item)
            if mine is None:          # the disk cannot tell (not APFS): no own size at all rather than a wrong one
                own = None
            elif st.st_nlink == 1:
                own += mine
            else:
                seen = linked.setdefault((st.st_dev, st.st_ino), [st.st_nlink, 0, mine])
                seen[1] += 1
    if own is not None:
        own += sum(mine for links, seen, mine in linked.values() if seen >= links)
    return Size(total, count, tuple(errors), own)


_ATTR_CMN_RETURNED_ATTRS = 0x80000000
_ATTR_CMNEXT_PRIVATESIZE = 0x00000008
_FSOPT_NOFOLLOW, _FSOPT_ATTR_CMN_EXTENDED = 0x1, 0x20


def private_size(path):
    """The bytes of a file that no other file shares (getattrlist ATTR_CMNEXT_PRIVATESIZE, macOS 10.15 on: an APFS clone
    shares its blocks until written; 2026-11-18, a fresh 8 MB file and its clone both 0, a plain copy 8 MB), or None
    where the disk does not report it."""
    import ctypes
    import struct

    class AttrList(ctypes.Structure):
        _fields_ = [('bitmapcount', ctypes.c_ushort), ('reserved', ctypes.c_ushort), ('commonattr', ctypes.c_uint32),
                    ('volattr', ctypes.c_uint32), ('dirattr', ctypes.c_uint32), ('fileattr', ctypes.c_uint32),
                    ('forkattr', ctypes.c_uint32)]   # forkattr holds the extended common attributes (FSOPT_ATTR_CMN_EXTENDED)

    global _getattrlist
    if _getattrlist is None:
        try:
            _getattrlist = ctypes.CDLL(None, use_errno=True).getattrlist
        except (OSError, AttributeError):
            _getattrlist = False
    if not _getattrlist:
        return None
    request = AttrList(5, 0, _ATTR_CMN_RETURNED_ATTRS, 0, 0, 0, _ATTR_CMNEXT_PRIVATESIZE)
    answer = ctypes.create_string_buffer(64)
    if _getattrlist(os.fsencode(str(path)), ctypes.byref(request), answer, len(answer),
                    _FSOPT_NOFOLLOW | _FSOPT_ATTR_CMN_EXTENDED):
        return None
    returned = struct.unpack_from('<5I', answer, 4)   # after the u32 length: the attributes the disk gave
    if not returned[4] & _ATTR_CMNEXT_PRIVATESIZE:
        return None
    return struct.unpack_from('<q', answer, 24)[0]


_getattrlist = None


def default_shader_dir(spec, home):
    return Path(home) / 'Library/Application Support/MacNativeGames' / spec.data_folder / 'shader-cache'


def shader_env(spec):
    """(the variable that moves this cache, the variables also accepted)."""
    names = SHADER_ENV.get(spec.id, (f'{spec.data_folder.upper()}_SHADER_CACHE',))
    return names[0], names


def shader_dir(spec, home, environ=None):
    """Where the launcher writes this game's shader cache: its environment variable when set, then the
    fixed folder."""
    environ = os.environ if environ is None else environ
    for name in SHADER_ENV.get(spec.id, ()):
        value = environ.get(name)
        if value:
            return Path(value).expanduser()
    return default_shader_dir(spec, home)


def refuse_reason(path, home, protected=()):
    """Why this folder may not be cleaned or moved, or None when it may. The rules: never a root,
    never the home folder or one of its parents, never a folder holding (or held by) a protected
    path -- the game file, the app, the save -- and never a system folder."""
    path = Path(path)
    if not path.exists():
        return 'missing'
    if path.is_symlink():
        return 'symlink'
    resolved = path.resolve()
    if not path.is_dir():
        return 'file'
    if resolved == Path(resolved.anchor):
        return 'root'
    home = Path(home).resolve()
    if resolved == home or resolved in home.parents:      # the home folder itself, or one of its parents
        return 'home'
    parts = resolved.parts
    if len(parts) > 1 and parts[1] in SYSTEM:
        return 'system'
    for item in protected:
        if item is None:
            continue
        item = Path(item).expanduser().resolve()
        if resolved == item or item in resolved.parents or resolved in item.parents:
            return 'protected'
    return None


def clean(path, home, protected=()):
    """Empty a folder, keeping the folder itself (the game makes it again). Returns (what was
    removed, error). Everything removed is counted first, so the window can say what it did."""
    reason = refuse_reason(path, home, protected)
    if reason is not None:
        return Size(), reason
    path = Path(path)
    removed, errors = Size(), []
    try:
        entries = list(os.scandir(path))
    except OSError as error:
        return Size(), error.strerror or str(error)
    for entry in entries:
        item = Path(entry.path)
        try:
            size = measure(item) if entry.is_dir(follow_symlinks=False) else Size(
                entry.stat(follow_symlinks=False).st_size, 1)
            if entry.is_dir(follow_symlinks=False):
                shutil.rmtree(item)
            else:
                item.unlink()
            removed = Size(removed.bytes + size.bytes, removed.files + size.files, removed.errors)
        except OSError as error:
            errors.append(f'{item}: {error.strerror or error}')
    return (removed, None) if not errors else (removed, '; '.join(errors))


def move(src, dst):
    """Move a folder's contents into another folder, creating it. Cross-volume moves are copied and
    the source removed by shutil.move. Returns (how many entries moved, error). The target must be
    absent or empty, so a second move never mixes two caches."""
    src, dst = Path(src), Path(dst)
    if not src.is_dir():
        return 0, 'source_missing'
    if src.resolve() == dst.resolve() or src.resolve() in dst.resolve().parents:
        return 0, 'inside_source'
    if dst.exists() and (not dst.is_dir() or any(dst.iterdir())):
        return 0, 'target_not_empty'
    try:
        dst.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        return 0, error.strerror or str(error)
    moved, errors = 0, []
    for entry in list(os.scandir(src)):
        try:
            shutil.move(entry.path, dst / entry.name)
            moved += 1
        except (OSError, shutil.Error) as error:
            errors.append(f'{entry.name}: {error}')
    return (moved, None) if not errors else (moved, '; '.join(errors))


def human(size):
    """A size a person reads at a glance: 0 B, 512 B, 12.3 KB, 1.4 GB."""
    value = float(size)
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if value < 1024 or unit == 'TB':
            return f'{value:.0f} {unit}' if unit == 'B' else f'{value:.1f} {unit}'
        value /= 1024
    return f'{value:.1f} TB'
