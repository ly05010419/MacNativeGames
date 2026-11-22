#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone/iPad IPA from a prepared work tree: Tears of the Kingdom (v0, v121, v143)
or Breath of the Wild (botw160, botw190; the runtime and graphics layer built with GAME_BOTW, game_patches_botw.c, its own
shader list and the add-on content beside the RomFS)."""
import argparse, json, os, plistlib, shutil, struct, subprocess, sys, time, zlib
from pathlib import Path
from compile_seed import METAL_FLAGS, seed_flags
from device_pipelines import device_gpus, make as make_device_pipelines, seed_check
from xcode_dir import developer_dir
import settings_strings

def pane_for(root, app):
    """The pane for one app, in place: app is 'totk', 'botw' or 'ns2' (Bananza, Pokopia). A row Root.plist marks with
    TOTKApp belongs to that app only, 'zelda' to Tears of the Kingdom, Breath of the Wild and Animal Crossing (the picture
    profile and MetalFX, user 2026-11-17: the picture is chosen in the game; Animal Crossing's app is built from these
    sources too, games/acnh/ios/tools/build_ipa.py, as Super Mario Odyssey's, Mario Kart 8 Deluxe's, Red Dead Redemption's and Pokémon Legends: Arceus's, user 2026-11-20 "塞尔达的 设置 都移植过来 要统一"); an unmarked
    row is everyone's. Any
    group the dropped rows leave empty goes too. tests/test_ios_packaging.py PicturePane checks the three panes."""
    def mine(s):
        mark=s.get('TOTKApp')
        return mark is None or mark==app or (mark=='zelda' and app in ('totk','botw','acnh','odyssey','pla','mk8dx','rdr'))
    specs=[s for s in root['PreferenceSpecifiers'] if mine(s)]
    kept=[]
    for i,s in enumerate(specs):
        if s.get('Type')=='PSGroupSpecifier':
            nxt=specs[i+1] if i+1<len(specs) else None
            if nxt is None or nxt.get('Type')=='PSGroupSpecifier': continue
        kept.append(s)
    root['PreferenceSpecifiers']=kept
    return root

def seed_fingerprint(seed, lists):
    """What decides whether the app's seed is copied again (and pushed again by install_device.py): the manifest, the
    lists, the variant libraries by name, and every pipelines-* file by name (the name carries the GPU) and content. The
    pipelines' index (.tsv) is the same for every GPU, so with it alone an iPhone build kept the iPad's pipelines from the
    build before and the iPhone filled its own archive on its first start (2026-11-01)."""
    import hashlib
    fp=hashlib.sha256(b''.join((d/n).read_bytes() for d,n in [(seed,'seed-manifest.json'),(seed,'shader_map.tsv'),
                                (lists,'pso_list.tsv'),(lists,'pso_title.tsv')])
                      +''.join(sorted(p.name for p in seed.glob('variant-*'))).encode())
    for p in sorted(seed.glob('pipelines-*')): fp.update(p.name.encode()+b'\0'+hashlib.sha256(p.read_bytes()).digest())
    return fp.hexdigest()

def botw_pane(root):
    """Breath of the Wild's pane: the picture profile and MetalFX, as Tears of the Kingdom's (the picture is chosen in the
    game, user 2026-11-17)."""
    return pane_for(root,'botw')

def bananza_pane(root):
    """Bananza's own engine size; TOTK/BotW's docked presets describe other engines."""
    root=pane_for(root,'ns2')
    specs=root['PreferenceSpecifiers']
    at=next(i for i,s in enumerate(specs) if s.get('Key')=='TOTK_DOCKED')
    specs[at-1]['FooterText']=('輸入寬x高，例如 1280x720（720p）、960x540 或 1920x1080。'
        '改變遊戲渲染尺寸，保留原始貼圖和完整 mip。重新啟動遊戲後生效 · '
        'Enter widthxheight, such as 1280x720 (720p), 960x540 or 1920x1080. '
        'Changes the game render size; original textures and full mip chains are preserved. Relaunch to apply.')
    specs[at]={'Type':'PSTextFieldSpecifier','Key':'TOTK_OUTPUT_RES','Title':'渲染分辨率 · Render resolution',
        'DefaultValue':'960x540','AutocapitalizationType':'None','AutocorrectionType':'No','KeyboardType':'ASCIICapable'}
    return root

# The icon at one size, drawn with the system's AppKit through osascript (as MNG Launcher's tools/builder_app/
# resize_icon.js; the work tree carries only this game's folder). sips writes through the user's temporary folder on the
# system disk, which this Mac's build sandbox refuses: 2026-11-15 the iPad package stopped at "sips … Cannot write to
# file /var/folders/…/T/…". tests/test_ios_icon_no_sips.py.
RESIZE_ICON_JS = r"""ObjC.import('AppKit');
function run(argv) {
  var image = $.NSImage.alloc.initWithContentsOfFile(argv[0]);
  if (image.isNil()) throw new Error('cannot read ' + argv[0]);
  var n = parseInt(argv[1], 10);
  var rep = $.NSBitmapImageRep.alloc.initWithBitmapDataPlanesPixelsWidePixelsHighBitsPerSampleSamplesPerPixelHasAlphaIsPlanarColorSpaceNameBytesPerRowBitsPerPixel(
    null, n, n, 8, 4, true, false, $.NSDeviceRGBColorSpace, 0, 0);
  rep.size = $.NSMakeSize(n, n);
  var context = $.NSGraphicsContext.graphicsContextWithBitmapImageRep(rep);
  $.NSGraphicsContext.saveGraphicsState;
  $.NSGraphicsContext.setCurrentContext(context);
  context.setImageInterpolation($.NSImageInterpolationHigh);
  image.drawInRectFromRectOperationFraction($.NSMakeRect(0, 0, n, n), $.NSZeroRect, $.NSCompositingOperationCopy, 1.0);
  context.flushGraphics;
  $.NSGraphicsContext.restoreGraphicsState;
  var data = rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $());
  if (!data.writeToFileAtomically(argv[2], false)) throw new Error('cannot write ' + argv[2]);
  return '';
}
"""

def png_icon(source, pixels, out, log, env):
    script=Path(log).with_suffix('.js'); script.parent.mkdir(parents=True,exist_ok=True); script.write_text(RESIZE_ICON_JS)
    run(['/usr/bin/osascript','-l','JavaScript',script,source,str(pixels),out],Path(out).parent,Path(log),env)

def run(args, cwd, log, env):
    log.parent.mkdir(parents=True,exist_ok=True)
    with log.open('w') as f:
        p=subprocess.run([str(x) for x in args],cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT)
    if p.returncode:
        tail=log.read_text(errors='replace').splitlines()[-35:]
        raise RuntimeError(f"{args[0]} failed; see {log}\n"+'\n'.join(tail))

def clone(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    if subprocess.run(['cp','-c',str(src),str(dst)],capture_output=True).returncode: shutil.copy2(src,dst)

def clear_png(path,size):
    """A fully transparent RGBA PNG (a visionOS icon layer), written without an image library."""
    chunk=lambda kind,data: struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=b''.join(b'\0'+bytes(size*4) for _ in range(size))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',size,size,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b''))
def clone_folder(src,dst):
    """One APFS clone of a whole folder (macOS clonefile), as build_app.py copy_tree: the seed is ~100,000 files, which
    cp -cR clones one by one (minutes per copy, 2026-10-22)."""
    if sys.platform!='darwin': return False
    import ctypes
    libc=ctypes.CDLL(None);libc.clonefile.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint32]
    dst.parent.mkdir(parents=True,exist_ok=True)
    return libc.clonefile(os.fsencode(src),os.fsencode(dst),0)==0

def discard(path,beside=None):
    """Out of the way at once (a rename), deleted in the background: tens of thousands of files take minutes to delete on
    an external disk, and the build does not need to wait for it. `beside`: the folder to rename it into, when its own
    folder is copied or zipped next (a folder inside the app, the app inside Payload)."""
    if not path.exists(): return
    old=(beside or path.parent)/f'.{path.name}.old-{os.getpid()}-{time.time_ns()}'
    os.rename(path,old); subprocess.Popen(['/bin/rm','-rf',str(old)],start_new_session=True)

def copy_tree(src,dst):
    src=Path(src);dst=Path(dst)
    if dst.exists(): shutil.rmtree(dst)
    if clone_folder(src,dst): return
    dst.mkdir(parents=True,exist_ok=True)
    if sys.platform=='darwin':
        result=subprocess.run(['/bin/cp','-cR',str(src)+os.sep+'.',str(dst)],capture_output=True)
        if result.returncode==0: return
        shutil.rmtree(dst);dst.mkdir(parents=True,exist_ok=True)
    for p in src.rglob('*'):
        if p.is_file(): clone(p,dst/p.relative_to(src))

def configure_app_icon(tree, gen, project, logs, env, vision=False):
    # The home screen icon: the game's own, which the build took from its control data for the Mac app
    # (build/AppIcon.icns, scripts/extract_app_icon.py), as a single-size asset catalog. Without it the app had none.
    icon=tree/'build/AppIcon.icns'; catalog=gen/'Assets.xcassets'; shutil.rmtree(catalog,ignore_errors=True)
    if icon.is_file():
        info={'author':'xcode','version':1}
        if vision:   # a layered image stack instead of an icon set; actool wants at least two layers, so a clear one in front
            stack=catalog/'AppIcon.solidimagestack'
            for layer,name in (('Front','clear-1024.png'),('Back','icon-1024.png')):
                images=stack/f'{layer}.solidimagestacklayer/Content.imageset'; images.mkdir(parents=True)
                (images.parent/'Contents.json').write_text(json.dumps({'info':info}))
                (images/'Contents.json').write_text(json.dumps({'images':[{'filename':name,'idiom':'vision','scale':'2x'}],'info':info}))
            (stack/'Contents.json').write_text(json.dumps({'info':info,'layers':[{'filename':'Front.solidimagestacklayer'},{'filename':'Back.solidimagestacklayer'}]}))
            clear_png(stack/'Front.solidimagestacklayer/Content.imageset/clear-1024.png',1024); png=stack/'Back.solidimagestacklayer/Content.imageset/icon-1024.png'
        else:
            appicon=catalog/'AppIcon.appiconset'; appicon.mkdir(parents=True); png=appicon/'icon-1024.png'
            (appicon/'Contents.json').write_text(json.dumps({'images':[{'filename':png.name,'idiom':'universal','platform':'ios','size':'1024x1024'}],'info':info}))
        (catalog/'Contents.json').write_text(json.dumps({'info':info}))
        png_icon(icon,1024,png,logs/'ios-icon.log',env)
        project=project.replace('      - path: Gen/embed.S\n','      - path: Gen/embed.S\n      - path: Gen/Assets.xcassets\n')
        project=project.replace('      CODE_SIGN_STYLE: Manual\n','      CODE_SIGN_STYLE: Manual\n      ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon\n')
    return project

def project_for_platform(project, platform):
    if platform == 'ios': return project
    if platform != 'visionos': raise ValueError(f'unknown platform {platform}')
    for old,new in [('    iOS: "26.0"','    visionOS: "26.0"'),('    platform: iOS','    platform: visionOS'),
                    ('IPHONEOS_DEPLOYMENT_TARGET','XROS_DEPLOYMENT_TARGET'),
                    ('TARGETED_DEVICE_FAMILY: "1,2"','TARGETED_DEVICE_FAMILY: "7"'),
                    ('        UIRequiresFullScreen: true\n','')]:
        if old not in project: raise RuntimeError(f'project.yml no longer has {old!r}')
        project=project.replace(old,new)
    return project

def seed_reusable(seed, source_hash, source_files, sdk):
    manifest=seed/'seed-manifest.json'
    if not manifest.is_file(): return False
    cached=json.loads(manifest.read_text())
    shaders=[name for name in source_files if name.endswith('_b.metal')]
    return (cached.get('ios_source_manifest_sha256')==source_hash and
            cached.get('ios_metallibs')==len(shaders) and cached.get('ios_sdk','iphoneos')==sdk and
            cached.get('ios_metal_flags')==seed_flags(sdk) and
            all((seed/(name.removesuffix('.metal')+'.metallib')).is_file() for name in shaders))

def url_scheme(version):
    """The URL scheme the game's app answers (Info.plist CFBundleURLTypes): the iPhone MNG Launcher (tools/ios_launcher,
    Games.swift) sees the game installed and starts it through it. One per game, not per version: mng-totk, mng-botw,
    mng-oot, mng-dkb, mng-pkp."""
    return 'mng-'+('botw' if version.startswith('botw') else 'totk' if version in ('v0','v121','v143') else version)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--tree',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--version',choices=['v0','v121','v143','botw160','botw190','dkb','pkp'],required=True);p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--platform',choices=['ios','visionos'],default='ios')
    p.add_argument('--device-gpu',action='append',default=[],metavar='ARCH',help='compile the pipeline archive here for this device GPU (applegpu_g14p: iPhone 14); auto (default): the iPhones / iPads on the cable; none')
    p.add_argument('--image-outside',action='store_true',help='leave romfs.bin out: the device keeps it in Documents/GameData');p.add_argument('--app-name',default='Tears of the Kingdom');p.add_argument('--bundle-id',default='org.macnativegames.totk')
    a=p.parse_args(); tree=a.tree.resolve(); ios=tree/'ios';gen=ios/'Gen';logs=a.logs.resolve(); out=a.out.resolve()
    if out.suffix.lower() not in ('.ipa','.app'): raise RuntimeError('iOS output path must end in .ipa (or .app: the unsigned folder)')
    env=dict(os.environ,DEVELOPER_DIR=developer_dir())
    vision=a.platform=='visionos'; sdk_name='xros' if vision else 'iphoneos'
    if vision and a.version.startswith('botw'): raise RuntimeError('native visionOS builds currently support Tears of the Kingdom only')
    sdk=subprocess.check_output(['xcrun','--sdk',sdk_name,'--show-sdk-path'],env=env,text=True).strip()
    subprocess.run(['xcodebuild','-version'],env=env,check=True,stdout=subprocess.DEVNULL)
    # the repository's tools/deps: pinned XcodeGen (no Homebrew); build_app.py runs this from its copy in the work folder
    sys.path.insert(0,os.environ.get('MNG_REPO') or str(Path(__file__).resolve().parents[4]))
    from tools.deps import program
    generator=program('xcodegen')
    gen.mkdir(parents=True,exist_ok=True)
    botw=a.version.startswith('botw')
    ns2=a.version in ('dkb','pkp')
    # The runtime's tables and hashes from the player's files (runtime/game_data.h) go into the app as files, not into its code.
    game_data=tree/f'build/game-data/{a.version}'
    if not (game_data/'runtime-tables.bin').is_file(): raise RuntimeError(f'missing game data: {game_data} (make game-data-{a.version})')
    # Export the final code after all verified runtime patches have been applied, then embed it in executable segments.
    host=tree/('build/totk-native' if a.version=='v0' else f'build/totk-native-{a.version}')
    rewritten=tree/f'rewritten/{a.version}'; modules=gen/'modules';modules.mkdir(exist_ok=True)
    # The Settings pane chooses the scene size at launch, but the size patch is code the phone cannot write: export
    # with one valid size so the patch is embedded, then the device's own TOTK_RENDER_RES (main.m from the pane) drives
    # it. Without this the embedded code keeps the game's handheld size and the pane's scene row does nothing.
    e=dict(env,TOTK_EXPORT_IOS=str(modules),TOTK_RENDER_RES='1280x720')
    run([host,rewritten,'export-ios'],tree,logs/'export-ios.log',e)
    names=['main',*(f'subsdk{i}' for i in range(10) if (rewritten/f'subsdk{i}.patched.bin').is_file()),'sdk']
    flags=subprocess.check_output([sys.executable,ios/'tools/gen_embed.py',rewritten,gen/'embed.S','--text-dir',modules,*names],text=True,env=env).strip()
    # The app's sources use version-specific patch and generated runtime tables.
    project=project_for_platform((ios/'project.yml').read_text(),a.platform)
    # The Settings pane, copied so it can be changed for the game and given its languages (settings_strings.py: one language,
    # the device's, instead of every row in Chinese and English).
    settings=gen/'Settings.bundle'; shutil.rmtree(settings,ignore_errors=True); shutil.copytree(ios/'Settings.bundle',settings)
    project=project.replace('      - Settings.bundle','      - path: Gen/Settings.bundle',1)
    if ns2:
        project=project.replace('          - game_patches_ns2.c','          - game_patches.c\n          - game_patches_v143.c')
        define='GAME_DKB' if a.version=='dkb' else 'GAME_POKOPIA'
        project=project.replace('-I../build\n',f'-I../build -D{define}=1 -DGAME_NS2=1\n')
        root=plistlib.loads((settings/'Root.plist').read_bytes())
        root=bananza_pane(root) if a.version=='dkb' else pane_for(root,'ns2')
        (settings/'Root.plist').write_bytes(plistlib.dumps(root))
    elif botw:   # game_patches_botw.c instead of both Tears of the Kingdom files; GAME_BOTW for the runtime and the graphics layer
        project=project.replace('          - game_patches_botw.c','          - game_patches.c\n          - game_patches_v143.c')
        project=project.replace('-I../build\n','-I../build -DGAME_BOTW=1 -DGAME_'+a.version.upper()+'=1\n')
        root=botw_pane(plistlib.loads((settings/'Root.plist').read_bytes()))
        (settings/'Root.plist').write_bytes(plistlib.dumps(root))
    elif a.version=='v143': project=project.replace('          - loader.c','          - loader.c\n          - game_patches.c')
    else: project=project.replace('          - loader.c','          - loader.c\n          - game_patches_v143.c')
    if not botw and not ns2:   # Tears of the Kingdom: its own rows and the Zelda ones, without Bananza's and Pokopia's
        (settings/'Root.plist').write_bytes(plistlib.dumps(pane_for(plistlib.loads((settings/'Root.plist').read_bytes()),'totk')))
    settings_strings.localize(settings)   # after the game's own changes to the pane
    if not botw and not ns2 and a.version!='v0': project=project.replace('-I../build\n','-I../build -DTOTK_GAME_'+a.version.upper()+'=1\n')
    project=configure_app_icon(tree,gen,project,logs,env,vision)
    (ios/'project.generated.yml').write_text(project)
    # Build device dependencies separately: Homebrew's Mac archives cannot be linked into an iOS application.
    deps=tree/('build/xros-deps' if vision else 'build/ios-deps')
    run([sys.executable,ios/'tools/build_deps.py',deps,sdk_name],tree,logs/f'{sdk_name}-dependencies.log',env)
    shutil.copytree(deps/'include',gen/'deps/include',dirs_exist_ok=True)
    (gen/'deps/lib').mkdir(parents=True,exist_ok=True)
    for lib in ('libopus.a','libvpx.a'): clone(deps/'lib'/lib,gen/'deps/lib'/lib)
    # The shader translator in the app (build_app.py build_device_translator: the Mac's totk_xlate as a static library and
    # SPIRV-Cross for the device; totk_shader_rt.m DEVICE_XLATE): a variant the startup shaders lack is translated in the
    # background on the device. Without them (visionOS, or this script run on a tree build_app.py did not prepare) the
    # app skips such draws and records the request for fill_shaders.py, as before.
    xlate=tree/'build/ios-xlate'; xlate_libs=('libtotk_xlate_bootstrap.o','libtotk_xlate.a','libtotk_spirv_cross.a')
    device_xlate=not vision and all((xlate/lib).is_file() for lib in xlate_libs)
    for lib in xlate_libs:
        if device_xlate: clone(xlate/lib,gen/'deps/lib'/lib)
        elif (gen/'deps/lib'/lib).exists(): (gen/'deps/lib'/lib).unlink()   # an earlier build's
    print('shader translator in the app: '+('yes' if device_xlate else 'no (startup shaders and fill_shaders.py only)'))
    seed_src=tree/f'build/shader-seed/{a.version}'; seed=gen/'totk/shader-seed'
    if not (seed_src/'seed-manifest.json').is_file(): raise RuntimeError(f'missing verified shader seed: {seed_src}')
    import hashlib
    source_manifest_hash=hashlib.sha256((seed_src/'seed-manifest.json').read_bytes()).hexdigest()
    reuse_seed=seed_reusable(seed,source_manifest_hash,json.loads((seed_src/'seed-manifest.json').read_text())['files'],sdk_name)
    if not reuse_seed:
        discard(seed)
        copy_tree(seed_src,seed)
        run([sys.executable,ios/'tools/compile_seed.py',seed,sdk_name],tree,logs/f'{sdk_name}-shaders.log',env)
    seed_manifest=json.loads((seed/'seed-manifest.json').read_text())
    seed_manifest['ios_source_manifest_sha256']=source_manifest_hash; seed_manifest['ios_sdk']=sdk_name
    (seed/'seed-manifest.json').write_text(json.dumps(seed_manifest,indent=2)+'\n')
    (seed/'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    # The file list: every shader file with its SHA-256. Hashes the manifest already has are kept (the seed's own files,
    # checked when it was prepared, and the metallibs compile_seed.py hashed); only a file it lacks is read, so a build
    # whose seed did not change hashes nothing (all ~100,000 files were read again in each build, 2026-10-22).
    # shader_map.tsv is read in place (DM_NVN_SHMAP); the importer accepts only shader files, so it stays out of the list.
    manifest=json.loads((seed/'seed-manifest.json').read_text()); known=manifest.get('files',{})
    names=sorted(n for n in os.listdir(seed) if n not in ('seed-manifest.json','shader_map.tsv') and not n.startswith(('pipelines-','variant-')) and (seed/n).is_file())
    manifest['files']={n:known.get(n) or hashlib.sha256((seed/n).read_bytes()).hexdigest() for n in names}
    (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Generate the app project with resolved identity and the embedded-segment linker flags.
    env.update(TOTK_APP_NAME=a.app_name,TOTK_BUNDLE_ID=a.bundle_id,TOTK_URL_SCHEME=url_scheme(a.version))   # project.yml CFBundleURLTypes
    xcconfig=gen/'totk.xcconfig'
    frameworks=' '.join('-framework '+x for x in ['UIKit','Metal','MetalFX','Foundation','QuartzCore','ImageIO','CoreGraphics','CoreText','UniformTypeIdentifiers','CoreFoundation','AudioToolbox','GameController','CoreHaptics','CoreMotion','Photos','AVFAudio','Security'])
    # the start-up object as a file of its own (publish_ios_lib.sh: inside the archive nothing would pull it in)
    xlate_link=' Gen/deps/lib/libtotk_xlate_bootstrap.o Gen/deps/lib/libtotk_xlate.a Gen/deps/lib/libtotk_spirv_cross.a -lc++ -lz -licucore' if device_xlate else ''
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a{xlate_link} {flags}\n'
                        + ('OTHER_CFLAGS = $(inherited) -DTOTK_DEVICE_XLATE=1\n' if device_xlate else ''))
    run([generator,'generate','--spec','project.generated.yml'],ios,logs/'xcodegen.log',env)
    derived=tree/('build/xros-derived' if vision else 'build/ios-derived');run(['xcodebuild','-project','TOTKIOS.xcodeproj','-scheme','TOTKIOS','-configuration','Release',
        '-destination','generic/platform='+('visionOS' if vision else 'iOS'),'-derivedDataPath',derived,'CODE_SIGNING_ALLOWED=NO','CODE_SIGNING_REQUIRED=NO','CODE_SIGN_IDENTITY='],
        ios,logs/f'xcodebuild-{a.platform}.log',env)
    app=derived/f'Build/Products/Release-{sdk_name}/TOTK.app'
    if not app.is_dir(): raise RuntimeError(f'Xcode did not produce {app}')
    # Carry the increased-memory-limit entitlement in the executable's ad-hoc signature so re-signing tools keep it;
    # the game's 3.2 GiB pool exceeds the default iOS footprint limit.
    run(['codesign','--force','--sign','-','--entitlements',ios/'Sources/TOTK.entitlements',app/'TOTK'],ios,logs/'ios-entitlements.log',env)
    # The game data goes into a copy of Xcode's product, never into the product itself: it then stays small (its own
    # files only), and each build copies it once instead of three times (product -> Payload -> output, ~27,000 seed
    # files each, 5 of the 7 minutes of a build, 2026-10-23). Products of earlier builds still carry GameData.
    # Renamed beside the product, not inside it: inside, the copy below took the half-deleted folder along (a 33 GB IPA,
    # zip stopped at files the background delete had removed, 2026-10-25).
    if (app/'GameData').exists(): discard(app/'GameData',beside=app.parent)
    dest=out if out.suffix=='.app' else tree/'build/ios-payload/Payload/TOTK.app'
    discard(dest,beside=dest.parent.parent if out.suffix!='.app' else None)   # not in Payload, which is zipped whole
    copy_tree(app,dest)
    res=dest/'GameData';res.mkdir(exist_ok=True)
    copy_tree(game_data,res/'game-data')   # TOTK_GAME_DATA (Sources/main.m)
    romfs=tree/f'extracted/{a.version}/romfs'
    copy_tree(romfs,res/'romfs')
    if a.image_outside:   # the index stays (it changes with the build); the 17 GB image is in Documents/GameData (main.m)
        for p in list((res/'romfs').glob('romfs.bin*')): p.unlink()
    for src,dst in [(tree/f'extracted/{a.version}/romfs-patches',res/'romfs-patches'),
                    (tree/f'extracted/{a.version}/romfs-patches-out',res/'romfs-patches-out'),
                    (tree/'extracted/firmware/shared-fonts',res/'shared-fonts')]:
        if src.is_dir(): copy_tree(src,dst)   # only the overlays and fonts this game actually has
    if botw:   # the add-on content packs the build extracted (games/botw/scripts/extract_aoc_romfs.py), read through TOTK_AOC (main.m)
        aoc=tree/f'extracted/{a.version}/aoc'
        for pack in sorted(aoc.iterdir()) if aoc.is_dir() else []:
            if (pack/'romfs.bin').is_file() and (pack/'runtime-index.bin').is_file():
                for n in ('romfs.bin','runtime-index.bin'): clone(pack/n,res/'aoc'/pack.name/n)
    # The first frame waits only for the title screen's shaders (pso_title.tsv); the runtime looks for the list next to
    # DM_NVN_SHMAP, which is in shader-seed on iOS. Without it the first frame waits for the whole warm-up.
    # Exported from the one shared shader list (games/totk/assets/shader_list.tsv, docs/shader-list.md).
    # The list's other warm rows (its render pipelines) go in as pso_list.tsv: shader-seed/pso_manifest.tsv is the seed's own
    # list there, and the runtime reads both (nvn_mtl.m warm_start). Before 2026-10-22 they stayed in the build logs.
    lists=tree/'build/ios-seed-lists';shutil.rmtree(lists,ignore_errors=True);lists.mkdir(parents=True)
    folder='dkb' if a.version=='dkb' else 'pokopia'
    shader_list=(tree.parent/folder/'assets/shader_list.tsv' if ns2 else
                 tree.parent/'botw/assets/shader_list.tsv' if botw else tree/'assets/shader_list.tsv')
    # This version's rows, each variant named as the device seed serves its bind state (shader_list.py resolve).
    run([sys.executable,tree/'scripts/shader_list.py','resolve',seed_src,shader_list,lists/'pso_list.tsv',
         lists/'pso_title.tsv'],tree,logs/'shader-list.log',env)
    # The device's pipeline archive, compiled here (2026-10-29): the Mac game binary makes every pipeline of the lists from
    # the device seed's metallibs (pipeline-archive mode), device_pipelines.py compiles them for the device GPU with metal-tt
    # into the seed (pipelines-<arch>-*); the ports' build does the same (build_port_ipa.py, device_pipelines.make).
    gpus=device_gpus(a.device_gpu,env)
    lists_args=[lists/'pso_title.tsv',lists/'pso_list.tsv',seed/'pso_manifest.tsv']
    make_device_pipelines(gpus,host,rewritten,lists_args,[seed/'seed-manifest.json',lists/'pso_list.tsv',lists/'pso_title.tsv',host],
                          seed,tree/f'build/ios-pipelines/{a.version}',sdk_name,run,tree,logs,env,clone)
    # What the device reads: the seed and the two lists. With --image-outside it goes beside the app (<out>-shader-seed),
    # for install_device.py to put into Documents/GameData once per content (main.m); unchanged content is not copied again.
    fingerprint=seed_fingerprint(seed,lists)
    seed_out=out.with_name(out.stem+'-shader-seed') if a.image_outside else res/'shader-seed'
    stamp=seed_out/'fingerprint.txt'
    if not (a.image_outside and stamp.is_file() and stamp.read_text().strip()==fingerprint):
        discard(seed_out); copy_tree(seed,seed_out)
        for n in ('pso_list.tsv','pso_title.tsv'): clone(lists/n,seed_out/n)
        stamp.write_text(fingerprint+'\n')
    for g in gpus:   # what goes to the device carries each cabled GPU's pipelines (install_device.py checks it again)
        state,line=seed_check(seed_out,g)
        if state!='ok': raise RuntimeError(f'{seed_out}: {line}')
    if a.image_outside: print(f'shader seed: {seed_out} (install_device.py puts it into Documents once per content)')
    if out.suffix=='.app':   # the unsigned app as a folder, for install_device.py: no 100,000-file zip
        print(f'app ready: {out}; unsigned, install it with ios/tools/install_device.py'); return
    payload=tree/'build/ios-payload'
    out.parent.mkdir(parents=True,exist_ok=True);out.unlink(missing_ok=True)
    run(['zip','-qry','-0',out,'Payload'],payload,logs/'ipa-zip.log',env)
    print(f'IPA ready: {out} ({out.stat().st_size/2**30:.1f} GiB); unsigned, sideload with your signing tool')

if __name__=='__main__':
    try: main()
    except (RuntimeError,subprocess.CalledProcessError,OSError) as e: print(f'ERROR: {e}',file=sys.stderr);sys.exit(1)
