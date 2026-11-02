"""The games the builder can build: data only, mirroring build_app.py (REPO/GAME/BOTW, VERSIONS, TITLES, APPS).

build_app.py loads its versions explicitly, so they are declared here too and never discovered from the assets
folder. tests/test_builder_games.py compares these associations with build_app.py's source."""
from dataclasses import dataclass


@dataclass(frozen=True)
class VersionSpec:
    key: str        # build_app.py's VERSIONS key, also the player's mods folder name (v0, v112, ...)
    display: str    # the manifest's display_version
    manifest: str   # file name in the game's assets folder


@dataclass(frozen=True)
class GameSpec:
    id: str
    display_name: str
    title_id: str
    app_name: str
    bundle_id: str
    data_folder: str        # ~/Library/Application Support/MacNativeGames/<data_folder>
    work_folder_name: str   # default work folder, next to the game file
    assets_dir: str         # relative to the repository
    versions: tuple[VersionSpec, ...]


TOTK = GameSpec(
    id='totk',
    display_name='Tears of the Kingdom',
    title_id='0100F2C0115B6000',
    app_name='Tears of the Kingdom',
    bundle_id='org.macnativegames.totk',
    data_folder='TOTK',
    work_folder_name='TOTK_build',
    assets_dir='games/totk/assets',
    versions=(
        VersionSpec('v0', '1.0.0', 'expected-v1.0.0.json'),
        VersionSpec('v112', '1.1.2', 'expected-v1.1.2.json'),
        VersionSpec('v121', '1.2.1', 'expected-v1.2.1.json'),
        VersionSpec('v143', '1.4.3', 'expected-v1.4.3.json'),
    ),
)

BOTW = GameSpec(
    id='botw',
    display_name='Breath of the Wild',
    title_id='01007EF00011E000',
    app_name='Breath of the Wild',
    bundle_id='org.macnativegames.botw',
    data_folder='BotW',
    work_folder_name='BotW_build',
    assets_dir='games/botw/assets',
    versions=(
        VersionSpec('botw160', '1.6.0', 'expected-v1.6.0.json'),
        VersionSpec('botw190', '1.9.0', 'expected-v1.9.0.json'),   # base + the 1.9.0 update; --dlc not in the builder yet
    ),
)

GAMES = {spec.id: spec for spec in (TOTK, BOTW)}
