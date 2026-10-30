"""The two caches the builder shows and cleans: the build's work folder and the game's shader cache.

The work folder is build_app.py's: by default <game file's folder>/<work folder name> (TOTK_build,
BotW_build), or the folder the build was given with --work; the builder remembers the last one in
settings.json (SavedPaths.work). The shader cache is the game launcher's: <Application Support>/
MacNativeGames/<TOTK|BotW>/shader-cache, holding one rt-<chain> folder per Metal pipeline chain, and
the launcher's TOTK_SHADER_CACHE environment variable moves it (games/totk/app/launcher.m; Breath of
the Wild's launcher is the same program and reads the same name). The builder does not move it: the
launcher reads no folder chosen here.

Everything here is read-only or explicitly confirmed by the caller: measure() only walks, clean()
removes a folder's contents (never the folder, never a game, save or app) and move() moves one
folder's contents. The guards in refuse_reason() are the safety rules; a caller that wants to
delete something shows the reason instead. This module has no Tk: the window runs measure() and
move() on a thread and shows the result.
"""
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil

# Environment variables the game's launcher reads for the shader cache, in the order it checks them.
SHADER_ENV = {'totk': ('TOTK_SHADER_CACHE',), 'botw': ('TOTK_SHADER_CACHE',)}

# Top-level folders a cache never lives under, so a bad path can never clean the system.
SYSTEM = frozenset({'System', 'Library', 'Applications', 'usr', 'bin', 'sbin', 'etc', 'private', 'var', 'dev'})


@dataclass(frozen=True)
class Size:
    """A recursive count: bytes and files under a folder, and the paths that could not be read."""

    bytes: int = 0
    files: int = 0
    errors: tuple = ()

    @property
    def clean(self) -> bool:
        return not self.errors


def measure(path):
    """Walk a folder without following symlinks; a missing folder counts as empty. Errors are kept,
    never raised: one unreadable file must not hide the rest of the count."""
    path = Path(path)
    total, count, errors = 0, 0, []
    if not path.is_dir():
        return Size()
    for root, folders, files in os.walk(path, followlinks=False):
        folders[:] = [f for f in folders if not Path(root, f).is_symlink()]
        for name in files:
            item = Path(root, name)
            try:
                if item.is_symlink():
                    continue
                total += item.stat().st_size
                count += 1
            except OSError as error:
                errors.append(f'{item}: {error.strerror or error}')
    return Size(total, count, tuple(errors))


def work_dir(spec, game_file, saved):
    """The work folder a build uses: the saved --work path when there is one, else the game's
    default next to its file (paths.effective_paths' rule)."""
    if saved:
        return Path(saved).expanduser()
    game = Path(game_file).expanduser() if game_file else None
    if game is None:
        return None
    return game.parent / spec.work_folder_name


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
