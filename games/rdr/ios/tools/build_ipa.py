#!/usr/bin/env python3
"""Red Dead Redemption on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py, the Zelda app
and tools) with this game's profile, as Mario Kart 8 Deluxe's (whose runtime this one is). From the Mac build's work
folder (games/rdr/README.md: rewritten/local the patched modules and their fixups, main's leading patch area included,
extracted/local/romfs the merged game data with its index, build/ the Mac host with its tables and the icon of the game's
control data, scripts/prepare_rdr_shader_seed.py the shader seed from shaders.rpf and main):

  python3 games/rdr/ios/tools/build_ipa.py --work <work> --out "<folder>/Red Dead Redemption.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Red Dead Redemption.ipa" --device "<name>" \\
      --game-data <work>/extracted/local/romfs/romfs.bin

What is this game's own:
- main's 1.2 GiB .bss and its 16 MiB leading patch area: the shared gen_embed.py embeds the file's parts (the patch area,
  the text, the data up to the end of the file); .bss and the key page are the loader's zero-filled reservation
  (runtime/loader.c map_module), not in the app;
- no system font: the game draws with its own fonts (fonts.rpf) and never asks for a shared one, on the Mac as here, so
  the app carries none (GameData/shared-fonts stays empty: no macOS font, no firmware font);
- no movie library: main decodes its Bink movies itself (no libvpx);
- its icon is the one of its control data (build/AppIcon.icns, scripts/build_rdr.py);
- the Mac's translations are the work folder's (no Mac app yet): build/shader-cache, or --mac-shader-cache."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402

CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
NO_FONTS = 'ios-fonts'   # an empty GameData/shared-fonts: the game asks for no system font


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/rdr-native', build=work / 'build', rewritten=work / 'rewritten/local',
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
            raise RuntimeError(f'missing {need}: build the Mac host and the shader seed first (games/rdr/README.md)')


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
    game='rdr', define='GAME_RDR', name='Red Dead Redemption', bundle_id='org.macnativegames.rdr', layout=layout,
    mac_cache=mac_cache, libs=('libopus.a',), prepare=prepare, check=check, pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
