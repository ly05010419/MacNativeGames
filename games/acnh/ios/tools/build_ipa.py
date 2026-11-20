#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone / iPad app of Animal Crossing: New Horizons from a Mac build's work folder
(games/acnh/build.py: prepared/ the verified game data, native/ the Mac host with its generated tables and shader seed,
fonts/ and dlc/), with the Zelda ports' iPhone / iPad app and tools: the app's sources are games/totk/ios/Sources (built with
GAME_ACNH), the embed, dependency and Xcode tools games/totk/ios/tools, and so is the installer:

  python3 games/acnh/ios/tools/build_ipa.py --work <work> --out "<folder>/Animal Crossing.ipa" --image-outside
  python3 games/totk/ios/tools/install_device.py "<folder>/Animal Crossing.ipa" --device "<name>" \
      --game-data <work>/prepared/update/romfs/romfs.bin

tests/test_acnh_ios_unified.py keeps it that way (user 2026-11-20: "统一 统一 统一！！！ 这是规范 要求！！！ 用测试保证").

iOS cannot execute code written at run time, so the rewritten modules are compiled into the app (gen_embed.py; the loader
maps them back, runtime/loader.c), with the game patches applied on the Mac (loader.c export-ios): the original text, and
main's once more with the 60 FPS mod's speed corrections (gen_embed.py --alt-text), mapped when the picture chose 60. The
device cannot translate shaders either: the app carries the prepared seed (native/build/shader-seed) and what this Mac
translated while playing (--mac-shader-cache, the Mac app's shader cache); a variant neither has is skipped on the device."""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path
ZELDA_TOOLS = Path(__file__).resolve().parents[3] / 'totk/ios/tools'   # Zelda's iOS tools, used as they are
sys.path.insert(0, str(ZELDA_TOOLS))
from xcode_dir import developer_dir

GAME = Path(__file__).resolve().parents[2]   # games/acnh
REPO = GAME.parents[1]
TOTK = REPO / 'games/totk'
SHELL = TOTK / 'ios/Sources'   # the shared iPhone / iPad app (user 2026-11-20: "塞尔达的 设置 都移植过来 要统一")
APP = 'TOTK'   # the product name: TOTK.app, as the shared shell's, which Zelda's install_device.py signs and installs
BUNDLE_ID = 'org.macnativegames.acnh'
LIBS = ('libopus.a', 'libvpx.a', 'libvorbis.a', 'libogg.a')
# Mac-only probes (IOKit, Cocoa windows), left out of the iPhone app; its loader never reaches their modes there
MAC_ONLY = ('frontend.m', 'loader.c')
# Notes, generators and included fragments in the source folders: not app resources (the .inc files are #included)
NOT_SOURCES = ('**/*.md', '**/*.py', '**/*.txt', '**/*.inc')
# The home screen icon as plain PNGs that Info.plist names: an asset catalog needs actool, which asks for a simulator
# runtime this Xcode may not have
ICONS = (('AppIcon60x60@2x.png', 120), ('AppIcon60x60@3x.png', 180), ('AppIcon76x76@2x~ipad.png', 152),
         ('AppIcon83.5x83.5@2x~ipad.png', 167))
ICON_PLIST = '''
        CFBundleIcons:
          CFBundlePrimaryIcon:
            CFBundleIconFiles: [AppIcon60x60]
        CFBundleIcons~ipad:
          CFBundlePrimaryIcon:
            CFBundleIconFiles: [AppIcon60x60, AppIcon76x76, AppIcon83.5x83.5]'''


def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-35:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))


def clone(src, dst):
    """An APFS clone (cp -c): the 10 GB RomFS image takes no extra space and no time on the same volume."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode:
        shutil.copy2(src, dst)


def module_names(tables):
    """The modules in the loader's order, as the generated tables list them."""
    names = re.findall(r'\{\.name="([A-Za-z0-9_]+)",\.digest=', tables.read_text(errors='replace'))
    if not names:
        raise RuntimeError(f'no modules in {tables}')
    return names


def version_of(native):
    text = (native / 'build/version-config.h').read_text()
    raw = int(re.search(r'ACNH_VERSION_RAW\s+(\d+)', text).group(1))
    return {458752: '1.2.1', 2097152: '3.0.1', 2228224: '3.0.3'}.get(raw, str(raw))


