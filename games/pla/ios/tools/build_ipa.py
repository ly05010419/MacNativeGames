#!/usr/bin/env python3
"""Pokémon Legends: Arceus on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py, the Zelda
app and tools) with this game's profile. From the Mac build's work folder (games/pla/README.md: extracted/v1101 the
verified game data, rewritten/v1101 the patched modules and their fixups, build/ the Mac host with its tables, the shader
seed and the icon, scripts/prepare_pla_shader_seed.py):

  python3 games/pla/ios/tools/build_ipa.py --work <work> --out "<folder>/Pokemon Legends Arceus.ipa" --image-outside \\
      --mac-shader-cache "<Mac app>/Contents/Resources/Work/build/shader-cache/rt-totk-native-v4-unverified"
  python3 games/totk/ios/tools/install_device.py "<folder>/Pokemon Legends Arceus.ipa" --device "<name>" \\
      --game-data <work>/extracted/v1101/romfs/romfs.bin

What is this game's own:
- its shared font: one plain TrueType font for every type (runtime/shared_font.c PLA_SHARED_FONT_TTF): the project's
  open-source Noto Sans SC (tools/deps/open_fonts.py), carried in GameData/shared-fonts as stand-in.ttf with OFL.txt;
- its shader sources get the fixes its Mac runtime makes before compiling them (nvn_mtl.m msl_init_vertex_outputs /
  msl_zero_unwritten_colors, the host's msl-rewrite) before the device's libraries are compiled from them;
- its Mac app keeps the shaders it translated inside the app (Contents/Resources/Work/build/shader-cache), not in
  Application Support: give it with --mac-shader-cache, or the work folder's own (PLA_DEV builds) is taken."""
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402

CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
# The project's open-source shared fonts (tools/deps/open_fonts.py, Noto, SIL OFL 1.1; user 2026-11-20: "没有字体的情况下 自动使用
# 开源字体"), not a system font: the phone carries no font of macOS. Slot 1, Noto Sans SC, holds Latin, kana and the CJK
# characters, so one font serves every text of the game (runtime/shared_font.c PLA_SHARED_FONT_TTF takes one TrueType).
OPEN_FONT_SLOT = '1-chinese-simplified.bfttf'
STAND_IN = 'stand-in.ttf'   # the name the shared app's main.m passes as PLA_SHARED_FONT_TTF


def layout(work):
    return port_ipa.Layout(
        host=work / 'build/pla-native', build=work / 'build', rewritten=work / 'rewritten/v1101',
        fixups=lambda n: work / f'rewritten/v1101/{n}.fixups.tsv', romfs=work / 'extracted/v1101/romfs',
        fonts=work / 'ios-fonts', seed=work / 'build/shader-seed', version='1.1.1')   # icon: build/AppIcon.icns (Makefile)


def mac_cache(L):
    """The Mac app's translations: a PLA_DEV app's are the work folder's; a packaged app's are inside it (--mac-shader-cache)."""
    work = L.build.parent
    for app in (work.parent / 'Pokemon Legends Arceus.app', work.parent / 'pla-app/Pokemon Legends Arceus.app'):
        if (app / 'Contents/Resources/Work/build' / CHAIN_CACHE).is_dir():
            return app / 'Contents/Resources/Work/build' / CHAIN_CACHE
    return L.build / CHAIN_CACHE


def prepare(L, gen, env, run, logs, work):
    """The stand-in font in the fonts folder the app carries (GameData/shared-fonts): the open-source Noto Sans SC as plain
    TrueType (the BFTTF's payload after its 8-byte header, key 0), with its licence beside it."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
    from tools.deps.open_fonts import open_fonts
    folder = open_fonts()
    data = (folder / OPEN_FONT_SLOT).read_bytes()[8:]
    if data[:4] != b'\x00\x01\x00\x00':
        raise RuntimeError(f'{folder / OPEN_FONT_SLOT}: not a TrueType payload')
    L.fonts.mkdir(parents=True, exist_ok=True)
    dest = L.fonts / STAND_IN
    if not dest.is_file() or dest.read_bytes() != data:
        dest.unlink(missing_ok=True); dest.write_bytes(data)
    for stale in L.fonts.glob('*'):   # nothing else in the folder the app carries (a system font from an earlier build)
        if stale.name not in (STAND_IN, 'OFL.txt'): stale.unlink()
    shutil.copyfile(folder / 'OFL.txt', L.fonts / 'OFL.txt')


def check(L):
    """What the Mac build leaves (build_port_ipa.check_mac_build, with the stand-in font instead of firmware fonts)."""
    for need in (L.host, L.build / 'runtime_tables.h', L.romfs / 'romfs.bin', L.romfs / 'runtime-index.bin',
                 L.seed / 'seed-manifest.json', L.fonts / STAND_IN):
        if not need.exists():
            raise RuntimeError(f'missing {need}: build the Mac app and its shader seed first (games/pla/README.md, '
                               'scripts/prepare_pla_shader_seed.py)')


def sources_fix(L, folder, run, log, env):
    """The Mac runtime's MSL fixes in the seed's sources, by the host itself (nvn_mtl.m mtl_msl_rewrite_tool)."""
    run([L.host, L.rewritten, 'msl-rewrite', folder], folder.parent, log, env)


def pane(specs):
    """No sync folder (its file system keeps the saves in Documents only), no shader inbox; the profiles' footer (its own
    30 fps step, no frame-rate choice)."""
    port_ipa.drop_group(specs, 'TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'})
    port_ipa.drop_group(specs, 'TOTK_CONTRIBUTE_SHADERS', {'TOTK_CONTRIBUTE_SHADERS'})
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，最清晰，會發熱。效能：720p 掌機模式，流暢。省電：720p 掌機模式，發熱最少、最省電。'
        '自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。遊戲以自己的 30 fps 運行。'
        '重新啟動遊戲後生效 · Quality: 1080p docked, the sharpest, runs warm. Performance: 720p handheld, smooth. Economy: '
        '720p handheld, the coolest. Custom: what you chose under Advanced on the game\'s start screen. MetalFX: enlarges the '
        'picture to the screen, sharper and heavier on the GPU. The game runs at its own 30 fps. Takes effect after '
        'relaunching the game')


PORT = port_ipa.Port(
    game='pla', define='GAME_PLA', name='Pokemon Legends Arceus', bundle_id='org.macnativegames.pla', layout=layout,
    mac_cache=mac_cache, prepare=prepare, check=check, sources_fix=sources_fix, pane=pane,
    # its 32 GiB address region (runtime/services.c VAMM) needs the larger address space
    entitlements=('com.apple.developer.kernel.extended-virtual-addressing',))

if __name__ == '__main__':
    port_ipa.cli(PORT)
