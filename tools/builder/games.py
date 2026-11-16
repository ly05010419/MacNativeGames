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

# The games MNG Launcher's game pages serve, and which pages each has (tools/builder_jobs/tabs.py TABS; the window shows
# these only: tools/builder_app Model.swift gamePages). Ocarina of Time (user 2026-11-13): Controls, Storage and Saves;
# its Settings and Mods are not written for it yet.
# Metroid Dread (Switch 1): built by games/dread/build.py, not build_app.py, so not in GAMES, as Ocarina of Time is
# not. MNG Launcher's game pages serve it (PAGES). Its build.py already speaks the launcher's contract (--inspect,
# --json-progress, --keys, --update, --work, --mod) and reads games/dread/game.json for its own metadata, which is
# where these values come from. Its data folder is its own (Dread/), so it takes no other game's slot.
DREAD = GameSpec(
    id='dread',
    display_name='Metroid Dread',
    title_id='010093801237C000',
    app_name='Metroid Dread',
    bundle_id='org.macnativegames.dread',
    data_folder='Dread',
    assets_dir='games/dread/assets',
    versions=(VersionSpec('v100', '1.0.0', 'expected-v1.0.0.json'),
              VersionSpec('v210', '2.1.0', 'expected-v2.1.0.json')),
)

PAGE_GAMES = {**GAMES, OOT.id: OOT, DREAD.id: DREAD}
# 诊断模式 (2026-11-13) was removed 2026-11-15 (user: "去掉诊断模式 快速启动 默认带诊断"): a quick App starts with its
# diagnostics itself (games/oot/app/launcher.m).
PAGES = {'totk': ('settings', 'mods', 'controls', 'storage', 'saves'),
         'botw': ('settings', 'mods', 'controls', 'storage', 'saves'),
         'oot': ('controls', 'storage', 'saves'),
         'dread': ('controls',)}
# Metroid Dread has Controls only: its launcher (games/dread/app/launcher.m) reads DREAD_WORK and DREAD_DISPLAY and
# neither TOTK_PROFILE_DIR nor TOTK_SHADER_CACHE, which are what the Saves and Storage pages point at
# (saves.PROFILE_ENV, storage.SHADER_ENV). Giving it those pages would show pages with nothing behind them.
# Settings and Mods are not written for it either, as for Ocarina of Time.