def project(work, gen, flags_file, version):
    """The Xcode project spec, with absolute paths: the sources are games/acnh's, everything generated is the work folder's."""
    rt, gfx, build = GAME / 'runtime', GAME / 'third_party/d2r_gfx/gfx', work / 'native/build'
    excludes = ''.join(f'\n          - "{name}"' for name in MAC_ONLY + NOT_SOURCES)
    not_sources = ''.join(f'\n          - "{name}"' for name in NOT_SOURCES)
    icon_plist = ICON_PLIST if (gen / 'icons' / ICONS[0][0]).is_file() else ''
    return f'''name: ACNHIOS
options:
  bundleIdPrefix: org.macnativegames
  deploymentTarget:
    iOS: "26.0"
configFiles:
  Release: {flags_file}
  Debug: {flags_file}
targets:
  ACNHIOS:
    type: application
    platform: iOS
    sources:
      - path: {SHELL}   # the iPhone / iPad app every Zelda port uses (start screen, picture, touch, settings)
        excludes:
          - TOTK.entitlements
          - Info.plist
          - "*.lproj"   # its Home Screen menu's strings (save sync, which Animal Crossing has not)
      - path: {GAME / 'ios/Sources'}   # Animal Crossing's answers to what the shell asks of a runtime (ios_front.m)
        excludes:
          - ACNH.entitlements
          - Info.plist
      - path: {gen / 'Settings.bundle'}   # the shell's pane for this game (acnh_pane)
      - path: {rt}
        excludes:{excludes}
      - path: {gfx / 'nvn'}
        excludes:{not_sources}
      - path: {gfx / 'tex'}
        excludes:{not_sources}
          - nvn_bctc.metal   # the runtime compiles its copy from source (nvn_mtl.m k_bctc_msl)
      - path: {gen / 'embed.S'}
      - path: {rt / 'loader.c'}
        compilerFlags: "-Dmain=totk_host_main"
    info:
      path: {gen / 'Info.plist'}
      properties:
        CFBundleDisplayName: Animal Crossing
        CFBundleName: Animal Crossing
        CFBundleIdentifier: $(PRODUCT_BUNDLE_IDENTIFIER)
        CFBundleShortVersionString: "{version}"
        CFBundleVersion: "1"
        UILaunchScreen: {{}}{icon_plist}
        UIFileSharingEnabled: true
        LSSupportsOpeningDocumentsInPlace: true
        CFBundleURLTypes:   # the iPhone MNG Launcher sees the game installed and starts it (tools/ios_launcher)
          - CFBundleURLName: org.macnativegames.launch
            CFBundleURLSchemes:
              - mng-acnh
        UIRequiresFullScreen: true
        UISupportedInterfaceOrientations:
          - UIInterfaceOrientationLandscapeLeft
          - UIInterfaceOrientationLandscapeRight
        UIStatusBarHidden: true
        UIViewControllerBasedStatusBarAppearance: false
        UIApplicationSupportsIndirectInputEvents: true
        GCSupportsControllerUserInteraction: true
        NSMotionUsageDescription: Tilt the device to move the camera while you play with the on-screen controls.
        GCSupportedGameControllers:
          - ProfileName: ExtendedGamepad
        CADisableMinimumFrameDurationOnPhone: true
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default
                UISceneDelegateClassName: TOTKSceneDelegate
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: {BUNDLE_ID}
      PRODUCT_NAME: {APP}
      CODE_SIGN_ENTITLEMENTS: {GAME / 'ios/Sources/ACNH.entitlements'}
      TARGETED_DEVICE_FAMILY: "1,2"
      IPHONEOS_DEPLOYMENT_TARGET: "26.0"
      CODE_SIGN_STYLE: Manual
      CODE_SIGNING_ALLOWED: NO
      ENABLE_BITCODE: NO
      GCC_C_LANGUAGE_STANDARD: gnu11
      CLANG_ENABLE_OBJC_ARC: YES
      GCC_WARN_INHIBIT_ALL_WARNINGS: YES
      HEADER_SEARCH_PATHS:
        - $(inherited)
        - {gfx / 'nvn'}
        - {gfx / 'nvn/include'}
        - {gen}
        - {gen / 'deps/include'}
        - {gen / 'deps/include/opus'}
        - {build}
      OTHER_CFLAGS: $(inherited) -DGAME_ACNH=1 -include {build / 'version-config.h'} -I{SHELL} -mcpu=apple-a15 -I{rt} -I{gfx / 'nvn'} -I{gfx / 'nvn/include'} -I{gen} -I{build} -idirafter {TOTK / 'runtime'} -idirafter {TOTK / 'third_party/d2r_gfx/gfx/nvn'}
'''


