"""build_app.py imported once, as the one source of what the games are and how their files are read (title IDs, the
Switch 2 dump layout, the shared fonts check). Importing it reads its expected-value files and runs nothing else."""
import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD_APP = REPO / 'build_app.py'
UPDATE_RUNTIME = REPO / 'update_runtime.py'

_module = None


def build_app():
    global _module
    if _module is None:
        spec = importlib.util.spec_from_file_location('mng_build_app', BUILD_APP)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _module = module
    return _module


# The app each game builds into: the names build_app.py gives them (APPS; Ocarina of Time in games/oot/build.py).
GAMES = {
    'totk': 'Tears of the Kingdom',
    'botw': 'Breath of the Wild',
    'oot': 'Ocarina of Time',
    'dkb': 'Donkey Kong Bananza',
    'pkp': 'Pokémon Pokopia',
}
OOT_TITLE = '0400486024DEC000'   # build_app.main_ns2


def game_of_title(title):
    """'totk', 'botw', 'oot', 'dkb', 'pkp' or None for a title ID (16 hex digits) of a base, update or add-on."""
    b = build_app()
    title = title.upper()
    if title == OOT_TITLE:
        return 'oot'
    if title in b.NS2:
        return b.NS2[title]['version']
    application, _ = b.title_kind(title)
    return b.TITLES.get(application)
