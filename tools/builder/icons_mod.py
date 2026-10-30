"""The button-icons mod made from the player's layout (E6 S6.3, C2, C3, C5, B1, B5). The model: no Tk.

build() makes the mod in <library>/.icons-build: its romfs/ first gets the size table the other enabled mods would
install for this version (the last one in the list wins, as Apply composes; none: the script reads the app's), then
icons_generate.py runs with the icons venv's Python (cancellable, a timeout) and draws the four font archives and grows
only their entries in that table; the script's own manifest.tsv is removed. install() runs in the Mods part's job (the
only writer of library.json): the mod gets the reserved id 'builder-button-icons', replaced in place (the new folder
beside it, the old one renamed aside, the new one renamed in, the old one removed), enabled, kept last in the list
(mod_library.save), revalidated, with the record of what it was made from; mods the player agreed to switch off (other
font mods) are switched off. record() hashes controls.txt, controller-type.txt, the answer kept for auto and the seeded
table, so the tabs can say when the icons no longer match."""
import hashlib
import json
from pathlib import Path
import os
import shutil

from tools.builder import cancellable, controls_file, icons_env, icons_plan, mod_library, mod_tools

NAME = 'Button icons (your controls)'
GENERATE_S = 300.0
SCRIPT = Path(__file__).with_name('icons_generate.py')


class BuildError(Exception):
    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}')
        self.reason, self.detail = reason, detail


def table_name(repo, app, version):
    manifest_tool, _ = mod_tools.tools(repo)
    index = manifest_tool.read_index(mod_tools.app_index(app, version))
    return next(p for p in index if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.rsizetable.zs'))


def others(library):
    return [m for m in library.state['enabled'] if m != mod_library.ICONS]


def seed_table(library, name):
    """The table the other enabled mods would install (the last one wins), or None."""
    found = None
    for mod_id in others(library):
        path = library.path(mod_id) / 'romfs' / name
        if path.is_file():
            found = path.read_bytes()
    return found


def font_mods(library):
    """Other enabled mods that replace the font archives (an older icons mod such as xbox-icons)."""
    return [m for m in others(library) if any((library.path(m) / 'romfs/Font').glob('*.bfarc.zs'))]


def _sha(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def record(controls_raw, type_raw, answer, table):
    return {'controls': _sha(controls_raw), 'type': _sha(type_raw), 'answer': answer, 'table': _sha(table)}


def current(repo, app, version, library, game_dir, store, game):
    """The record of what the icons mod would be made from now."""
    read = lambda p: Path(p).read_bytes() if Path(p).is_file() else None  # noqa: E731
    return record(read(Path(game_dir) / 'controls.txt'), read(Path(game_dir) / 'controller-type.txt'),
                  controls_file.read_answer(store, game), seed_table(library, table_name(repo, app, version)))


def status(library, current):
    """'none', 'current' or 'outdated' for the icons mod against the record of what it would be made from now."""
    if mod_library.ICONS not in library.state['order']:
        return 'none'
    return 'current' if library.state.get('icons') == current else 'outdated'


def build(repo, app, version, library, inputs, builder_dir, cancel=None):
    """The made mod's folder (<library>/.icons-build, holding romfs/); BuildError with the step that failed."""
    work = library.root / '.icons-build'
    shutil.rmtree(work, ignore_errors=True)
    romfs = work / 'romfs'
    name = table_name(repo, app, version)
    table = seed_table(library, name)
    try:
        romfs.mkdir(parents=True)
        if table is not None:
            (romfs / name).parent.mkdir(parents=True, exist_ok=True)
            (romfs / name).write_bytes(table)
        plan_file = work / 'plan.json'
        plan_file.write_text(json.dumps(icons_plan.plan(inputs)))
        argv = [icons_env.python(icons_env.venv(builder_dir)), '-I', SCRIPT, romfs, app, version, plan_file]
        try:
            code, _, err = cancellable.run(argv, cancel, GENERATE_S)
        except cancellable.Stopped as stop:
            raise BuildError(stop.reason, stop.detail) from None
        if code != 0:
            raise BuildError('generator', cancellable.last_line(err))
        (romfs / 'manifest.tsv').unlink(missing_ok=True)
        plan_file.unlink()
        if len(list((romfs / 'Font').glob('*.bfarc.zs'))) == 0:
            raise BuildError('generator', 'no font archive was written')
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise
    return work, table


def install(repo, app, version, library, work, made_from, switch_off=()):
    """In the Mods part's job: the made folder becomes the icons mod (in place), enabled and last; the record kept."""
    target = library.path(mod_library.ICONS)
    new, aside = target.with_name(target.name + '.new'), target.with_name(target.name + '.old')
    for stale in (new, aside):
        shutil.rmtree(stale, ignore_errors=True)
    os.replace(work, new)
    if target.exists():
        os.replace(target, aside)
    os.replace(new, target)
    shutil.rmtree(aside, ignore_errors=True)
    state = library.state
    if mod_library.ICONS not in state['order']:
        state['order'].append(mod_library.ICONS)
    state['names'][mod_library.ICONS] = NAME
    state['notes'][mod_library.ICONS] = []
    mod_library.revalidate(repo, app, version, library, [mod_library.ICONS])
    valid = mod_library.validation(library, mod_library.ICONS)
    if valid.valid:                                 # an invalid one is kept off, and nothing else is switched off
        state['enabled'] = [m for m in state['order'] if m not in switch_off and
                            (m in state['enabled'] or m == mod_library.ICONS)]
        state['icons'] = made_from
    mod_library.save(library)
    return valid
