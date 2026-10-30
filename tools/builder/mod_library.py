"""The player's mod library and Apply (S5.2, M2-M6). The model: no Tk.

The player adds every mod (mod_add, since S5.5); the builder ships none. Each added mod is cloned into
<data>/mod-library/<version>/<id>/ (APFS clones: no extra space on the same volume) and validated (mod_tools.validate);
only a valid mod can be enabled. Apply composes the enabled ones in order (the later wins on a shared path) and hands
them to main's own mod_folder.install, which checks, writes the manifest, swaps the load folder atomically and keeps the
old one as a backup. Nothing enabled, or a composition that loads nothing, sets the load folder aside (kept). A folder
the builder installed carries `.made-by-builder` with the path, size and mtime of every file it holds (manifest.tsv
included, the marker itself excluded); an older backup is removed only when it is a real folder named <version>.backup-*
whose files still match its marker exactly, the newest builder backup kept. Anything else is the player's and is never
removed by the builder. Apply refuses while the game runs or the free space is short. A load folder found without a
marker is first imported as one mod, "Installed before the builder"."""
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from tools.builder import game_state, mod_shared, mod_tools

MARKER, STATE, IMPORTED = '.made-by-builder', 'library.json', 'installed-before-the-builder'
ICONS = 'builder-button-icons'      # E6 S6.3: the builder's icons mod, reserved, always last (its size table wins)
TITLE_IDS = {'v143': '0100F2C0115B6000', 'botw190': '01007EF00011E000'}
MARGIN = 1 << 30                    # free space kept beyond the composed size
COPY_S = 1800.0


@dataclass(frozen=True)
class Mod:
    id: str
    name: str
    enabled: bool
    validation: mod_tools.Validation
    conflicts: tuple = ()           # paths another enabled mod also replaces
    code: bool = False              # S5.4: its code is included in the next build
    notes: tuple = ()               # S5.4: e.g. 'cheats_ignored'


@dataclass(frozen=True)
class ApplyResult:
    ok: bool
    reason: str = ''                # '' or: running, no_space, nothing, set_aside, install_failed
    lines: tuple = ()
    removed_backups: int = 0


@dataclass(frozen=True)
class Library:
    root: Path                      # <data>/mod-library/<version>
    load: Path                      # main's mods_folder(version)
    state: dict = field(default_factory=dict)

    def path(self, mod_id):
        return self.root / mod_id


def open_library(data_dir, version, load):
    root = Path(data_dir) / 'mod-library' / version
    try:
        state = json.loads((root / STATE).read_text())
    except (OSError, ValueError):
        state = {}
    state.pop('format', None)                       # save writes it
    state = {**state,                               # keys another builder wrote are kept as they are
             'order': list(state.get('order', [])), 'enabled': list(state.get('enabled', [])),
             'names': dict(state.get('names', {})), 'validation': dict(state.get('validation', {})),
             'code': list(state.get('code', [])), 'notes': dict(state.get('notes', {})), 'icons': state.get('icons'),
             'applied': state.get('applied'), 'applied_failed': bool(state.get('applied_failed', False)),
             'same': dict(state.get('same', {}))}
    return Library(root, Path(load), state)


def save(library):
    for key in ('order', 'enabled'):                # E6 S6.3 B1: the icons mod last, whatever moved
        if ICONS in library.state[key]:
            library.state[key] = [m for m in library.state[key] if m != ICONS] + [ICONS]
    library.root.mkdir(parents=True, exist_ok=True)
    partial = library.root / (STATE + '.partial')
    partial.write_text(json.dumps({'format': 1, **library.state}, indent=2) + '\n')
    os.replace(partial, library.root / STATE)


def _files(folder):
    return sorted(p for p in Path(folder).rglob('*') if p.is_file())


def _size(folder):
    return sum(p.stat().st_size for p in _files(folder))