def acnh_pane(bundle):
    """The Settings app pane: the shell's own (games/totk/ios/Settings.bundle) for this game (its tools/build_ipa.py
    pane_for 'acnh': the picture profile and MetalFX with the other rows everyone has), without the save sync and shader
    contribution rows (Animal Crossing keeps its saves in Documents only and has no shader inbox, as on its Mac) and with the picture footer saying what its profiles are; then in the
    device's language (settings_strings.py), as every Zelda port's."""
    import importlib.util, plistlib
    tools = TOTK / 'ios/tools'
    sys.path.insert(0, str(tools))
    spec = importlib.util.spec_from_file_location('totk_build_ipa', tools / 'build_ipa.py')
    totk = importlib.util.module_from_spec(spec); spec.loader.exec_module(totk)
    shutil.rmtree(bundle, ignore_errors=True); shutil.copytree(TOTK / 'ios/Settings.bundle', bundle)
    root = totk.pane_for(plistlib.loads((bundle / 'Root.plist').read_bytes()), 'acnh')
    specs = root['PreferenceSpecifiers']
    for first, group in (('TOTK_CLOUD_SAVE_FOLDER_NAME', {'TOTK_CLOUD_SAVE_FOLDER_NAME', 'TOTK_CLOUD_SAVE_PICK', 'TOTK_CLOUD_SAVE_STOP'}),
                         ('TOTK_CONTRIBUTE_SHADERS', {'TOTK_CONTRIBUTE_SHADERS'})):   # its own: no sync folder, no shader inbox
        at = next(i for i, x in enumerate(specs) if x.get('Key') == first)
        del specs[at - 1]   # the group heading
        specs[:] = [x for x in specs if x.get('Key') not in group]
    at = next(i for i, x in enumerate(specs) if x.get('Key') == 'TOTK_PICTURE_PROFILE')
    specs[at - 1]['FooterText'] = (
        '品質：1080p 底座模式，30 fps，最清晰，會發熱。效能：720p 掌機模式，60 fps（60 幀補丁），流暢。省電：720p 掌機模式，30 fps，'
        'Switch 螢幕大小（iPad），發熱最少、最省電。自訂：遊戲開始畫面「進階」裡選的設定。MetalFX：把畫面放大到螢幕，更清晰，更吃 GPU。'
        '重新啟動遊戲後生效 · Quality: 1080p docked, 30 fps, the sharpest, runs warm. Performance: 720p handheld, 60 fps (the '
        '60 FPS mod), smooth. Economy: 720p handheld, 30 fps, the Switch\'s screen size (iPad), the coolest. Custom: what you '
        'chose under Advanced on the game\'s start screen. MetalFX: enlarges the picture to the screen, sharper and heavier '
        'on the GPU. Takes effect after relaunching the game')
    # MetalFX: off, or the picture to the screen (Animal Crossing has no smaller scene for the scene stage; its Mac's row)
    mfx = next(x for x in specs if x.get('Key') == 'TOTK_METALFX')
    mfx['Values'] = ['off', 'screen']
    mfx['Titles'] = ['關 · Off', '到螢幕 · To the screen']
    mfx['DefaultValue'] = 'off'
    (bundle / 'Root.plist').write_bytes(plistlib.dumps(root))
    totk.settings_strings.localize(bundle)


