"""The controller's part for the Mods tab (S5.2): read the library, add, remove, order, enable, Apply, and remove the
player's own backups when asked. It runs the model on the daemon load_jobs and emits events through the
BuildController that owns it; the view calls it as controller.mods.

One mods job at a time. Apply needs the controller idle and no settings job (a Play) running, and while it runs the
controller is not idle (O3): no build and no Play start beside it. The game and its app are the Build tab's
(controller.start's context); only the latest version of each game, and only when main's tools support it
(mod_tools.supported). S5.4: a picked folder or .zip is searched for the mod (a refusal comes back in the state, with
its reason); code mods are toggled for the next build, which composes them at its start (code_for_build) and records
them on success; planned_command shows the same --mod path. S5.5: an Add of a mod holds the controller (not idle) and
the controller's cancel stops an archive's extraction; a full read checks again the mods validated by older rules
(mod_tools.RULES), names the enabled ones that became not valid and so were switched off, and removes a .unzip a crash
left."""
from dataclasses import dataclass, replace
from functools import partial
from pathlib import Path
import shutil
import threading

from tools.builder import game_state, icons_mod, mod_add, mod_applied, mod_code, mod_find, mod_library, mod_tools
from tools.builder.games import GAMES
from tools.builder.build_run import collect
from tools.builder.session import IconsDone, ModsApplied, ModsStateReady, ProcessError

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


def _build_library(repo, data_dir, snapshot):
    """(the library, the mods whose code a build of this snapshot includes), or (None, ()) when it has no library.
    S5.4 R1: code only for TOTK with an update; O2: such a build records its set even when empty, so a build after
    the code was switched off clears the record."""
    spec, fields = GAMES[snapshot[0]], dict(snapshot[1])
    if spec.id != 'totk' or not mod_tools.supported(repo, spec, LATEST[spec.id]):
        return None, ()
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    return library, mod_code.planned(library, spec.id, bool(fields.get('update')))


def _compose(library, mod_ids):
    """At the build's start, on the worker: (the --mod folder, the record, its writer for a success, which BuildRun
    calls with the app the build made). mod_ids are the toggles as they were at Build (O3)."""
    folder, record = mod_code.compose(library, mod_ids)
    return folder, record, partial(mod_code.write_record, library.root, record)


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
    """E6 S6.3 B5: the made icons mod into the library (this part's job is the only writer), then Apply."""
    if game_state.game_running(Path(data_dir) / spec.data_folder):
        shutil.rmtree(work, ignore_errors=True)
        return IconsResult(False, 'running')
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    v = icons_mod.install(repo, app, LATEST[spec.id], library, Path(work), made_from, switch_off)
    if not v.valid:
        return IconsResult(False, 'invalid', v.reason)
    r = mod_applied.apply_and_record(repo, spec, app, LATEST[spec.id], library, data_dir)
    return IconsResult(r.ok, r.reason, '', r)


class ModsController:
    def __init__(self, owner):
        self.owner = owner
        self._job = self._kind = None
        self._cancel = threading.Event()

    @property
    def busy(self):
        return self._job is not None

    @property
    def applying(self):
        return self._kind == 'apply'

    @property
    def holds(self):
        """Apply or an Add runs: the controller is not idle, so closing asks and waits (S5.5 B2; a folder's copy too,
        or a cp across volumes would outlive the window)."""
        return self._kind in ('apply', 'add', 'icons')

    def cancel(self):
        """Stops an archive's extraction (its tar group is killed); the copy and the validation after it run on."""
        if self._kind == 'add':
            self._cancel.set()

    def _submit(self, kind, fn, *args):
        self.owner._open()
        if self._job is not None:
            raise RuntimeError('a mods change is still running')
        spec, app = self.owner.start._context()
        self._job = self.owner.load_jobs.submit(fn, self.owner.repo_root, spec, app, self.owner.start.paths.data, *args)
        self._kind = kind

    def state(self, full=True):
        self._submit('state', read_state, full)

    def add(self, folder):
        cancel = threading.Event()          # kept only once submitted: a refused call leaves the running one's
        self._submit('add', change, 'add', self.owner.runner, Path(folder), cancel)
        self._cancel = cancel

    def remove(self, mod_id):
        self._submit('change', change, 'remove', mod_id)

    def enable(self, mod_id, on):
        self._submit('change', change, 'enable', mod_id, on)

    def move(self, mod_id, step):
        self._submit('change', change, 'move', mod_id, step)

    def remove_backups(self, names):
        self._submit('change', change, 'remove_backups', tuple(names))

    def code(self, mod_id, on):
        self._submit('change', change, 'code', mod_id, on)

    def code_for_build(self, snapshot):
        """S5.4: None, or a callable prepare_request runs at the build's start to compose the included code mods
        (possibly none: its record is then empty)."""
        library, mod_ids = _build_library(self.owner.repo_root, self.owner.start.paths.data, snapshot)
        return partial(_compose, library, mod_ids) if library is not None else None

    def code_folder(self, snapshot):
        """The --mod path a build of snapshot would get (for --dry-run), or None; nothing is written."""
        library, mod_ids = _build_library(self.owner.repo_root, self.owner.start.paths.data, snapshot)
        return mod_code.folder(library) if mod_ids else None

    def apply(self):
        if not self.owner.idle or self.owner.start.busy:
            raise RuntimeError('a build, an update or Play is running')
        self._submit('apply', apply)

    def install_icons(self, work, made_from, switch_off=()):
        """Called by the Controls part once its icons are made; holds the controller until Apply ends."""
        self._submit('icons', install_icons, Path(work), made_from, tuple(switch_off))

    def tick(self):
        if self._job is None or not self._job.done():
            return
        job, kind, self._job, self._kind = self._job, self._kind, None, None
        value, error = collect(job)
        if error is not None:
            self.owner._emit(ProcessError('mods', repr(error)))
        else:
            self.owner._emit(ModsApplied(value) if kind == 'apply' else IconsDone(value) if kind == 'icons'
                             else ModsStateReady(value))
