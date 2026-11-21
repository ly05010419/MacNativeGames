#!/usr/bin/env python3
"""Animal Crossing: New Horizons on the iPhone / iPad: the shared port build (games/totk/ios/tools/build_port_ipa.py,
the Zelda app and tools) with this game's profile. From a Mac build's work folder (games/acnh/build.py: prepared/ the
verified game data, native/ the Mac host with its tables and shader seed, fonts/, dlc/):

  python3 games/acnh/ios/tools/build_ipa.py --work <work> --out "<folder>/Animal Crossing.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Animal Crossing.ipa" --device "<name>" \\
      --game-data <work>/prepared/update/romfs/romfs.bin

What is this game's own: main's text exported a second time with the 60 FPS mod's speed corrections (gen_embed.py
--alt-text; runtime/loader.c maps it when the picture chose 60, frame_rate.c writes only the data words), the add-ons and
the 60 FPS files in GameData, its pane's picture footer and MetalFX choices, libogg and libvorbis."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'totk/ios/tools'))
import build_port_ipa as port_ipa  # noqa: E402


def layout(work):
    native, prepared = work / 'native', work / 'prepared'
    raw = int(re.search(r'ACNH_VERSION_RAW\s+(\d+)', (native / 'build/version-config.h').read_text()).group(1))
    return port_ipa.Layout(
        host=native / 'build/acnh-native', build=native / 'build', rewritten=native / 'rewritten',
        fixups=lambda n: prepared / f'update/rewritten/{n}/{n}.fixups.tsv', romfs=prepared / 'update/romfs',
        fonts=work / 'fonts', seed=native / 'build/shader-seed',
        version={458752: '1.2.1', 2097152: '3.0.1', 2228224: '3.0.3'}.get(raw, str(raw)))


def exports(host, rewritten, gen, env, run, logs, work):
    """The original 30 FPS text, and main's once more with the 60 FPS speed corrections (frame_rate.c)."""
    fps60 = work / 'native/build/fps60'
    clean = {k: v for k, v in env.items() if k not in ('ACNH_FPS_ASSETS', 'ACNH_FPS120_ASSETS')}
    run([host, rewritten, 'export-ios'], work, logs / 'export-ios.log', dict(clean, TOTK_EXPORT_IOS=str(gen / 'modules'), ACNH_FPS='30'))
    if not (fps60 / 'manifest.json').is_file():
        print('frame rate: this build has no 60 FPS data (games/acnh/build.py --no-fps-mod): 30 FPS only')
        return []
    m60 = gen / 'modules-60fps'
    if m60.exists(): __import__('shutil').rmtree(m60)
    m60.mkdir()
    run([host, rewritten, 'export-ios'], work, logs / 'export-ios-60fps.log',
        dict(clean, TOTK_EXPORT_IOS=str(m60), ACNH_FPS='60', ACNH_FPS_ASSETS=str(fps60)))
    return ['--alt-text', f'main={m60}']


def game_data(res, work, L, extra):
    """The add-ons, and the 60 FPS mod's files when its text is in (its checked manifest, data words, RomFS overlay)."""
    for folder, dest, want in ((work / 'dlc', 'dlc', (work / 'dlc/manifest.json').is_file()),
                               (work / 'native/build/fps60', 'fps60', bool(extra))):
        if want:
            for f in sorted(folder.rglob('*')):
                if f.is_file():
                    port_ipa.clone(f, res / dest / f.relative_to(folder))


def pane(specs):
    """No sync folder, no shader inbox (as on its Mac); the profiles' footer."""
    port_ipa.drop_group(specs, 'TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'})
    port_ipa.drop_group(specs, 'TOTK_CONTRIBUTE_SHADERS', {'TOTK_CONTRIBUTE_SHADERS'})
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，30 fps，最清晰，會發熱。效能：720p 掌機模式，60 fps（60 幀補丁），流暢。省電：720p 掌機模式，30 fps，'
        '發熱最少、最省電。自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。'
        '重新啟動遊戲後生效 · Quality: 1080p docked, 30 fps, the sharpest, runs warm. Performance: 720p handheld, 60 fps (the '
        '60 FPS mod), smooth. Economy: 720p handheld, 30 fps, the coolest. Custom: what you '
        'chose under Advanced on the game\'s start screen. MetalFX: enlarges the picture to the screen, sharper and heavier '
        'on the GPU. Takes effect after relaunching the game')


PORT = port_ipa.Port(
    game='acnh', define='GAME_ACNH', name='Animal Crossing', bundle_id='org.macnativegames.acnh', layout=layout,
    mac_cache=lambda L: Path.home() / f'Library/Application Support/MacNativeGames/ACNH-{L.version}/shader-cache',
    cflags=lambda L: f'-include {L.build / "version-config.h"}',
    libs=('libopus.a', 'libvpx.a', 'libvorbis.a', 'libogg.a'), deps_args=('--vorbis',),
    exports=exports, game_data=game_data, pane=pane)

if __name__ == '__main__':
    port_ipa.cli(PORT)