def clone_file(source, dest):
    """One file as an APFS clone (cp -c, no shell), or a plain copy where cloning is not possible."""
    if subprocess.run(['/bin/cp', '-c', str(source), str(dest)], capture_output=True).returncode != 0:
        shutil.copy2(source, dest)
    return dest


def clone(runner, source, dest):
    """source's content into dest: APFS clones (cp -c), a plain copy on another volume after a free-space check."""
    Path(dest).mkdir(parents=True, exist_ok=True)
    if os.stat(source).st_dev != os.stat(dest).st_dev and shutil.disk_usage(dest).free < _size(source) + MARGIN:
        raise OSError(f'not enough free space to copy {source}')
    result = runner.run(['/bin/cp', '-cR', f'{source}/.', str(dest)], timeout_s=COPY_S)
    if result.outcome.name != 'OK':
        raise OSError(f'copying {source} failed: {(result.stderr or result.outcome.name).strip()[-200:]}')


def _new_id(library, name):
    base = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')[:40] or 'mod'
    mod_id, n = base, 2
    while library.path(mod_id).exists() or mod_id in library.state['order'] or mod_id == ICONS:
        mod_id, n = f'{base}-{n}', n + 1
    return mod_id


def revalidate(repo, app, version, library, ids=None):
    for mod_id in ids or list(library.state['order']):
        v = mod_tools.validate(repo, app, version, library.path(mod_id))
        library.state['validation'][mod_id] = v.__dict__ | {'taken': list(v.taken), 'misspelt': list(v.misspelt),
                                                            'versions': list(v.versions)}
        if not v.valid and mod_id in library.state['enabled']:
            library.state['enabled'].remove(mod_id)


def validation(library, mod_id):
    data = dict(library.state['validation'].get(mod_id) or {'valid': False, 'reason': 'unchecked'})
    for key in ('taken', 'misspelt', 'versions'):
        data[key] = tuple(data.get(key, ()))
    return mod_tools.Validation(**data)


def _shared(library):
    return mod_shared.owners(library, library.state['enabled'])


def record_same(library):
    """The full read notes which shared files are the same bytes (mod_shared); saved when that changes."""
    if mod_shared.record_same(library, _shared(library)):
        save(library)


def mods(library):
    """The player's mods. A path whose files are the same bytes in every mod that replaces it is no conflict
    (mod_shared)."""
    shared = mod_shared.conflicts(library, _shared(library))
    return tuple(Mod(m, library.state['names'].get(m, m), m in library.state['enabled'], validation(library, m),
                     tuple(sorted(p for p, ids in shared.items() if m in ids)), m in library.state['code'],
                     tuple(library.state['notes'].get(m, ()))) for m in library.state['order'])


def set_enabled(library, mod_id, on):
    if on and not validation(library, mod_id).valid:
        raise ValueError(f'{mod_id} is not valid for this app')
    enabled = [m for m in library.state['enabled'] if m != mod_id]
    library.state['enabled'] = [m for m in library.state['order'] if m in enabled or (on and m == mod_id)]
    save(library)


def move(library, mod_id, step):
    order = library.state['order']
    i = order.index(mod_id)
    j = max(0, min(len(order) - 1, i + step))
    order.insert(j, order.pop(i))
    library.state['enabled'] = [m for m in order if m in library.state['enabled']]
    save(library)


def remove(library, mod_id):
    """Deletes the builder's copy only; the folder the player chose stays."""
    shutil.rmtree(library.path(mod_id), ignore_errors=True)
    for key in ('order', 'enabled', 'code'):
        library.state[key] = [m for m in library.state[key] if m != mod_id]
    library.state['notes'].pop(mod_id, None)
    library.state['names'].pop(mod_id, None)
    library.state['validation'].pop(mod_id, None)
    save(library)


