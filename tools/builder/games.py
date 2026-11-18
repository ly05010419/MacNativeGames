"""The games the builder can build: data only, mirroring build_app.py (REPO/GAME/BOTW, VERSIONS, TITLES, APPS).

build_app.py loads its versions explicitly, so they are declared here too and never discovered from the assets
folder. tests/test_builder_games.py compares these associations with build_app.py's source."""
from dataclasses import dataclass


@dataclass(frozen=True)
class VersionSpec:
    key: str        # build_app.py's VERSIONS key, also the player's mods folder name (v0, v121, ...)
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
    assets_dir: str         # relative to the repository
    versions: tuple[VersionSpec, ...]
    slot_folder: str = ''   # the data folder whose game-slot the game takes, when not its own (game_state.slot_dir)


TOTK = GameSpec(
    id='totk',
    display_name='Tears of the Kingdom',
    title_id='0100F2C0115B6000',
    app_name='Tears of the Kingdom',
    bundle_id='org.macnativegames.totk',
    data_folder='TOTK',
    assets_dir='games/totk/assets',
    versions=(
        VersionSpec('v0', '1.0.0', 'expected-v1.0.0.json'),
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
    assets_dir='games/botw/assets',
    versions=(
        VersionSpec('botw160', '1.6.0', 'expected-v1.6.0.json'),
        VersionSpec('botw190', '1.9.0', 'expected-v1.9.0.json'),   # base + the 1.9.0 update; --dlc not in the builder yet
    ),
)

GAMES = {spec.id: spec for spec in (TOTK, BOTW)}

# Ocarina of Time (Switch 2): built by games/oot/build.py, not build_app.py, so not in GAMES (the catalog the build pages
# and tests/builder_buildapp_source.py compare with build_app.py). MNG Launcher's game pages serve it (PAGES). Its launcher
# takes the game slot in TOTK/ (games/oot/app/launcher.m: TOTK_GAME_SLOT_DIR's default), its other files are in its own
# folder as TOTK's are: profile/, shader-cache/, controls.txt (same app/launcher.m).
OOT = GameSpec(
    id='oot',
    display_name='Ocarina of Time',
    title_id='0400486024DEC000',
    app_name='Ocarina of Time',
    bundle_id='org.macnativegames.oot',
    data_folder='OcarinaOfTime',
    assets_dir='games/oot/assets',
    versions=(VersionSpec('v131072', 'v131072', 'expected-v131072.json'),),
    slot_folder='TOTK',
)

# Animal Crossing: New Horizons (community port): built by games/acnh/build.py. Its launcher keeps one data folder per game
# version (games/acnh/scripts/acnh_app_launcher.c ACNH_DATA_FOLDER); the Launcher's pages serve 3.0.3, the version it
# builds by default. Its settings live in the game's own panel (key 1), so it has no Settings or Mods page here.
ACNH = GameSpec(
    id='acnh',
    display_name='Animal Crossing: New Horizons',
    title_id='01006F8002326000',
    app_name='Animal Crossing New Horizons',
    bundle_id='org.macnativegames.acnh',
    data_folder='ACNH-3.0.3',
    assets_dir='games/acnh/assets',
    versions=(VersionSpec('v2228224', '3.0.3', 'expected-v2228224.json'),),
)

# The Legend of Zelda: Echoes of Wisdom (community port): built by games/eow/build.py; its launcher (games/eow/app/launcher.c)
# keeps its data in EoW/, controls.txt there. Controls (2026-11-17: its runtime has the settings panel's key bindings and its own
# runtime/keyboard_map.h, which the page's key test reads), Storage and Saves pages.
EOW = GameSpec(
    id='eow',
    display_name='The Legend of Zelda: Echoes of Wisdom',
    title_id='01008CF01BAAC000',
    app_name='The Legend of Zelda Echoes of Wisdom',
    bundle_id='org.macnativegames.eow',
    data_folder='EoW',
    assets_dir='games/eow/assets',
    versions=(VersionSpec('v196608', '1.1.0', 'expected-v1.1.0.json'),),
)

# Super Mario Odyssey (community port): built by games/odyssey/build.py (scripts/build_odyssey.py); its launcher
# (scripts/odyssey_app_launcher.c) keeps its data in Odyssey/, controls.txt there.
ODYSSEY = GameSpec(
    id='odyssey',
    display_name='Super Mario Odyssey',
    title_id='0100000000010000',
    app_name='Super Mario Odyssey',
    bundle_id='org.macnativegames.odyssey',
    data_folder='Odyssey',
    assets_dir='games/odyssey/assets',
    versions=(VersionSpec('v262144', '1.3.0', 'expected-local-update.json'),),
)

# The games MNG Launcher's game pages serve, and which pages each has (tools/builder_jobs/tabs.py TABS; the window shows
# these only: tools/builder_app Model.swift gamePages). Ocarina of Time (user 2026-11-13): Controls, Storage and Saves;
# its Settings and Mods are not written for it yet.
PAGE_GAMES = {**GAMES, OOT.id: OOT, ACNH.id: ACNH, EOW.id: EOW, ODYSSEY.id: ODYSSEY}
# 诊断模式 (2026-11-13) was removed 2026-11-15 (user: "去掉诊断模式 快速启动 默认带诊断"): a quick App starts with its
# diagnostics itself (games/oot/app/launcher.m).
PAGES = {'totk': ('settings', 'mods', 'controls', 'storage', 'saves'),
         'botw': ('settings', 'mods', 'controls', 'storage', 'saves'),
         'oot': ('controls', 'storage', 'saves'),
         'acnh': ('controls', 'storage', 'saves'),
         'eow': ('controls', 'storage', 'saves'),
         'odyssey': ('controls', 'storage', 'saves')}
