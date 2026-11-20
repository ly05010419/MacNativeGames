#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone / iPad app of Animal Crossing: New Horizons from a Mac build's work folder
(games/acnh/build.py: prepared/ the verified game data, native/ the Mac host with its generated tables and shader seed,
fonts/ and dlc/). Sign and install it with install_device.py. Forked from games/dread/ios/tools/build_ipa.py.

  python3 games/acnh/ios/tools/build_ipa.py --work <the Mac build's work folder> --out "<folder>/Animal Crossing.ipa"

iOS cannot execute code written at run time, so the rewritten modules are compiled into the app (gen_embed.py; the loader
maps them back, runtime/loader.c), with the game patches applied on the Mac (loader.c export-ios). The frame-rate mod
patches code when the game starts, so the iPhone plays at the original 30 FPS. The device cannot translate shaders
either: the app carries the prepared seed (native/build/shader-seed) and what this Mac translated while playing
(--mac-shader-cache, the Mac app's shader cache); a variant neither has is skipped on the device."""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from xcode_dir import developer_dir

GAME = Path(__file__).resolve().parents[2]   # games/acnh
REPO = GAME.parents[1]
APP = 'ACNH'   # the product name, ACNH.app (install_device.py)
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
      - path: {GAME / 'ios/Sources'}
        excludes:
          - ACNH.entitlements
          - Info.plist
      - path: {GAME / 'ios/Settings.bundle'}   # the Settings app pane: the active players switch (mng_active.h)
      - path: {rt}
        excludes:{excludes}
      - path: {gfx / 'nvn'}
        excludes:{not_sources}
      - path: {gfx / 'tex'}
        excludes:{not_sources}
          - nvn_bctc.metal   # the runtime compiles its copy from source (nvn_mtl.m k_bctc_msl)
      - path: {gen / 'embed.S'}
      - path: {rt / 'loader.c'}
        compilerFlags: "-Dmain=acnh_host_main"
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
        GCSupportedGameControllers:
          - ProfileName: ExtendedGamepad
        CADisableMinimumFrameDurationOnPhone: true
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default
                UISceneDelegateClassName: ACNHSceneDelegate
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
      OTHER_CFLAGS: $(inherited) -DGAME_ACNH=1 -include {build / 'version-config.h'} -mcpu=apple-a15 -I{rt} -I{gfx / 'nvn'} -I{gfx / 'nvn/include'} -I{gen} -I{build}
'''


def default_mac_cache(version):
    return Path.home() / f'Library/Application Support/MacNativeGames/ACNH-{version}/shader-cache'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True, help='the Mac build\'s work folder (games/acnh/build.py --work)')
    p.add_argument('--out', type=Path, required=True, help='.ipa, or .app for the unsigned folder (install_device.py takes both)')
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
    # segments. No frame-rate assets: the export carries the original 30 FPS code, as the iPhone plays it.
    modules = gen / 'modules'; shutil.rmtree(modules, ignore_errors=True); modules.mkdir()
    export_env = {k: v for k, v in env.items() if k not in ('ACNH_FPS_ASSETS', 'ACNH_FPS120_ASSETS')}
    export_env.update(TOTK_EXPORT_IOS=str(modules), ACNH_FPS='30')
    run([host, native / 'rewritten', 'export-ios'], work, logs / 'export-ios.log', export_env)
    flags = subprocess.check_output([sys.executable, GAME / 'ios/tools/gen_embed.py', staged, gen / 'embed.S',
                                     '--text-dir', modules, *names], text=True, env=env).strip()

    # Opus, libvpx, libvorbis and libogg for the device: the Mac archives (tools/deps) cannot be linked into an iOS app.
    deps = work / 'ios-deps'
    if not all((deps / 'lib' / lib).is_file() for lib in LIBS):
        run([sys.executable, GAME / 'ios/tools/build_deps.py', deps, 'iphoneos'], work, logs / 'dependencies.log', env)
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
    shutil.rmtree(dest, ignore_errors=True); shutil.copytree(app, dest, symlinks=True)
    for f in sorted((gen / 'icons').iterdir()):
        shutil.copy2(f, dest / f.name)
    res = dest / 'GameData'
    for name in ('romfs.bin', 'runtime-index.bin', 'index.json'):
        if (romfs / name).is_file():
            clone(romfs / name, res / 'romfs' / name)
    for f in sorted((work / 'fonts').iterdir()):
        if f.is_file():
            clone(f, res / 'fonts' / f.name)
    dlc = work / 'dlc'
    if (dlc / 'manifest.json').is_file():
        for f in sorted(dlc.iterdir()):
            if f.is_file():
                clone(f, res / 'dlc' / f.name)
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
        print(f'app ready: {out}; unsigned, install it with games/acnh/ios/tools/install_device.py')
        return
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs / 'ipa-zip.log', env)
    print(f'IPA ready: {out} ({out.stat().st_size / 2**30:.1f} GiB); unsigned, install it with games/acnh/ios/tools/install_device.py')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
