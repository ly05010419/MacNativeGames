#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone/iPad IPA from a prepared work tree: Tears of the Kingdom (v0, v112, v121, v143)
or Breath of the Wild (botw160, botw190; the runtime and graphics layer built with GAME_BOTW, game_patches_botw.c, its own
shader list and the add-on content beside the RomFS)."""
import argparse, json, os, shutil, subprocess, sys, time
from pathlib import Path
from compile_seed import METAL_FLAGS
from xcode_dir import developer_dir

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
def clone_folder(src,dst):
    """One APFS clone of a whole folder (macOS clonefile), as build_app.py copy_tree: the seed is ~100,000 files, which
    cp -cR clones one by one (minutes per copy, 2026-10-22)."""
    if sys.platform!='darwin': return False
    import ctypes
    libc=ctypes.CDLL(None);libc.clonefile.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint32]
    dst.parent.mkdir(parents=True,exist_ok=True)
    return libc.clonefile(os.fsencode(src),os.fsencode(dst),0)==0

def discard(path):
    """Out of the way at once (a rename), deleted in the background: tens of thousands of files take minutes to delete on
    an external disk, and the build does not need to wait for it."""
    if not path.exists(): return
    old=path.with_name(f'.{path.name}.old-{os.getpid()}-{time.time_ns()}')
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
            cached.get('ios_metal_flags')==' '.join(METAL_FLAGS) and
            all((seed/(name.removesuffix('.metal')+'.metallib')).is_file() for name in shaders))

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--tree',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--version',choices=['v0','v112','v121','v143','botw160','botw190'],required=True);p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--platform',choices=['ios','visionos'],default='ios')
    p.add_argument('--image-outside',action='store_true',help='leave romfs.bin out: the device keeps it in Documents/GameData');p.add_argument('--app-name',default='Tears of the Kingdom');p.add_argument('--bundle-id',default='org.macnativegames.totk')
    a=p.parse_args(); tree=a.tree.resolve(); ios=tree/'ios';gen=ios/'Gen';logs=a.logs.resolve(); out=a.out.resolve()
    if out.suffix.lower() not in ('.ipa','.app'): raise RuntimeError('iOS output path must end in .ipa (or .app: the unsigned folder)')
    env=dict(os.environ,DEVELOPER_DIR=developer_dir())
    vision=a.platform=='visionos'; sdk_name='xros' if vision else 'iphoneos'
    if vision and a.version.startswith('botw'): raise RuntimeError('native visionOS builds currently support Tears of the Kingdom only')
    sdk=subprocess.check_output(['xcrun','--sdk',sdk_name,'--show-sdk-path'],env=env,text=True).strip()
    subprocess.run(['xcodebuild','-version'],env=env,check=True,stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'): raise RuntimeError('install xcodegen: brew install xcodegen')
    gen.mkdir(parents=True,exist_ok=True)
    botw=a.version.startswith('botw')
    version_dir=tree/'build'/('' if a.version=='v0' else a.version)
    for name in ('runtime_tables.h','romfs_config.h')+(('aoc_config.h',) if botw else ()):
        clone(version_dir/name,gen/name)
    clone(tree/'build/shared_fonts_config.h',gen/'shared_fonts_config.h')
    # Export the final code after all verified runtime patches have been applied, then embed it in executable segments.
    host=tree/('build/totk-native' if a.version=='v0' else f'build/totk-native-{a.version}')
    rewritten=tree/f'rewritten/{a.version}'; modules=gen/'modules';modules.mkdir(exist_ok=True)
    e=dict(env,TOTK_EXPORT_IOS=str(modules))
    run([host,rewritten,'export-ios'],tree,logs/'export-ios.log',e)
    flags=subprocess.check_output([sys.executable,ios/'tools/gen_embed.py',rewritten,gen/'embed.S','--text-dir',modules,'main','subsdk0','sdk'],text=True,env=env).strip()
    # The app's sources use version-specific patch and generated runtime tables.
    project=project_for_platform((ios/'project.yml').read_text(),a.platform)
    if botw:   # game_patches_botw.c instead of both Tears of the Kingdom files; GAME_BOTW for the runtime and the graphics layer
        project=project.replace('          - game_patches_botw.c','          - game_patches.c\n          - game_patches_v143.c')
        project=project.replace('-I../build\n','-I../build -DGAME_BOTW=1 -DGAME_'+a.version.upper()+'=1\n')
    elif a.version=='v143': project=project.replace('          - loader.c','          - loader.c\n          - game_patches.c')
    else: project=project.replace('          - loader.c','          - loader.c\n          - game_patches_v143.c')
    if not botw and a.version!='v0': project=project.replace('-I../build\n','-I../build -DTOTK_GAME_'+a.version.upper()+'=1\n')
    # The home screen icon: the game's own, which the build took from its control data for the Mac app
    # (build/AppIcon.icns, scripts/extract_app_icon.py), as a single-size asset catalog. Without it the app had none.
    icon=tree/'build/AppIcon.icns'; catalog=gen/'Assets.xcassets'; shutil.rmtree(catalog,ignore_errors=True)
    if icon.is_file() and not vision:
        appicon=catalog/'AppIcon.appiconset'; appicon.mkdir(parents=True)
        (catalog/'Contents.json').write_text(json.dumps({'info':{'author':'xcode','version':1}}))
        run(['sips','-s','format','png','-z','1024','1024',icon,'--out',appicon/'icon-1024.png'],tree,logs/'ios-icon.log',env)
        (appicon/'Contents.json').write_text(json.dumps({'images':[{'filename':'icon-1024.png','idiom':'universal','platform':'ios','size':'1024x1024'}],
                                                         'info':{'author':'xcode','version':1}}))
        project=project.replace('      - path: Gen/embed.S\n','      - path: Gen/embed.S\n      - path: Gen/Assets.xcassets\n')
        project=project.replace('      CODE_SIGN_STYLE: Manual\n','      CODE_SIGN_STYLE: Manual\n      ASSETCATALOG_COMPILER_APPICON_NAME: AppIcon\n')
    (ios/'project.generated.yml').write_text(project)
    # Build device dependencies separately: Homebrew's Mac archives cannot be linked into an iOS application.
    deps=tree/('build/xros-deps' if vision else 'build/ios-deps')
    run([sys.executable,ios/'tools/build_deps.py',deps,sdk_name],tree,logs/f'{sdk_name}-dependencies.log',env)
    shutil.copytree(deps/'include',gen/'deps/include',dirs_exist_ok=True)
    (gen/'deps/lib').mkdir(parents=True,exist_ok=True)
    for lib in ('libopus.a','libvpx.a'): clone(deps/'lib'/lib,gen/'deps/lib'/lib)
    seed_src=tree/f'build/shader-seed/{a.version}'; seed=gen/'totk/shader-seed'
    if not (seed_src/'seed-manifest.json').is_file(): raise RuntimeError(f'missing verified shader seed: {seed_src}')
    import hashlib
    source_manifest_hash=hashlib.sha256((seed_src/'seed-manifest.json').read_bytes()).hexdigest()
    reuse_seed=seed_reusable(seed,source_manifest_hash,json.loads((seed_src/'seed-manifest.json').read_text())['files'],sdk_name)
    if not reuse_seed:
        if seed.exists(): shutil.rmtree(seed)
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
    names=sorted(n for n in os.listdir(seed) if n not in ('seed-manifest.json','shader_map.tsv') and (seed/n).is_file())
    manifest['files']={n:known.get(n) or hashlib.sha256((seed/n).read_bytes()).hexdigest() for n in names}
    (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Generate the app project with resolved identity and the embedded-segment linker flags.
    env.update(TOTK_APP_NAME=a.app_name,TOTK_BUNDLE_ID=a.bundle_id)
    xcconfig=gen/'totk.xcconfig'
    frameworks=' '.join('-framework '+x for x in ['UIKit','Metal','MetalFX','Foundation','QuartzCore','ImageIO','CoreGraphics','CoreText','UniformTypeIdentifiers','CoreFoundation','AudioToolbox','GameController','AVFAudio','Security'])
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a {flags}\n')
    run(['xcodegen','generate','--spec','project.generated.yml'],ios,logs/'xcodegen.log',env)
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
    if (app/'GameData').exists(): discard(app/'GameData')
    dest=out if out.suffix=='.app' else tree/'build/ios-payload/Payload/TOTK.app'
    discard(dest); copy_tree(app,dest)
    res=dest/'GameData';res.mkdir(exist_ok=True)
    romfs=tree/f'extracted/{a.version}/romfs'
    copy_tree(romfs,res/'romfs')
    if a.image_outside:   # the index stays (it changes with the build); the 17 GB image is in Documents/GameData (main.m)
        for p in list((res/'romfs').glob('romfs.bin*')): p.unlink()
    for src,dst in [(tree/f'extracted/{a.version}/romfs-patches',res/'romfs-patches'),
                    (tree/f'extracted/{a.version}/romfs-patches-out',res/'romfs-patches-out'),
                    (tree/'extracted/firmware/shared-fonts',res/'shared-fonts')]:
        if src.is_dir() or not botw: copy_tree(src,dst)   # Breath of the Wild has no RomFS overlays
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
    shader_list=tree.parent/'botw/assets/shader_list.tsv' if botw else tree/'assets/shader_list.tsv'   # the work folder's copy of games/botw
    run([sys.executable,tree/'scripts/shader_list.py','export',shader_list,lists/'pso_list.tsv',
         lists/'pso_title.tsv'],tree,logs/'shader-list.log',env)
    aliases=seed_src.with_name(seed_src.name+'.aliases.tsv')   # listed variants the seed carries under another name
    if aliases.is_file():
        run([sys.executable,tree/'scripts/shader_list.py','rename',aliases,lists/'pso_title.tsv',
             lists/'pso_list.tsv'],tree,logs/'shader-list-rename.log',env)
    # What the device reads: the seed and the two lists. With --image-outside it goes beside the app (<out>-shader-seed),
    # for install_device.py to put into Documents/GameData once per content (main.m); unchanged content is not copied again.
    fingerprint=hashlib.sha256(b''.join((d/n).read_bytes() for d,n in [(seed,'seed-manifest.json'),(seed,'shader_map.tsv'),
                                        (lists,'pso_list.tsv'),(lists,'pso_title.tsv')])).hexdigest()
    seed_out=out.with_name(out.stem+'-shader-seed') if a.image_outside else res/'shader-seed'
    stamp=seed_out/'fingerprint.txt'
    if not (a.image_outside and stamp.is_file() and stamp.read_text().strip()==fingerprint):
        discard(seed_out); copy_tree(seed,seed_out)
        for n in ('pso_list.tsv','pso_title.tsv'): clone(lists/n,seed_out/n)
        stamp.write_text(fingerprint+'\n')
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
