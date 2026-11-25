#!/usr/bin/env python3
"""Persona 5 Royal on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py, the Zelda app
and tools) with this game's profile, from the Mac build's work folder (games/p5r/README.md).

Not made yet (README.md, "Not yet"): the shader seed tool scripts/prepare_p5r_shader_seed.py is still to be written, so
check() below stops before anything is packed. This script and Sources/ios_front.m came with the folder this game's was
made from (3d36dbbe) and until 2026-11-25 still declared that game's identity; the identity is this game's now (game,
GAME_P5R, bundle id, build/p5r-native; tests/test_game_identity.py), the layout and the checks are still to be gone over
for this game when its iPhone app is made.

  python3 games/p5r/ios/tools/build_ipa.py --work <work> --out "<folder>/Persona 5 Royal.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Persona 5 Royal.ipa" --device "<name>" \\
      --game-data <work>/extracted/local/romfs/romfs.bin"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402

CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
NO_FONTS = 'ios-fonts'   # an empty GameData/shared-fonts: the game asks for no system font


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/p5r-native', build=work / 'build', rewritten=work / 'rewritten/local',
        fixups=lambda n: work / f'rewritten/local/{n}.fixups.tsv', romfs=work / 'extracted/local/romfs',
        fonts=work / NO_FONTS, seed=work / 'build/shader-seed', version='1.0.4')   # icon: build/AppIcon.icns


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
            raise RuntimeError(f'missing {need}: build the Mac host and the shader seed first (games/p5r/README.md)')


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
    game='p5r', define='GAME_P5R', name='Persona 5 Royal', bundle_id='org.macnativegames.p5r', layout=layout,
    mac_cache=mac_cache, libs=('libopus.a',), prepare=prepare, check=check, pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
