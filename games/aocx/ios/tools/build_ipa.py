#!/usr/bin/env python3
"""Hyrule Warriors: Age of Calamity on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py,
the Zelda app and tools) with this game's profile, as Red Dead Redemption's and Mario Kart 8 Deluxe's (whose runtime this
one is). From the Mac build's work folder (games/aocx/README.md: rewritten/local the patched modules and their fixups,
extracted/local/romfs the merged game data with its index, extracted/aoc the add-on content, build/ the Mac host with its
tables and the icon of the game's control data, scripts/prepare_aocx_shader_seed.py the shader seed from the game's RDB
data files and main):

  python3 games/aocx/ios/tools/build_ipa.py --work <work> --out "<folder>/Age of Calamity.ipa" --image-outside \\
      --device-gpu applegpu_g14p --device-gpu applegpu_g15p
  python3 games/totk/ios/tools/install_device.py "<folder>/Age of Calamity.ipa" --device "<name>" \\
      --game-data <work>/extracted/local/romfs/romfs.bin

What is this game's own:
- the add-on content (runtime/fs_host.c TOTK_AOC_DIR: the four add-ons the player owns, 1.5 GB) travels in the app's
  GameData, as Mario Kart's Booster Course Pass;
- no system font: the game draws with its own fonts and never asks for a shared one, on the Mac as here, so the app
  carries none (GameData/shared-fonts stays empty: no macOS font, no firmware font);
- its VP9 movies are decoded by the host's libvpx (runtime/movie_host.c, the shared one), built for the device as
  Tears of the Kingdom's (the port build's default libraries);
- its icon is the one of its control data (build/AppIcon.icns, scripts/build_aocx.py);
- the Mac's translations are the work folder's (no Mac app yet): build/shader-cache, or --mac-shader-cache."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402

CHAIN_CACHE = 'shader-cache/rt-aocx-native-v6-shared-memory'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
NO_FONTS = 'ios-fonts'   # an empty GameData/shared-fonts: the game asks for no system font
GAME_DATA = {'aoc': 'extracted/aoc'}   # GameData names (games/totk/ios/Sources/main.m GAME_AOCX block)


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/aocx-native', build=work / 'build', rewritten=work / 'rewritten/local',
        fixups=lambda n: work / f'rewritten/local/{n}.fixups.tsv', romfs=work / 'extracted/local/romfs',
        fonts=work / NO_FONTS, seed=work / 'build/shader-seed', version='1.4.1')   # icon: build/AppIcon.icns


def mac_cache(L):
    """The translations the Mac host made while playing: the work folder's (its runs from the work folder, README.md)."""
    return L.build / CHAIN_CACHE


def prepare(L, gen, env, run, logs, work):
    """The fonts folder the app carries, empty (a font left by an earlier build is removed)."""
    L.fonts.mkdir(parents=True, exist_ok=True)
    for stale in L.fonts.iterdir():
        if stale.is_file(): stale.unlink()


def check(L):
    """What the Mac build leaves (build_port_ipa.check_mac_build without the firmware fonts this game does not use)."""
    for need in (L.host, L.build / 'runtime_tables.h', L.romfs / 'romfs.bin', L.romfs / 'runtime-index.bin',
                 L.seed / 'seed-manifest.json', L.build / 'AppIcon.icns'):
        if not need.exists():
            raise RuntimeError(f'missing {need}: build the Mac host and the shader seed first (games/aocx/README.md)')


def game_data(res, work, L, extra):
    """The add-on content into GameData (cloned), every file of the extracted add-ons' folders; their reports (<n>.json)
    stay in the work folder (runtime/fs_host.c lists the numbered folders only)."""
    for name, rel in GAME_DATA.items():
        src = work / rel
        if not src.is_dir():
            raise RuntimeError(f'missing {src}: extract the add-on content first (scripts/extract_dlc.py)')
        for f in sorted(src.rglob('*')):
            if f.is_file() and not f.name.startswith('.') and f.parent != src:
                port_ipa.clone(f, res / name / f.relative_to(src))


def pane(specs):
    """No sync folder (its file system keeps the saves in Documents only), no shader inbox; the profiles' footer (its own
    30 fps, no frame-rate choice)."""
    port_ipa.drop_group(specs, 'TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'})
    port_ipa.drop_group(specs, 'TOTK_CONTRIBUTE_SHADERS', {'TOTK_CONTRIBUTE_SHADERS'})
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，最清晰，會發熱。效能：720p 掌機模式，流暢。省電：720p 掌機模式，發熱最少、最省電。'
        '自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。遊戲以自己的 30 FPS 執行。重新啟動遊戲後生效 · '
        'Quality: 1080p docked, the sharpest, runs warm. Performance: 720p handheld, smooth. Economy: 720p handheld, the '
        'coolest. Custom: what you chose under Advanced on the game\'s start screen. MetalFX: enlarges the picture to the screen, '
        'sharper and heavier on the GPU. The game runs at its own 30 FPS. Takes effect after relaunching the game')


PORT = port_ipa.Port(
    game='aocx', define='GAME_AOCX', name='Age of Calamity', bundle_id='org.macnativegames.aocx', layout=layout,
    mac_cache=mac_cache, prepare=prepare, check=check, game_data=game_data, pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
