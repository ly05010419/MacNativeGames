#!/usr/bin/env python3
"""Super Mario Odyssey on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py, the Zelda
app and tools) with this game's profile. From the Mac build's work folder (games/odyssey/build.py: rewritten/ the
patched modules and their fixups, extracted/local/romfs the game data, extracted/firmware/shared-fonts, build/ the Mac
host with its tables and shader seed):

  python3 games/odyssey/ios/tools/build_ipa.py --work <work> --out "<folder>/Super Mario Odyssey.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Super Mario Odyssey.ipa" --device "<name>" \\
      --game-data <work>/extracted/local/romfs/romfs.bin"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/odyssey-native', build=work / 'build', rewritten=work / 'rewritten',
        fixups=lambda n: work / f'rewritten/{n}.fixups.tsv', romfs=work / 'extracted/local/romfs',
        fonts=work / 'extracted/firmware/shared-fonts', seed=work / 'build/shader-seed', version='1.3.0',
        icon=work / 'app-assets/OdysseyIcon.jpg')   # its Mac build's (scripts/extract_app_icon.py)


def pane(specs):
    """No sync folder (its file system keeps the saves in Documents only); the profiles' footer (no frame-rate choice)."""
    port_ipa.drop_group(specs, 'TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'})
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，最清晰，會發熱。效能：720p 掌機模式，流暢。省電：720p 掌機模式，發熱最少、最省電。'
        '自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。重新啟動遊戲後生效 · Quality: 1080p docked, '
        'the sharpest, runs warm. Performance: 720p handheld, smooth. Economy: 720p handheld, the '
        'coolest. Custom: what you chose under Advanced on the game\'s start screen. MetalFX: enlarges the picture to the screen, '
        'sharper and heavier on the GPU. Takes effect after relaunching the game')


PORT = port_ipa.Port(
    game='odyssey', define='GAME_ODYSSEY', name='Super Mario Odyssey', bundle_id='org.macnativegames.odyssey', layout=layout,
    mac_cache=lambda L: Path.home() / 'Library/Application Support/MacNativeGames/Odyssey/shader-cache/rt-totk-native-v4-unverified',
    pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
