"""The controller's part for the Textures tab (S9.2): add, switch and remove the texture pack, its heap table for a
build, and Play's 8 GB mode. The pack lives in the Mods tab's library, so every change runs as a Mods job
(controller.mods: one at a time, an Add holds the controller and its cancel stops an archive's extraction) and answers
with the same ModsStateReady, whose state carries the tab's part (ModsState.textures). Apply is the Mods tab's. The view
calls it as controller.textures."""
from dataclasses import replace
from pathlib import Path

from tools.builder import controller_mods, game_state, mod_find, textures
from tools.builder.checks import BuildPaths
from tools.builder.games import GAMES
from tools.builder.paths import effective_paths


def change(repo, spec, app, data_dir, action, *args):
    """One change of the pack, then the Mods state (quick read)."""
    version = controller_mods.LATEST[spec.id]
    if spec.id not in textures.GAMES or app is None or game_state.app_version(app) != version:
        raise ValueError(f'texture packs are for {", ".join(textures.GAMES)} {version} with its app only')
    library = controller_mods._library(repo, spec, version, data_dir)
    if action == 'add':
        runner, source, cancel = args
        try:
            textures.add(runner, repo, app, version, library, source, cancel)
        except mod_find.Refused as refused:
            return replace(controller_mods.read_state(repo, spec, app, data_dir, full=False),
                           refused=(refused.reason, refused.detail))
        return replace(controller_mods.read_state(repo, spec, app, data_dir, full=False), added=(Path(source).stem, 1))
    elif action == 'on':
        textures.set_on(library, *args)
    elif action == 'remove':
        textures.remove(library)
    else:
        raise ValueError(f'unknown textures action {action!r}')
    return controller_mods.read_state(repo, spec, app, data_dir, full=False)


def heap_for_build(repo, library, snapshot, cancel=None):
    """C4: None, or the callable the build's start runs for the pack's fitted heap table (mod_code.compose's heap):
    Tears of the Kingdom with an update file and the pack switched on. cancel (S19.1): the build's, which stops the
    fit."""
    spec, fields = GAMES[snapshot[0]], dict(snapshot[1])
    if spec.id not in textures.GAMES or not fields.get('update') or textures.wanted_heap(library) is None:
        return None
    app = effective_paths(spec, BuildPaths(**{f: Path(v) if v else None for f, v in snapshot[1]})).out
    return lambda: textures.build_heap(repo, library, app, controller_mods.LATEST[spec.id], cancel)


def play_env(repo, spec, app, data_dir):
    """C6, on the Play job: TOTK_DRAM_8GB when the app was built with a pack's heaps; {} otherwise."""
    if spec.id not in textures.GAMES or app is None:
        return {}
    return textures.play_env(controller_mods._library(repo, spec, controller_mods.LATEST[spec.id], data_dir), app)


class TexturesController:
    def __init__(self, owner):
        self.owner = owner

    @property
    def busy(self):
        return self.owner.mods.busy

    def add(self, source):
        self.owner.mods._submit_cancellable('add', change, 'add', self.owner.runner, Path(source))

    def switch(self, on):
        self.owner.mods._submit('change', change, 'on', bool(on))

    def remove(self):
        self.owner.mods._submit('change', change, 'remove')
