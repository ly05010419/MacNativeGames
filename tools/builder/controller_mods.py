"""The Mods page's model calls (S5.2): read the library, add, remove, order, enable, Apply, install the made icons
mod, and remove the player's own backups when asked. MNG Launcher's Mods page (tools/builder_jobs/tab_mods.py) and
Controls page (tab_controls.py) call these, one writer of library.json at a time (the page holds a lock).

Only the latest version of each game, and only when main's tools support it (mod_tools.supported). S5.4: a picked
folder or .zip is searched for the mod (a refusal comes back in the state, with its reason); code mods are toggled for
the next build, which composes them (tools/builder_jobs/code_mods.py over mod_code) and records them on success. S5.5:
an archive's extraction stops when its cancel Event is set; a full read checks again the mods validated by older rules
(mod_tools.RULES), names the enabled ones that became not valid and so were switched off, and removes a .unzip a crash
left."""
from dataclasses import dataclass, replace
from pathlib import Path
import shutil

from tools.builder import game_state, icons_mod, mod_add, mod_applied, mod_code, mod_find, mod_library, mod_tools

LATEST = {'totk': 'v143', 'botw': 'botw190'}


@dataclass(frozen=True)
class ModsState:
    game: str
    name: str
    version_label: str
    app: Path | None
    reason: str = ''                # '' or: no_game_file, no_app, not_latest, not_supported
    mods: tuple = ()
    changed: bool = False           # O4/F4: the load folder's manifest no longer matches this app: Apply again
    player_backups: tuple = ()      # names of backups the builder will never remove by itself
    imported: bool = False          # M5 happened on this reading
    checked: bool = True            # the full reading (import and re-check) ran; a quick one after a change does not
    code_next: tuple = ()           # S5.4: names of the mods whose code the next build includes
    code_pending: bool = False      # S5.4: the app was built with other code than the toggles now: build again
    refused: tuple = ()             # S5.4: (reason, detail) of an Add that found nothing usable
    added: tuple = ()               # S5.5: (what was picked, how many mods it gave) after an Add
    switched_off: tuple = ()        # S5.5 C1b: (name, versions) of enabled mods the new rules made not valid
    outdated: tuple = ()            # E6 S6.3: the icons mod, when it no longer matches the layout or the size tables
    not_applied: bool = False       # S7.3 N2: the game holds other mods than the library (Apply again; mod_applied)


def _label(spec, version):
    return next((v.display for v in spec.versions if v.key == version), '')


def read_state(repo, spec, app, data_dir, full=True):
    """The tab's state; full also imports a hand-copied folder (M5) and re-checks the load folder (F4), which hashes
    it: on opening the tab, after Apply and after a build, not on every toggle."""
    base = dict(game=spec.id, name=spec.display_name, version_label=_label(spec, LATEST[spec.id]), app=app)
    if app is None:
        return ModsState(**base, reason='no_game_file')
    if not mod_tools.supported(repo, spec, LATEST[spec.id]):
        return ModsState(**base, reason='not_supported')
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    if full:
        shutil.rmtree(library.root / mod_add.UNZIP, ignore_errors=True)   # one a crash left (B2)
    names = tuple(library.state['names'].get(m, m) for m in mod_code.included(library))
    if not Path(app).is_dir():                  # S5.4: a build with the update still merges the code mods switched on
        return ModsState(**base, reason='no_app', code_next=names)
    version = game_state.app_version(app)
    if version != LATEST[spec.id]:
        return ModsState(**base, reason='not_latest', code_next=names)
    imported = full and mod_library.import_existing(repo, app, version, library)
    switched_off = _recheck(repo, app, version, library) if full else ()
    changed = full and (library.load / mod_library.MARKER).exists() and mod_tools.recheck(repo, app, version,
                                                                                          library.load)
    if full:
        mod_library.record_same(library)        # identical shared files are no conflict (mod_shared)
    _, theirs = mod_library.backups(library)
    return ModsState(**base, mods=mod_library.mods(library), changed=changed,
                     player_backups=tuple(p.name for p in theirs), imported=imported, checked=full,
                     code_next=names, code_pending=mod_code.pending(library, app), switched_off=switched_off,
                     outdated=_outdated(repo, spec, app, version, library, data_dir),
                     not_applied=mod_applied.not_applied(library))