def default_mac_cache(version):
    return Path.home() / f'Library/Application Support/MacNativeGames/ACNH-{version}/shader-cache'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True, help='the Mac build\'s work folder (games/acnh/build.py --work)')
    p.add_argument('--out', type=Path, required=True, help='.ipa, or .app for the unsigned folder (install_device.py takes both)')
    p.add_argument('--image-outside', action='store_true', help='leave romfs.bin out: the device keeps it in Documents/GameData '
                   '(games/totk/ios/tools/install_device.py --game-data <work>/prepared/update/romfs/romfs.bin)')
    p.add_argument('--mac-shader-cache', type=Path, help='the shaders the Mac app translated while playing (default: the '
                   'Mac app\'s, ~/Library/Application Support/MacNativeGames/ACNH-<version>/shader-cache); read only')
    a = p.parse_args()
    work, out = a.work.expanduser().resolve(), a.out.expanduser().resolve()
    if out.suffix.lower() not in ('.ipa', '.app'):
        raise RuntimeError('the output must end in .ipa or .app')
    native, prepared = work / 'native', work / 'prepared'
    ios = work / 'ios'; gen = ios / 'Gen'; logs = work / 'logs/ios'
    gen.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    subprocess.run(['xcodebuild', '-version'], env=env, check=True, stdout=subprocess.DEVNULL)
    sys.path.insert(0, str(REPO))   # the repository's tools/deps: pinned xcodegen, cmake and ninja (no Homebrew)
    from tools.deps import path as deps_path
    env['PATH'] = os.pathsep.join(deps_path('xcodegen', 'cmake', 'ninja') + [env.get('PATH', '')])

    host, tables = native / 'build/acnh-native', native / 'build/runtime_tables.h'
    romfs = prepared / 'update/romfs'
    for need in (host, tables, romfs / 'romfs.bin', romfs / 'runtime-index.bin', work / 'fonts/manifest.json'):
        if not need.exists():
            raise RuntimeError(f'missing {need}: build the Mac app first (games/acnh/build.py)')
    version = version_of(native)
    names = module_names(tables)

    # gen_embed.py reads <name>.patched.bin and <name>.fixups.tsv side by side
    staged = gen / 'rewritten'; shutil.rmtree(staged, ignore_errors=True); staged.mkdir()
    for name in names:
        (staged / f'{name}.patched.bin').symlink_to(native / f'rewritten/{name}.patched.bin')
        (staged / f'{name}.fixups.tsv').symlink_to(prepared / f'update/rewritten/{name}/{name}.fixups.tsv')

    # The rewritten modules as the game patches leave them (loader.c export-ios), compiled into the executable's
    # segments: the original 30 FPS text, and main's text once more with the 60 FPS speed corrections applied
    # (frame_rate.c; iOS cannot patch code at run time, so the loader maps that text when the game runs at 60).
    fps60 = native / 'build/fps60'
    export_env = {k: v for k, v in env.items() if k not in ('ACNH_FPS_ASSETS', 'ACNH_FPS120_ASSETS')}
    modules = gen / 'modules'; shutil.rmtree(modules, ignore_errors=True); modules.mkdir()
    run([host, native / 'rewritten', 'export-ios'], work, logs / 'export-ios.log',
        dict(export_env, TOTK_EXPORT_IOS=str(modules), ACNH_FPS='30'))
    alt = []
    if (fps60 / 'manifest.json').is_file():
        modules60 = gen / 'modules-60fps'; shutil.rmtree(modules60, ignore_errors=True); modules60.mkdir()
        run([host, native / 'rewritten', 'export-ios'], work, logs / 'export-ios-60fps.log',
            dict(export_env, TOTK_EXPORT_IOS=str(modules60), ACNH_FPS='60', ACNH_FPS_ASSETS=str(fps60)))
        alt = ['--alt-text', f'main={modules60}']
    else:
        print('frame rate: this build has no 60 FPS data (games/acnh/build.py --no-fps-mod): 30 FPS only')
    flags = subprocess.check_output([sys.executable, ZELDA_TOOLS / 'gen_embed.py', staged, gen / 'embed.S',
                                     '--text-dir', modules, *alt, *names], text=True, env=env).strip()

    # Opus, libvpx, libvorbis and libogg for the device: the Mac archives (tools/deps) cannot be linked into an iOS app.
    deps = work / 'ios-deps'
    if not all((deps / 'lib' / lib).is_file() for lib in LIBS):
        run([sys.executable, ZELDA_TOOLS / 'build_deps.py', deps, 'iphoneos', '--vorbis'], work, logs / 'dependencies.log', env)
    shutil.copytree(deps / 'include', gen / 'deps/include', dirs_exist_ok=True)
    for lib in LIBS:
        clone(deps / 'lib' / lib, gen / 'deps/lib' / lib)

    # The home screen icon: the game's own, which the Mac build took from its control data (ICONS).
    icon, icons = native / 'build/AppIcon.icns', gen / 'icons'
    shutil.rmtree(icons, ignore_errors=True); icons.mkdir()
    if icon.is_file():
        for name, px in ICONS:
            run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), icon, '--out', icons / name], work, logs / 'icon.log', env)
    shutil.copy2(GAME / 'ios/Sources/Info.plist', gen / 'Info.plist')   # xcodegen writes the properties into it
    acnh_pane(gen / 'Settings.bundle')
    frameworks = ' '.join('-framework ' + x for x in [
        'UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics', 'CoreText',
        'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox', 'GameController', 'CoreHaptics', 'CoreMotion',
        'AVFAudio', 'VideoToolbox', 'CoreMedia', 'CoreVideo', 'Security'])
    xcconfig = gen / 'acnh.xcconfig'
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} ' +
                        ' '.join(str(gen / 'deps/lib' / lib) for lib in LIBS) + f' -lc++ -lz {flags}\n')
    (ios / 'project.yml').write_text(project(work, gen, xcconfig, version))
    run(['xcodegen', 'generate', '--spec', 'project.yml'], ios, logs / 'xcodegen.log', env)
    # The target with the iphoneos SDK and no -destination: a destination asks for Xcode's iOS platform component
    # (Xcode › Settings › Components), which a build with the SDK alone does not need.
    derived = ios / 'derived'
    run(['xcodebuild', '-project', 'ACNHIOS.xcodeproj', '-target', 'ACNHIOS', '-configuration', 'Release', '-sdk', 'iphoneos',
         'ARCHS=arm64', f'SYMROOT={derived / "Build/Products"}', f'OBJROOT={derived / "obj"}',
         'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY='], ios, logs / 'xcodebuild.log', env)
    app = derived / f'Build/Products/Release-iphoneos/{APP}.app'
    if not app.is_dir():
        raise RuntimeError(f'Xcode did not produce {app}')
    # The increased-memory-limit entitlement in the ad-hoc signature, so re-signing tools keep it.
    run(['codesign', '--force', '--sign', '-', '--entitlements', GAME / 'ios/Sources/ACNH.entitlements', app / APP],
        ios, logs / 'entitlements.log', env)

    # The app with its game data: a copy of Xcode's product, so the product stays small.
    payload = ios / 'payload'
    dest = out if out.suffix == '.app' else payload / f'Payload/{APP}.app'
    shutil.rmtree(dest if out.suffix == '.app' else payload / 'Payload', ignore_errors=True)   # an app of an earlier name too
    shutil.copytree(app, dest, symlinks=True)
    for f in sorted((gen / 'icons').iterdir()):
        shutil.copy2(f, dest / f.name)
    res = dest / 'GameData'
    # --image-outside (Zelda's build_ipa.py option of the same name): romfs.bin stays out of the app; install_device.py
    # --game-data copies it once into Documents/GameData, so an update carries only the program and shaders
    for name in (('runtime-index.bin', 'index.json') if a.image_outside else ('romfs.bin', 'runtime-index.bin', 'index.json')):
        if (romfs / name).is_file():
            clone(romfs / name, res / 'romfs' / name)
    for f in sorted((work / 'fonts').iterdir()):
        if f.is_file():
            clone(f, res / 'shared-fonts' / f.name)
    dlc = work / 'dlc'
    if (dlc / 'manifest.json').is_file():
        for f in sorted(dlc.iterdir()):
            if f.is_file():
                clone(f, res / 'dlc' / f.name)
    if alt:   # the 60 FPS mod's files (its checked manifest, the data words and the RomFS overlay; frame_rate.c)
        for f in sorted(fps60.rglob('*')):
            if f.is_file():
                clone(f, res / 'fps60' / f.relative_to(fps60))
    seed = native / 'build/shader-seed'
    if not (seed / 'seed-manifest.json').is_file():
        raise RuntimeError(f'no prepared shader seed in {seed}: run games/acnh/build.py first')
    for f in sorted(seed.iterdir()):
        if f.is_file():
            clone(f, res / 'shader-seed' / f.name)
    print(f'shaders: {json.loads((seed / "seed-manifest.json").read_text())["shaders"]} prepared variants')
    cache = (a.mac_shader_cache.expanduser() if a.mac_shader_cache else default_mac_cache(version))
    shaders = sorted(cache.iterdir()) if cache.is_dir() else []
    for f in shaders:
        if f.is_file() and f.suffix in ('.metal', '.json', '.variants'):
            clone(f, res / 'shader-cache' / f.name)
    print(f'shaders: {sum(1 for f in shaders if f.name.endswith(".variants"))} more from the Mac\'s cache ({cache})')
    if out.suffix == '.app':
        print(f'app ready: {out}; unsigned, install it with games/totk/ios/tools/install_device.py')
        return
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs / 'ipa-zip.log', env)
    print(f'IPA ready: {out} ({out.stat().st_size / 2**30:.1f} GiB); unsigned, install it with games/totk/ios/tools/install_device.py' + (f' --game-data {romfs / "romfs.bin"}' if a.image_outside else ''))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
