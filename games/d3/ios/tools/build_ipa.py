#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone/iPad IPA of Diablo III from a prepared work folder (games/d3/Makefile prepare
runtime: <work>/build/d3-native, its generated headers and rewritten/v1441792).

Everything generated goes to <work>/ios; the sources stay in the repository (games/d3). The 17 GB RomFS image is either
carried in the app (GameData/romfs/romfs.bin) or, with --image-outside, left out: install_device.py --game-data copies it
once into the app's Documents/GameData. The shader seed is made from a Mac runtime shader cache (tools/make_seed.py).
"""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compile_seed import METAL_FLAGS
from xcode_dir import developer_dir

SRC = Path(__file__).resolve().parents[2]   # games/d3
VERSION = 'v1441792'
MODULES = ('main', 'sdk')
FRAMEWORKS = ['UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics', 'CoreText',
              'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox', 'GameController', 'CoreHaptics', 'CoreMotion',
              'IOKit', 'AVFAudio', 'Security']

def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-40:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))

def clone(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True); dst.unlink(missing_ok=True)
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode: shutil.copy2(src, dst)

def copy_tree(src, dst):
    src, dst = Path(src), Path(dst)
    if dst.exists(): shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['/bin/cp', '-cR', str(src), str(dst)], capture_output=True).returncode: shutil.copytree(src, dst)

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True, help='the D3 work folder (D3_build)')
    p.add_argument('--out', type=Path, required=True, help='.ipa, or .app for the unsigned folder (install_device.py)')
    p.add_argument('--shader-cache', type=Path, required=True, help='a Mac runtime shader cache (rt-<chain>); only read')
    p.add_argument('--image-outside', action='store_true', help='leave romfs.bin out: the device keeps it in Documents/GameData')
    p.add_argument('--app-name', default='Diablo III'); p.add_argument('--bundle-id', default='org.macnativegames.d3')
    a = p.parse_args()
    work = a.work.resolve(); out = a.out.resolve(); ios = work / 'ios'; gen = ios / 'Gen'; logs = work / 'logs/ios'
    if out.suffix.lower() not in ('.ipa', '.app'): raise RuntimeError('iOS output path must end in .ipa (or .app)')
    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    subprocess.run(['xcodebuild', '-version'], env=env, check=True, stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'):
        sys.path.insert(0, str(SRC.parents[1]))
        from tools.deps import program
        xcodegen = str(program('xcodegen'))
    else:
        xcodegen = shutil.which('xcodegen')
    host = work / 'build/d3-native'; rewritten = work / f'rewritten/{VERSION}'
    for need in [host, *(work / 'build' / n for n in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h')),
                 *(rewritten / f'{m}.patched.bin' for m in MODULES)]:
        if not need.is_file(): raise RuntimeError(f'missing {need}: run make -C games/d3 WORK=<work> prepare runtime first')
    gen.mkdir(parents=True, exist_ok=True)
    for name in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h'): clone(work / 'build' / name, gen / name)

    # The final code after the game patches, embedded in executable segments (iOS cannot execute code written at run time).
    modules = gen / 'modules'; modules.mkdir(exist_ok=True)
    e = {k: v for k, v in env.items() if not k.startswith(('TOTK_', 'BY3_', 'DM_'))}
    e['TOTK_EXPORT_IOS'] = str(modules)
    run([host, rewritten, 'export-ios'], work, logs / 'export-ios.log', e)
    flags = subprocess.check_output([sys.executable, SRC / 'ios/tools/gen_embed.py', rewritten, gen / 'embed.S',
                                     '--text-dir', modules, *MODULES], text=True, env=env).strip()

    project = (SRC / 'ios/project.yml').read_text().replace('@SRC@', str(SRC))
    icon = work / 'build/AppIcon.icns'; catalog = gen / 'Assets.xcassets'; shutil.rmtree(catalog, ignore_errors=True)
    if icon.is_file():   # the game's own icon (scripts/extract_app_icon.py, made for the Mac app)
        appicon = catalog / 'AppIcon.appiconset'; appicon.mkdir(parents=True)
        (catalog / 'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))
        run(['sips', '-s', 'format', 'png', '-z', '1024', '1024', icon, '--out', appicon / 'icon-1024.png'], work, logs / 'icon.log', env)
        (appicon / 'Contents.json').write_text(json.dumps({'images': [{'filename': 'icon-1024.png', 'idiom': 'universal',
            'platform': 'ios', 'size': '1024x1024'}], 'info': {'author': 'xcode', 'version': 1}}))
        project = project.replace('      - path: Gen/embed.S\n', '      - path: Gen/embed.S\n      - path: Gen/Assets.xcassets\n')
        project = project.replace('      CODE_SIGN_STYLE: Manual\n', '      CODE_SIGN_STYLE: Manual\n      ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon\n')
    (ios / 'project.generated.yml').write_text(project)
    if not (gen / 'Info.plist').is_file():
        (gen / 'Info.plist').write_text('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                                        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict/></plist>\n')

    # Device builds of the audio and VP9 libraries: Homebrew's Mac archives cannot be linked into an iOS app.
    deps = work / 'build/ios-deps'
    if not all((deps / 'lib' / lib).is_file() for lib in ('libopus.a', 'libvpx.a')):
        run([sys.executable, SRC / 'ios/tools/build_deps.py', deps], work, logs / 'dependencies.log', env)
    shutil.copytree(deps / 'include', gen / 'deps/include', dirs_exist_ok=True)
    for lib in ('libopus.a', 'libvpx.a'): clone(deps / 'lib' / lib, gen / 'deps/lib' / lib)

    # The shaders: the Mac cache's translations, compiled for the device (compile_seed.py keeps libraries by source hash).
    seed = work / f'build/shader-seed-ios/{VERSION}'
    run([sys.executable, SRC / 'ios/tools/make_seed.py', a.shader_cache.expanduser().resolve(), seed], work, logs / 'seed.log', env)
    # Without Xcode's Metal toolchain (xcodebuild -downloadComponent MetalToolchain) the device compiles the MSL itself the
    # first time each shader is used (nvn_mtl.m shader_load), slower on the first start but the same shaders.
    if subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', '--version'], env=env, capture_output=True).returncode == 0:
        run([sys.executable, SRC / 'ios/tools/compile_seed.py', seed, 'iphoneos'], work, logs / 'shaders.log', env)
    else:
        print('note: no Metal toolchain (xcodebuild -downloadComponent MetalToolchain): the device compiles the shaders on first use', flush=True)
    manifest = json.loads((seed / 'seed-manifest.json').read_text())
    (seed / 'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')   # read in place (DM_NVN_SHMAP); not a seed file
    names = sorted(n for n in os.listdir(seed) if n not in ('seed-manifest.json', 'shader_map.tsv', 'pso_list.tsv') and (seed / n).is_file())
    known = manifest['files']
    manifest['files'] = {n: known.get(n) or hashlib.sha256((seed / n).read_bytes()).hexdigest() for n in names}
    (seed / 'seed-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    env.update(D3_APP_NAME=a.app_name, D3_BUNDLE_ID=a.bundle_id)
    libs = ' '.join(str(gen / 'deps/lib' / lib) for lib in ('libopus.a', 'libvpx.a'))
    (gen / 'd3.xcconfig').write_text(f"OTHER_LDFLAGS = $(inherited) {' '.join('-framework ' + x for x in FRAMEWORKS)} {libs} {flags}\n")
    run([xcodegen, 'generate', '--spec', 'project.generated.yml'], ios, logs / 'xcodegen.log', env)
    derived = work / 'build/ios-derived'
    run(['xcodebuild', '-project', 'D3IOS.xcodeproj', '-scheme', 'D3IOS', '-configuration', 'Release', '-destination',
         'generic/platform=iOS', '-derivedDataPath', derived, 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO',
         'CODE_SIGN_IDENTITY='], ios, logs / 'xcodebuild.log', env)
    app = derived / 'Build/Products/Release-iphoneos/D3.app'
    if not app.is_dir(): raise RuntimeError(f'Xcode did not produce {app}')
    # The increased-memory-limit entitlement in the ad-hoc signature, so re-signing tools keep it.
    run(['codesign', '--force', '--sign', '-', '--entitlements', SRC / 'ios/Sources/D3.entitlements', app / 'D3'], ios, logs / 'entitlements.log', env)

    res = app / 'GameData'; shutil.rmtree(res, ignore_errors=True); res.mkdir()
    romfs = work / f'extracted/{VERSION}/romfs'; (res / 'romfs').mkdir()
    for name in ('runtime-index.bin', 'index.json'): clone(romfs / name, res / 'romfs' / name)
    if not a.image_outside: clone(romfs / 'romfs.bin', res / 'romfs/romfs.bin')   # a link to the Mac app's copy: cloned
    aoc = work / f'extracted/{VERSION}/aoc'
    if aoc.is_dir(): copy_tree(aoc, res / 'aoc')
    copy_tree(work / 'extracted/firmware/shared-fonts', res / 'shared-fonts')
    copy_tree(seed, res / 'shader-seed')

    payload = work / 'build/ios-payload'; shutil.rmtree(payload, ignore_errors=True); (payload / 'Payload').mkdir(parents=True)
    copy_tree(app, payload / 'Payload/D3.app')
    if out.suffix == '.app':
        copy_tree(payload / 'Payload/D3.app', out)
        print(f'app ready: {out}; unsigned, install it with games/d3/ios/tools/install_device.py'); return
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs / 'ipa-zip.log', env)
    print(f'IPA ready: {out} ({out.stat().st_size / 2**30:.1f} GiB); unsigned, sideload with your signing tool')

if __name__ == '__main__':
    try: main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error: print(f'ERROR: {error}', file=sys.stderr); sys.exit(1)
