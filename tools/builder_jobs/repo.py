"""build_app.py imported once, as the one source of what the games are and how their files are read (title IDs, the
Switch 2 dump layout, the shared fonts check). Importing it reads its expected-value files and runs nothing else."""
import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD_APP = REPO / 'build_app.py'
UPDATE_RUNTIME = REPO / 'update_runtime.py'
SHADER_UPDATE = REPO / 'shader_update.py'

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
    'dread': 'Metroid Dread',
    'tmnt': "TMNT Shredder's Revenge",
    'acnh': 'Animal Crossing New Horizons',   # games/acnh/build.py APP_NAME
    'eow': 'The Legend of Zelda Echoes of Wisdom',   # games/eow/build.py APP_NAME
    'odyssey': 'Super Mario Odyssey',   # games/odyssey/build.py APP_NAME
}
OOT_TITLE = '0400486024DEC000'   # build_app.main_ns2

# Games this repository builds from the command line (games/<folder>/build.py) that the Launcher does not (user
# 2026-11-22: "去掉 WORLDEND SYNDROME", only from the Launcher; "去掉 塞尔达传说 御天之剑 HD"). Both ran .NET programs,
# which a Launcher, carrying no .NET SDK, cannot build. A file of one is named and left out, not taken for another game.
NOT_IN_LAUNCHER = {'01002DA013484000': ('The Legend of Zelda: Skyward Sword HD', '塞尔达传说 御天之剑 HD', 'games/ss'),
                   '010048900CF64000': ('WORLDEND SYNDROME', 'WORLDEND SYNDROME', 'games/worldend')}


def game_of_title(title):
    """'totk', 'botw', 'oot', 'dkb', 'pkp', 'acnh' (and the other community ports) or None for a title ID (16 hex digits) of a base, update or add-on."""
    b = build_app()
    title = title.upper()
    if title == OOT_TITLE:
        return 'oot'
    if title in b.NS2:
        return b.NS2[title]['version']
    application, _ = b.title_kind(title)
    if application == '0100FE701475A000':
        return 'tmnt'
    if application == '010093801237C000':
        return 'dread'
    if application == '01006F8002326000':
        return 'acnh'
    if application == '01008CF01BAAC000':
        return 'eow'
    if application == '0100000000010000':
        return 'odyssey'
    return b.TITLES.get(application)