def _outdated(repo, spec, app, version, library, data_dir):
    if mod_library.ICONS not in library.state['order']:
        return ()
    now = icons_mod.current(repo, app, version, library, Path(data_dir) / spec.data_folder,
                            Path(data_dir) / 'Builder/controls-tab.json', spec.id)
    return (mod_library.ICONS,) if icons_mod.status(library, now) == 'outdated' else ()


def _recheck(repo, app, version, library):
    """C1b: the mods validated by older rules, checked again once; the enabled ones it switched off."""
    stale = [m for m in library.state['order'] if mod_library.validation(library, m).rules != mod_tools.RULES]
    if not stale or not mod_tools.app_index(app, version).is_file():
        return ()                           # no index (an app being replaced): the next full read checks them
    enabled = list(library.state['enabled'])
    mod_library.revalidate(repo, app, version, library, stale)
    mod_library.save(library)
    return tuple((library.state['names'].get(m, m), mod_library.validation(library, m).versions)
                 for m in enabled if m not in library.state['enabled'])


def _library(repo, spec, version, data_dir):
    _, folder_tool = mod_tools.tools(repo)
    return mod_library.open_library(Path(data_dir) / spec.data_folder, version, folder_tool.mods_folder(version))


def change(repo, spec, app, data_dir, action, *args):
    """One library change, then the new state."""
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    if action == 'add':
        runner, folder, cancel = args
        try:
            made = mod_add.add(runner, repo, app, LATEST[spec.id], library, folder, cancel=cancel)
        except mod_find.Refused as refused:
            return replace(read_state(repo, spec, app, data_dir, full=False), refused=(refused.reason, refused.detail))
        picked = Path(folder).stem if Path(folder).is_file() else Path(folder).name
        return replace(read_state(repo, spec, app, data_dir, full=False), added=(picked, len(made)))
    elif action == 'code':
        mod_id, on = args
        mod_code.set_code(library, mod_id, on, mod_library.validation(library, mod_id))
        mod_library.save(library)
    elif action == 'remove':
        mod_library.remove(library, *args)
    elif action == 'enable':
        mod_library.set_enabled(library, *args)
    elif action == 'move':
        mod_library.move(library, *args)
    elif action == 'remove_backups':
        mod_library.remove_backups(library, *args)
    else:
        raise ValueError(f'unknown mods action {action!r}')
    return read_state(repo, spec, app, data_dir, full=False)


def apply(repo, spec, app, data_dir):
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    return mod_applied.apply_and_record(repo, spec, app, LATEST[spec.id], library, data_dir)


@dataclass(frozen=True)
class IconsResult:
    ok: bool
    reason: str = ''                # '' or: running, invalid, a build or venv step (icons_mod, icons_env), apply's
    detail: str = ''
    applied: object = None          # mod_library.ApplyResult


def install_icons(repo, spec, app, data_dir, work, made_from, switch_off):
    """E6 S6.3 B5: the made icons mod into the library (the caller holds the Mods page's lock), then Apply."""
    if game_state.game_running(Path(data_dir) / spec.data_folder):
        shutil.rmtree(work, ignore_errors=True)
        return IconsResult(False, 'running')
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    v = icons_mod.install(repo, app, LATEST[spec.id], library, Path(work), made_from, switch_off)
    if not v.valid:
        return IconsResult(False, 'invalid', v.reason)
    r = mod_applied.apply_and_record(repo, spec, app, LATEST[spec.id], library, data_dir)
    return IconsResult(r.ok, r.reason, '', r)
