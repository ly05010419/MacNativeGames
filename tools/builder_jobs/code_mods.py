"""The code patches of the player's mods in a full build, as the old window put them in (tools/builder/controller_mods
_build_library / _compose, S5.4): only Tears of the Kingdom with an update file, the mods whose code is switched on on the
Mods page composed into one --mod folder, and their record written only when the build succeeded (an empty set is
recorded too, so a build after the code was switched off clears it). The rules stay in tools/builder/mod_code.py."""
from functools import partial
from pathlib import Path

from tools.builder_jobs import repo


def compose(plan, home):
    """(['--mod', folder] or [], the writer to call with the app after a successful build, or None)."""
    if plan.get('game') != 'totk' or not plan.get('update'):
        return [], None
    from tools.builder import controller_mods, mod_code, mod_tools
    from tools.builder.games import GAMES
    spec, version = GAMES['totk'], controller_mods.LATEST['totk']
    if not mod_tools.supported(repo.REPO, spec, version):
        return [], None
    library = controller_mods._library(repo.REPO, spec, version, Path(home) / 'Library/Application Support/MacNativeGames')
    folder, record = mod_code.compose(library, mod_code.planned(library, 'totk', True))
    return (['--mod', str(folder)] if folder else []), partial(mod_code.write_record, library.root, record)
