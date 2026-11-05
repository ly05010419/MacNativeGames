#!/usr/bin/env python3
"""Replace a player's RomFS mod folder for one game version with a mod's romfs/, and write its manifest.

The app loads a RomFS mod while it plays, from
~/Library/Application Support/MacNativeGames/TOTK/mods/<version> (runtime/romfs.c through TOTK_ROMFS_MODS). A build with
--mod merges the mod's code patches; this module puts the data half there, and only when the build was asked to:
build_app.py --install-mod-romfs says that folder may be replaced. Nothing here runs for a build that leaves the flag out.

The replacement is built first, in a folder next to the player's (<folder>.new): the mod's romfs/ is copied there and
scripts/make_mod_manifest.py writes the manifest there against the app the build just made, so the paths the app replaces
itself are left out (the heap table included: the 2K overlay carries the mod's heap sizes). Only then is the player's
folder moved aside and the new one renamed into place, so the folder holds either what it held before or the complete new
copy - never a half-copied one. What the folder held is kept, not deleted: it stays next to it as
<folder>.backup-<date>-<time> (another mod, files copied by hand), and the build names that folder; the game never loads
it. The build says what it is about to do before it starts.

Two checks run before anything of the player's is touched: make_mod_manifest.py --check on the mod folder itself (a
misspelt path stops the build there), and a look at what that manifest would hold. A mod whose files would all be left
out - every one identical to the original, or one the app replaces itself - replaces nothing the game loads, so the
player's folder is left as it is and no manifest is written.

Nothing outside the folder is touched: the move never follows a symlink out of it, a symlinked folder is refused, and the
folder is the version's own (one game version, one mod set).
"""
from collections import namedtuple
from pathlib import Path
import re
import shutil
import subprocess
import time


def mods_folder(version):
    """<data>/mods/<version>: where the launcher points the game for a player's RomFS mods (runtime/romfs.c,
    TOTK_ROMFS_MODS). <data> is the game's own folder (app/launcher.m, GAME_FOLDER): BotW for Breath of the Wild's
    versions, TOTK for the others."""
    game = 'BotW' if version.startswith('botw') else 'TOTK'
    return Path.home() / 'Library/Application Support/MacNativeGames' / game / 'mods' / version


class InstallError(RuntimeError):
    """The mod folder would stop the game at start, or could not be written. `output` holds what the tool said, so the
    build can write it to its log."""

    def __init__(self, message, output=''):
        super().__init__(message)
        self.output = output


Installed = namedtuple('Installed', 'folder copied backup manifest')
Installed.__doc__ = """What one install did.

    folder     the player's mod folder that was replaced
    copied     how many files were copied out of the mod's romfs/
    backup     where what the folder held before was kept (<folder>.backup-<date>-<time>), or None when it held nothing
    manifest   the manifest tool's output lines"""


def files_below(folder):
    """Every file under `folder` (symlinks to files included, symlinks to folders not followed)."""
    return sorted(p for p in folder.rglob('*') if p.is_file())


def copy_tree(source, dest):
    """Copy every file under `source` into `dest`, keeping modification times; returns the count."""
    copied = 0
    for file in files_below(source):
        target = dest / file.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, target)
        copied += 1
    return copied


def backup_name(folder):
    """<folder>.backup-<date>-<time> next to the folder, with -2, -3, ... when that name is taken (never an existing one)."""
    base = folder.with_name(f'{folder.name}.backup-{time.strftime("%Y%m%d-%H%M%S")}')
    candidate, n = base, 1
    while candidate.exists() or candidate.is_symlink():
        n += 1
        candidate = base.with_name(f'{base.name}-{n}')
    return candidate


def _manifest(py, tree, folder, app, version, check):
    """Run scripts/make_mod_manifest.py on `folder` against the app; returns the lines it printed.

    check=True is --check: it says what the manifest would hold and writes nothing, so a folder that would stop the game
    at start is found before the player's folder is emptied."""
    command = [str(py), str(tree/'scripts/make_mod_manifest.py'), str(folder), '--app', str(app), '--version', version]
    if check:
        command.append('--check')
    result = subprocess.run(command, cwd=tree, capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    lines = [line for line in output.splitlines() if line.strip()]
    if result.returncode:
        raise InstallError(lines[-1] if lines else f'make_mod_manifest.py failed ({result.returncode})', output)
    return lines


def _would_hold(lines):
    """How many files the manifest would hold, read from the output of make_mod_manifest.py --check (0 when it says the
    folder replaces or adds no file)."""
    for line in lines:
        match = re.search(r'would hold (\d+) files', line)
        if match:
            return int(match.group(1))
        if 'replaces or adds no file' in line:
            return 0
    return 0


def install(mod, version, app, py, tree, *, say=print):
    """Replace the player's mod folder for `version` with `mod`'s romfs/ and write its manifest against `app`.

    Returns Installed, or None when nothing was installed - no romfs/, an empty one, or one whose files the game would
    all load from the app instead - in which case the player's folder is exactly as it was."""
    source = mod/'romfs'
    folder = mods_folder(version)
    if not source.is_dir():
        say(f'{mod.name} has no romfs/, so {folder} is left as it is')
        return None
    if not files_below(source):
        say(f'{mod.name}/romfs holds no files, so {folder} is left as it is')
        return None
    if folder.is_symlink():
        raise InstallError(f'{folder} is a symlink; replace it with a real folder before --install-mod-romfs writes there')
    found = _manifest(py, tree, source, app, version, check=True)     # the mod folder itself: nothing is written there
    if not _would_hold(found):
        say(f'{mod.name}/romfs would replace or add no file the game loads (every file is the original, or one the app '
            f'replaces itself), so {folder} is left as it is and no manifest is written')
        return None

    staged, backup = folder.with_name(f'{folder.name}.new'), backup_name(folder)
    try:
        if folder.exists() and not folder.is_dir():
            raise InstallError(f'{folder} is not a folder')
        folder.parent.mkdir(parents=True, exist_ok=True)
        if staged.is_symlink():
            staged.unlink()
        elif staged.exists():
            shutil.rmtree(staged)          # left over from an interrupted build: a copy, never the player's files
        staged.mkdir()
        copied = copy_tree(source, staged)
        manifest = _manifest(py, tree, staged, app, version, check=False)   # writes manifest.tsv inside the staged copy
        # the tool names the folder it wrote into; that is the player's folder once the swap below is done, so name that
        manifest = [line.replace(str(staged), str(folder)) for line in manifest]
        if not (staged/'manifest.tsv').is_file():
            raise InstallError(f'{staged} holds no manifest.tsv after make_mod_manifest.py ran, so nothing was swapped in',
                               '\n'.join(manifest))
        kept = None
        if folder.exists():
            folder.rename(backup)          # kept: what the player had there (another mod, files copied by hand)
            try:
                staged.rename(folder)
            except OSError:
                backup.rename(folder)      # put the player's folder back exactly as it was
                raise
            if any(backup.iterdir()):
                kept = backup
            else:
                backup.rmdir()             # it held nothing: no backup to keep
        else:
            staged.rename(folder)
        say(f'{folder}: replaced by {mod.name}/romfs and its manifest.tsv (--install-mod-romfs)'
            + (f'; what it held before is kept in {kept}' if kept else ''))
    except InstallError:
        raise
    except OSError as error:
        raise InstallError(f'{error.filename or folder}: {error.strerror or error}; {folder} holds what it held before',
                           '') from None
    return Installed(folder, copied, kept, manifest)
