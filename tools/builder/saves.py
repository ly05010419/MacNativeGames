"""The saves the builder can back up and restore.

Two runtimes keep saves here: the mainline games write a profile folder the launcher owns
(~/Library/Application Support/MacNativeGames/<TOTK|BotW>/profile, or the folder in the launcher's
*_PROFILE_DIR variable), and nxnative keeps one folder per 16-digit title id
(~/Library/Application Support/nxnative/<title id>). Everything in this module is standard library
copying: a backup is a plain directory of the same files and folder names, beside a backup.json that
says what it holds (game, source, time, every file's size and SHA-256), so a person can also read a
backup with the Finder.

Restore never deletes: it writes the backup's files over the profile and leaves anything the profile
has and the backup does not, reporting both. The window backs the current profile up first
(<game>-pre-restore-<time>) and never removes an older backup.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil

FORMAT = 1
BACKUP_JSON = 'backup.json'
HASH_LIMIT = 2 * 1024 * 1024 * 1024       # larger single files are listed by size only, never read whole

PROFILE_ENV = {'totk': 'TOTK_PROFILE_DIR', 'botw': 'TOTK_PROFILE_DIR'}   # one launcher for both games (app/launcher.m)
NXNATIVE = 'nxnative'


@dataclass(frozen=True)
class SaveFile:
    path: str                       # relative to the saved folder, with '/' separators
    size: int
    sha256: str | None = None


@dataclass(frozen=True)
class SaveSource:
    """One place a game keeps saves: the mainline profile, or an nxnative title folder."""

    game: str                       # the game id ('totk', 'botw') or '' for an unknown nxnative title
    kind: str                       # 'profile' | 'nxnative'
    title_id: str
    path: Path
    exists: bool
    files: tuple = ()
    bytes: int = 0
    protected: tuple = ()           # the game file and app a clean/restore must never touch


@dataclass(frozen=True)
class Backup:
    directory: Path
    game: str
    source: str
    created: str
    files: tuple
    bytes: int
    title_id: str = ''


@dataclass(frozen=True)
class RestorePlan:
    """What a restore writes, what it leaves alone, and what is already the same."""

    write: tuple = ()
    same: tuple = ()
    kept: tuple = ()
    target_exists: bool = True


def timestamp(now):
    return datetime.fromtimestamp(now).strftime('%Y%m%d-%H%M%S')


def default_backup_root(home):
    return Path(home) / 'Documents' / 'MacNativeGames-Backups'


def profile_dir(spec, home, environ=None):
    environ = os.environ if environ is None else environ
    value = environ.get(PROFILE_ENV.get(spec.id, ''))
    if value:
        return Path(value).expanduser()
    return Path(home) / 'Library/Application Support/MacNativeGames' / spec.data_folder / 'profile'


def nxnative_root(home):
    return Path(home) / 'Library/Application Support' / NXNATIVE


def nxnative_dirs(home, titles=()):
    """The title folders nxnative has actually written, as (title id, path), newest first; a name
    that is not a 16-digit hex title id is not a title folder and is skipped."""
    root = nxnative_root(home)
    titles = {t.upper() for t in titles}
    found = []
    try:
        entries = list(os.scandir(root))
    except OSError:
        return ()
    for entry in entries:
        name = entry.name.upper()
        if len(name) != 16 or any(c not in '0123456789ABCDEF' for c in name):
            continue
        if entry.is_dir(follow_symlinks=False):
            found.append((name, Path(entry.path)))
    return tuple(sorted(found, key=lambda pair: pair[1].name))


def list_files(path):
    """(SaveFile, ...) under a folder, without following symlinks; one unreadable file keeps the
    rest of the list (it is left out and appears in the count the caller can compare)."""
    path = Path(path)
    if not path.is_dir():
        return ()
    files = []
    for root, folders, names in os.walk(path, followlinks=False):
        folders[:] = sorted(f for f in folders if not Path(root, f).is_symlink())
        for name in sorted(names):
            item = Path(root, name)
            if item.is_symlink():
                continue
            try:
                size = item.stat().st_size
            except OSError:
                continue
            files.append(SaveFile(item.relative_to(path).as_posix(), size))
    return tuple(files)


def source(spec, home, environ=None):
    """The mainline profile for a game, whether or not it exists yet."""
    path = profile_dir(spec, home, environ)
    files = list_files(path)
    return SaveSource(spec.id, 'profile', spec.title_id, path, path.is_dir(), files,
                      sum(f.size for f in files))


def nxnative_sources(home, titles=('0100F2C0115B6000', '01007EF00011E000')):
    """Every nxnative title folder found, the two the games use marked with their game id."""
    known = {'0100F2C0115B6000': 'totk', '01007EF00011E000': 'botw'}
    found = []
    for title_id, path in nxnative_dirs(home, titles):
        files = list_files(path)
        found.append(SaveSource(known.get(title_id, ''), 'nxnative', title_id, path, True, files,
                                sum(f.size for f in files)))
    return tuple(found)


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _inside(child, parent):
    child, parent = Path(child).resolve(), Path(parent).resolve()
    return child == parent or parent in child.parents


def backup(source_path, root, label, game, now, title_id='', with_hash=True):
    """Copy a save folder into a new <label>-<time> folder under root, with backup.json beside the
    files. Refuses a root inside the source (that would copy the backup into itself) and an
    existing folder (never overwrites a backup). Returns (Backup, None) or (None, reason)."""
    source_path = Path(source_path).expanduser()
    root = Path(root).expanduser()
    if not source_path.is_dir():
        return None, 'source_missing'
    if _inside(root, source_path):
        return None, 'root_inside_source'
    directory = root / f'{label}-{timestamp(now)}'
    if directory.exists():
        return None, 'backup_exists'
    try:
        directory.mkdir(parents=True)
    except OSError as error:
        return None, error.strerror or str(error)
    files, copied, failures = [], [], []
    try:
        for entry in list(os.scandir(source_path)):
            target = directory / entry.name
            if entry.is_dir(follow_symlinks=False):
                shutil.copytree(entry.path, target, symlinks=False)
            elif entry.is_file(follow_symlinks=False):
                shutil.copy2(entry.path, target)
            else:
                continue                        # a broken link or a socket: not a save file
        for saved in list_files(directory):
            item = directory / saved.path
            hashed = None
            if with_hash and saved.size <= HASH_LIMIT:
                try:
                    hashed = _sha256(item)
                except OSError:
                    hashed = None
            files.append(SaveFile(saved.path, saved.size, hashed))
    except (OSError, shutil.Error) as error:
        return None, error.strerror if isinstance(error, OSError) else str(error)
    manifest = {
        'format': FORMAT, 'game': game, 'title_id': title_id, 'source': str(source_path),
        'created': datetime.fromtimestamp(now).isoformat(timespec='seconds'),
        'files': [{'path': f.path, 'size': f.size, 'sha256': f.sha256} for f in files],
    }
    try:
        (directory / BACKUP_JSON).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n',
                                             encoding='utf-8')
    except OSError as error:
        return None, error.strerror or str(error)
    return Backup(directory, game, str(source_path), manifest['created'], tuple(files),
                  sum(f.size for f in files), title_id), None


def _relative(path):
    """True for a plain relative path with '/' separators and no '..' or empty part."""
    parts = path.split('/')
    return bool(path) and not path.startswith('/') and '\\' not in path and all(p not in ('', '.', '..') for p in parts)


def read_backup(directory):
    """A backup folder's manifest as a Backup, or None when it is not a backup the builder wrote."""
    directory = Path(directory)
    try:
        data = json.loads((directory / BACKUP_JSON).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or data.get('format') != FORMAT or not isinstance(data.get('files'), list):
        return None
    files = []
    for item in data['files']:
        if not isinstance(item, dict) or not isinstance(item.get('path'), str) or not isinstance(item.get('size'), int):
            return None
        if not _relative(item['path']):
            return None                         # a path that would leave the backup or the save folder
        sha = item.get('sha256')
        files.append(SaveFile(item['path'], item['size'], sha if isinstance(sha, str) else None))
    return Backup(directory, str(data.get('game', '')), str(data.get('source', '')),
                  str(data.get('created', '')), tuple(files), sum(f.size for f in files),
                  str(data.get('title_id', '')))


def plan_restore(backup, target):
    """What restoring backup into target writes, leaves, and finds already equal. Comparison uses
    the manifest's SHA-256 when it has one, otherwise the size, and says which one was used by the
    tuple it returns: write = (SaveFile, ...), kept = (relative path, ...)."""
    target = Path(target)
    current = {f.path: f for f in list_files(target)}
    write, same = [], []
    for saved in backup.files:
        now = current.get(saved.path)
        if now is None or now.size != saved.size:
            write.append(saved)
        elif saved.sha256 is None:
            write.append(saved)                 # size matches but content is unproven: write it
        else:
            try:
                equal = _sha256(target / saved.path) == saved.sha256
            except OSError:
                equal = False
            (same if equal else write).append(saved)
    kept = tuple(sorted(path for path in current if path not in {f.path for f in backup.files}))
    return RestorePlan(tuple(write), tuple(same), kept, target.is_dir())


def restore(backup, target):
    """Write the backup's files over the target, then its folders. Nothing is removed. Returns
    (RestorePlan, error)."""
    plan = plan_restore(backup, target)
    target = Path(target)
    try:
        target.mkdir(parents=True, exist_ok=True)
        for saved in plan.write:
            src, dst = Path(backup.directory) / saved.path, target / saved.path
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    except (OSError, shutil.Error) as error:
        return plan, error.strerror if isinstance(error, OSError) else str(error)
    return plan, None


def find_backups(root):
    """(Backup, ...) the backups under a root, newest first; an unreadable folder is left out."""
    root = Path(root).expanduser()
    try:
        entries = sorted((Path(e.path) for e in os.scandir(root) if e.is_dir(follow_symlinks=False)),
                         key=lambda p: p.name, reverse=True)
    except OSError:
        return ()
    found = []
    for directory in entries:
        item = read_backup(directory)
        if item is not None:
            found.append(item)
    return tuple(found)
