#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone / iPad app of Metroid Dread 1.0.0 from a prepared work folder (games/Dread_build:
the verified rewrite in rewritten/v100, the Mac runtime build/dread-native with its generated tables, the RomFS in
extracted/v100/romfs, the prepared shader seed in build/shader-seed and the shaders the Mac translated in
build/shader-cache). Sign and install it with install_device.py.

  python3.14 games/dread/ios/tools/build_ipa.py --work games/Dread_build --out "games/Metroid Dread.ipa"

iOS cannot execute code written at run time, so the rewritten modules are compiled into the app (gen_embed.py; the loader
maps them back, runtime/loader.c), and it cannot translate shaders, so the app carries them translated: every shader of
the game for its default bind state plus the states assets/shader_list.tsv names (scripts/prepare_shader_seed.py), and
what the Mac translated while playing (its pipelines too). A variant neither has is skipped on the device."""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from xcode_dir import developer_dir

GAME = Path(__file__).resolve().parents[2]   # games/dread
VERSION = 'v100'
APP = 'Dread'   # the product name, Dread.app (install_device.py)

def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-35:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))

def clone(src, dst):
    """An APFS clone (cp -c): the 4.4 GB RomFS image takes no extra space and no time."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode: shutil.copy2(src, dst)

def project(work, gen, flags_file):
    """The Xcode project spec, with absolute paths: the sources are games/dread's, everything generated is the work folder's."""
    rt, nvn = GAME/'runtime', GAME/'third_party/d2r_gfx/gfx'
    return f'''name: DreadIOS
options:
  bundleIdPrefix: org.macnativegames
  deploymentTarget:
    iOS: "26.0"
configFiles:
  Release: {flags_file}
  Debug: {flags_file}
targets:
  DreadIOS:
    type: application
    platform: iOS
    sources:
      - path: {GAME/'ios/Sources'}
        excludes:
          - Dread.entitlements
          - Info.plist
      - path: {rt}
        excludes:
          - frontend.m
          - loader.c
      - path: {nvn/'nvn'}
      - path: {nvn/'tex'}
        excludes:
          - nvn_bctc.metal   # the runtime compiles its copy from source (nvn_mtl.m k_bctc_msl)
      - path: {gen/'embed.S'}
      - path: {rt/'loader.c'}
        compilerFlags: "-Dmain=dread_host_main"
{"      - path: " + str(gen/'Assets.xcassets') if (gen/'Assets.xcassets').is_dir() else ""}
    info:
      path: {gen/'Info.plist'}
      properties:
        CFBundleDisplayName: Metroid Dread
        CFBundleName: Metroid Dread
        CFBundleIdentifier: $(PRODUCT_BUNDLE_IDENTIFIER)
        CFBundleShortVersionString: "1.0.0"
        CFBundleVersion: "1"
        UILaunchScreen: {{}}
        UIFileSharingEnabled: true
        LSSupportsOpeningDocumentsInPlace: true
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
                UISceneDelegateClassName: DreadSceneDelegate
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: org.macnativegames.dread
      PRODUCT_NAME: {APP}
      CODE_SIGN_ENTITLEMENTS: {GAME/'ios/Sources/Dread.entitlements'}
      TARGETED_DEVICE_FAMILY: "1,2"
      IPHONEOS_DEPLOYMENT_TARGET: "26.0"
      CODE_SIGN_STYLE: Manual
      CODE_SIGNING_ALLOWED: NO
      ENABLE_BITCODE: NO
      GCC_C_LANGUAGE_STANDARD: gnu11
      CLANG_ENABLE_OBJC_ARC: YES
      GCC_WARN_INHIBIT_ALL_WARNINGS: YES
      {"ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon" if (gen/'Assets.xcassets').is_dir() else ""}
      HEADER_SEARCH_PATHS:
        - $(inherited)
        - {nvn/'nvn'}
        - {nvn/'nvn/include'}
        - {gen}
        - {gen/'deps/include'}
      OTHER_CFLAGS: $(inherited) -mcpu=apple-a15 -I{rt} -I{nvn/'nvn'} -I{nvn/'nvn/include'} -I{gen} -I{work/'build'}
'''

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--work', type=Path, default=GAME.parent/'Dread_build')
    p.add_argument('--out', type=Path, required=True, help='.ipa, or .app for the unsigned folder (install_device.py takes both)')
    p.add_argument('--simulator', action='store_true', help='an .app for the iOS Simulator on this Mac (no signing; no memory limit, and\n'
                   'its Metal is not the device\'s): xcrun simctl install booted <out>')
    a = p.parse_args(); work = a.work.resolve(); out = a.out.resolve()
    if out.suffix.lower() not in ('.ipa', '.app'): raise RuntimeError('the output must end in .ipa or .app')
    sim = a.simulator; sdk = 'iphonesimulator' if sim else 'iphoneos'
    if sim and out.suffix != '.app': raise RuntimeError('a Simulator build is an .app folder')
    ios = work/('build/ios-sim' if sim else 'build/ios'); gen = ios/'Gen'; logs = work/('logs/ios-sim' if sim else 'logs/ios'); gen.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    subprocess.run(['xcodebuild', '-version'], env=env, check=True, stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'): raise RuntimeError('install xcodegen: brew install xcodegen')
    host, rewritten, romfs = work/'build/dread-native', work/f'rewritten/{VERSION}', work/f'extracted/{VERSION}/romfs'
    for need in (host, romfs/'romfs.bin', work/'build/runtime_tables.h'):
        if not need.exists(): raise RuntimeError(f'missing {need}: build the Mac app first (games/dread/build.py, or make prepare runtime)')

    # The rewritten modules as the game patches leave them (loader.c export-ios), compiled into the executable's segments.
    modules = gen/'modules'; modules.mkdir(exist_ok=True)
    run([host, rewritten, 'export-ios'], work, logs/'export-ios.log', dict(env, TOTK_EXPORT_IOS=str(modules)))
    flags = subprocess.check_output([sys.executable, GAME/'ios/tools/gen_embed.py', rewritten, gen/'embed.S', '--text-dir', modules,
                                     'main', 'subsdk0', 'sdk'], text=True, env=env).strip()

    # Opus and libvpx for the device: Homebrew's Mac archives cannot be linked into an iOS app.
    deps = work/('build/ios-sim-deps' if sim else 'build/ios-deps')
    if not ((deps/'lib/libopus.a').is_file() and (deps/'lib/libvpx.a').is_file()):
        run([sys.executable, GAME/'ios/tools/build_deps.py', deps, sdk], work, logs/'dependencies.log', env)
    shutil.copytree(deps/'include', gen/'deps/include', dirs_exist_ok=True)
    for lib in ('libopus.a', 'libvpx.a'): clone(deps/'lib'/lib, gen/'deps/lib'/lib)

    # The home screen icon: the game's own, which the Mac build took from its control data (build/AppIcon.icns).
    icon, catalog = work/'build/AppIcon.icns', gen/'Assets.xcassets'; shutil.rmtree(catalog, ignore_errors=True)
    if icon.is_file():
        appicon = catalog/'AppIcon.appiconset'; appicon.mkdir(parents=True)
        (catalog/'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))
        run(['sips', '-s', 'format', 'png', '-z', '1024', '1024', icon, '--out', appicon/'icon-1024.png'], work, logs/'icon.log', env)
        (appicon/'Contents.json').write_text(json.dumps({'images': [{'filename': 'icon-1024.png', 'idiom': 'universal', 'platform': 'ios',
                                                                     'size': '1024x1024'}], 'info': {'author': 'xcode', 'version': 1}}))
    shutil.copy2(GAME/'ios/Sources/Info.plist', gen/'Info.plist')   # xcodegen writes the properties into it
    frameworks = ' '.join('-framework ' + x for x in ['UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics',
        'CoreText', 'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox', 'GameController', 'CoreHaptics', 'CoreMotion',
        'AVFAudio', 'VideoToolbox', 'CoreMedia', 'CoreVideo', 'Security'])
    xcconfig = gen/'dread.xcconfig'
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} {gen}/deps/lib/libopus.a {gen}/deps/lib/libvpx.a {flags}\n')
    (ios/'project.yml').write_text(project(work, gen, xcconfig))
    run(['xcodegen', 'generate', '--spec', 'project.yml'], ios, logs/'xcodegen.log', env)
    derived = work/('build/ios-sim-derived' if sim else 'build/ios-derived')
    run(['xcodebuild', '-project', 'DreadIOS.xcodeproj', '-scheme', 'DreadIOS', '-configuration', 'Release', '-destination', 'generic/platform=iOS' + (' Simulator' if sim else ''),
         '-derivedDataPath', derived, 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY=',
         *(['ARCHS=arm64', 'ONLY_ACTIVE_ARCH=NO'] if sim else [])], ios, logs/'xcodebuild.log', env)   # the game code is arm64 only
    app = derived/f'Build/Products/Release-{sdk}/{APP}.app'
    if not app.is_dir(): raise RuntimeError(f'Xcode did not produce {app}')
    # The increased-memory-limit entitlement in the ad-hoc signature, so re-signing tools keep it.
    run(['codesign', '--force', '--sign', '-', '--entitlements', GAME/'ios/Sources/Dread.entitlements', app/APP], ios, logs/'entitlements.log', env)

    # The app with its game data: a copy of Xcode's product, so the product stays small.
    payload = work/'build/ios-payload'
    dest = out if out.suffix == '.app' else payload/f'Payload/{APP}.app'
    shutil.rmtree(dest, ignore_errors=True); shutil.copytree(app, dest, symlinks=True)
    res = dest/'GameData'
    for name in ('romfs.bin', 'runtime-index.bin', 'index.json'):
        if (romfs/name).is_file(): clone(romfs/name, res/'romfs'/name)
    seed = work/'build/shader-seed'
    if not (seed/'seed-manifest.json').is_file(): raise RuntimeError(f'no prepared shader seed in {seed}: run scripts/prepare_shader_seed.py (build.py does)')
    for f in sorted(seed.iterdir()):
        if f.is_file(): clone(f, res/'shader-seed'/f.name)
    print(f'shaders: {json.loads((seed/"seed-manifest.json").read_text())["shaders"]} prepared variants')
    cache = work/'build/shader-cache/rt-totk-native-v4-unverified'   # gfx/nvn/dmsh_chain.h DMSH_CHAIN
    shaders = sorted(cache.iterdir()) if cache.is_dir() else []
    for f in shaders:
        if f.is_file(): clone(f, res/'shader-cache'/f.name)
    print(f'shaders: {sum(1 for f in shaders if f.name.endswith(".variants"))} more from the Mac\'s cache')
    if out.suffix == '.app': print(f'app ready: {out}; unsigned, install it with games/dread/ios/tools/install_device.py'); return
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs/'ipa-zip.log', env)
    print(f'IPA ready: {out} ({out.stat().st_size/2**30:.1f} GiB); unsigned, install it with games/dread/ios/tools/install_device.py')

if __name__ == '__main__':
    try: main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