def import_existing(repo, app, version, library):
    """M5: a load folder without the marker is the player's; it becomes one enabled mod (its files cloned)."""
    load = library.load
    if not load.is_dir() or (load / MARKER).exists() or IMPORTED in library.state['order']:
        return False
    files = [p for p in _files(load) if p.name != 'manifest.tsv' and not any(s.startswith('.') for s in
                                                                             p.relative_to(load).parts)]
    if not files:
        return False
    dest = library.path(IMPORTED) / 'romfs'
    for file in files:
        target = dest / file.relative_to(load)
        target.parent.mkdir(parents=True, exist_ok=True)
        clone_file(file, target)
    library.state['order'].insert(0, IMPORTED)
    library.state['names'][IMPORTED] = 'Installed before the builder'
    revalidate(repo, app, version, library, [IMPORTED])
    if validation(library, IMPORTED).valid:
        library.state['enabled'].insert(0, IMPORTED)
    save(library)
    return True


def _listing(folder):
    return sorted([p.relative_to(folder).as_posix(), p.stat().st_size, p.stat().st_mtime_ns]
                  for p in _files(folder) if p.name != MARKER)


def _builder_made(folder):
    """A real folder whose files still match its marker exactly (path, size, mtime)."""
    marker = Path(folder) / MARKER
    if Path(folder).is_symlink() or not marker.is_file():
        return False
    try:
        return json.loads(marker.read_text()) == _listing(folder)
    except (OSError, ValueError):
        return False


def backups(library):
    """(builder-made, the player's) backups next to the load folder, oldest first."""
    found = sorted(p for p in library.load.parent.glob(f'{library.load.name}.backup-*') if p.is_dir())
    return [p for p in found if _builder_made(p)], [p for p in found if not _builder_made(p)]


def remove_backups(library, names):
    """M6: the player's own backups, removed only when the player asked for these (names), never a symlink."""
    _, theirs = backups(library)
    removed = 0
    for folder in theirs:
        if folder.name in names and not folder.is_symlink():
            shutil.rmtree(folder)
            removed += 1
    return removed


def _set_aside(library, folder_tool):
    if library.load.exists():
        library.load.rename(folder_tool.backup_name(library.load))


def apply(repo, spec, app, version, library, data_dir):
    _, folder_tool = mod_tools.tools(repo)
    if game_state.game_running(Path(data_dir) / spec.data_folder):
        return ApplyResult(False, 'running')
    enabled = [m for m in library.state['enabled'] if validation(library, m).valid]
    if not enabled:
        _set_aside(library, folder_tool)
        return ApplyResult(True, 'nothing')
    staging = library.root / '.compose'
    shutil.rmtree(staging, ignore_errors=True)
    for mod_id in enabled:                           # in order: the later mod's file wins on a shared path
        shutil.copytree(library.path(mod_id) / 'romfs', staging / 'romfs', dirs_exist_ok=True,
                        copy_function=clone_file)
    try:
        library.load.parent.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(library.load.parent).free < _size(staging) + MARGIN:
            return ApplyResult(False, 'no_space')
        if game_state.game_running(Path(data_dir) / spec.data_folder):
            return ApplyResult(False, 'running')
        lines = []
        try:
            done = folder_tool.install(staging, version, Path(app), sys.executable, Path(repo) / 'games/totk',
                                       say=lines.append)
        except folder_tool.InstallError as e:
            return ApplyResult(False, 'install_failed', tuple(lines) + (str(e), getattr(e, 'output', '') or ''))
        if done is None:                             # O1: nothing would load: no stale mods stay in place
            _set_aside(library, folder_tool)
            return ApplyResult(True, 'set_aside', tuple(lines))
        (library.load / MARKER).write_text(json.dumps(_listing(library.load)) + '\n')
        builder_made, _ = backups(library)
        for old in builder_made[:-1]:                # the newest builder backup stays
            shutil.rmtree(old)
        return ApplyResult(True, '', tuple(lines), max(0, len(builder_made) - 1))
    finally:
        shutil.rmtree(staging, ignore_errors=True)
