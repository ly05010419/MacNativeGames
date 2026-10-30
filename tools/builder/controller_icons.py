"""The Controls tab's icons jobs (E6 S6.3): what the tab needs to offer "Update the game's button icons", and making
the icons mod (the venv first when the player agreed). Run on load_jobs by controller.controls; the made mod goes to
the Mods part, the only writer of library.json (controller_mods.install_icons).

E9 S9.1 C5: only the games in ICONS are offered; for the others the tab says so, with no button."""
from dataclasses import dataclass, field
from pathlib import Path

from tools.builder import icons_env, icons_mod, icons_plan
from tools.builder import controller_controls      # read_state, at call time (it imports this module)
from tools.builder.controller_mods import LATEST, _library

ICONS = ('totk',)                   # the games whose button icons the builder can make (make_button_icons.py)


@dataclass(frozen=True)
class IconsInfo:
    """E6 S6.3: what the Controls tab needs to offer 'Update the game's button icons'."""
    offered: bool
    reason: str = ''                # why not: not_offered, no_app, ask, nintendo, broken_file
    env: str = 'missing'            # icons_env.status: ready, missing, broken
    status: str = 'none'            # icons_mod.status: none, current, outdated
    font_mods: tuple = ()           # (id, name) of other enabled mods that replace the font archives
    shared: tuple = ()              # inputs two buttons drive (their prompt names the first)


@dataclass(frozen=True)
class IconsBuilt:
    ok: bool
    reason: str = ''
    detail: str = ''
    work: str = ''
    made_from: dict = field(default_factory=dict)


def icons_info(game_dir, store, repo, spec, app, data_dir, builder_dir):
    if spec.id not in ICONS:
        return IconsInfo(False, 'not_offered')
    state = controller_controls.read_state(game_dir, store, spec.id)
    why = 'not_offered' if state.reason else 'no_app' if app is None or not Path(app).is_dir() else \
        'broken_file' if state.error else 'ask' if state.by_label is None else 'nintendo' if state.by_label else ''
    if why:
        return IconsInfo(False, why)
    library = _library(repo, spec, LATEST[spec.id], data_dir)
    now = icons_mod.current(repo, app, LATEST[spec.id], library, game_dir, store, spec.id)
    names = library.state['names']
    return IconsInfo(True, '', icons_env.status(builder_dir), icons_mod.status(library, now),
                     tuple((m, names.get(m, m)) for m in icons_mod.font_mods(library)), icons_plan.shared(state.inputs))


def make_icons(game_dir, store, repo, spec, app, data_dir, builder_dir, install_env, cancel):
    """The icons venv (made first when the player agreed), then the mod made in the library's work folder."""
    if spec.id not in ICONS:
        return IconsBuilt(False, 'not_offered')
    try:
        if install_env:
            icons_env.install(builder_dir, cancel)
        if icons_env.status(builder_dir, cancel) != 'ready':
            return IconsBuilt(False, 'env', 'fontTools is not ready')
        state = controller_controls.read_state(game_dir, store, spec.id)
        if state.by_label is None or state.by_label or state.error:
            return IconsBuilt(False, 'changed')
        library = _library(repo, spec, LATEST[spec.id], data_dir)
        work, table = icons_mod.build(repo, app, LATEST[spec.id], library, state.inputs, builder_dir, cancel)
    except (icons_env.EnvError, icons_mod.BuildError) as e:
        return IconsBuilt(False, e.reason, e.detail)
    return IconsBuilt(True, work=str(work), made_from=icons_mod.record(state.raw, state.type_raw, state.answer, table))
