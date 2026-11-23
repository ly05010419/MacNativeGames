#!/usr/bin/env python3
"""Mario Kart 8 Deluxe on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py, the Zelda app
and tools) with this game's profile, as Super Mario Odyssey's (whose runtime this one is). From the Mac build's work
folder (games/mk8dx/README.md: rewritten/local the patched modules and their fixups, extracted/local/romfs the game data's
index beside its image, extracted/firmware the player's shared fonts and Mii parts, extracted/aoc the Booster Course
Pass, build/ the Mac host with its tables, scripts/prepare_mk8dx_shader_seed.py the shader seed):

  python3 games/mk8dx/ios/tools/build_ipa.py --work <work> --out "<folder>/Mario Kart 8 Deluxe.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Mario Kart 8 Deluxe.ipa" --device "<name>" \\
      --game-data <work>/extracted/local/romfs/romfs.bin

What is this game's own:
- the Mii parts of the player's firmware (system data MiiModel, scripts/extract_mii_model.py; runtime/mii_model.c
  TOTK_MII_MODEL) and the add-on content (runtime/fs_host.c TOTK_AOC_DIR: the Booster Course Pass's two small files)
  travel in the app's GameData; the system fonts are the player's firmware fonts, as on the Mac;
- its icon is the one of its control data (extracted/local/icon.jpg, beside control.nacp);
- the Mac's translations are the work folder's (no Mac app yet): build/shader-cache, or --mac-shader-cache."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402

CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
# The game's own files beside the image, under these GameData names (games/totk/ios/Sources/main.m GAME_MK8DX block)
GAME_DATA = {'mii-model': 'extracted/firmware/mii-model', 'aoc': 'extracted/aoc'}


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/mk8dx-native', build=work / 'build', rewritten=work / 'rewritten/local',
        fixups=lambda n: work / f'rewritten/local/{n}.fixups.tsv', romfs=work / 'extracted/local/romfs',
        fonts=work / 'extracted/firmware/shared-fonts', seed=work / 'build/shader-seed', version='4.0.1',
        icon=work / 'extracted/local/icon.jpg')   # the game's control data icon, beside its control.nacp


def mac_cache(L):
    """The translations the Mac host made while playing: the work folder's (its runs from the work folder, README.md)."""
    return L.build / CHAIN_CACHE


def game_data(res, work, L, extra):
    """The Mii parts and the add-on content into GameData (cloned; a few MB), every file of their folders."""
    for name, rel in GAME_DATA.items():
        src = work / rel
        if not src.is_dir():
            raise RuntimeError(f'missing {src}: build the Mac host first (games/mk8dx/README.md)')
        for f in sorted(src.rglob('*')):
            if f.is_file() and not f.name.startswith('.'):
                port_ipa.clone(f, res / name / f.relative_to(src))


def pane(specs):
    """No sync folder (its file system keeps the saves in Documents only), no shader inbox; the profiles' footer (its own
    60 fps, no frame-rate choice)."""
    port_ipa.drop_group(specs, 'TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'})
    port_ipa.drop_group(specs, 'TOTK_CONTRIBUTE_SHADERS', {'TOTK_CONTRIBUTE_SHADERS'})
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，最清晰，會發熱。效能：720p 掌機模式，流暢。省電：720p 掌機模式，發熱最少、最省電。'
        '自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。重新啟動遊戲後生效 · Quality: 1080p docked, '
        'the sharpest, runs warm. Performance: 720p handheld, smooth. Economy: 720p handheld, the '
        'coolest. Custom: what you chose under Advanced on the game\'s start screen. MetalFX: enlarges the picture to the screen, '
        'sharper and heavier on the GPU. Takes effect after relaunching the game')


PORT = port_ipa.Port(
    game='mk8dx', define='GAME_MK8DX', name='Mario Kart 8 Deluxe', bundle_id='org.macnativegames.mk8dx', layout=layout,
    mac_cache=mac_cache, game_data=game_data, pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
