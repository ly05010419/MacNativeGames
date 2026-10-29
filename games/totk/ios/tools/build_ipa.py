#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone/iPad IPA from a prepared work tree: Tears of the Kingdom (v0, v112, v121, v143)
or Breath of the Wild (botw160, botw190; the runtime and graphics layer built with GAME_BOTW, game_patches_botw.c, its own
shader list and the add-on content beside the RomFS)."""
import argparse, json, os, plistlib, shutil, subprocess, sys, time
from pathlib import Path
from compile_seed import METAL_FLAGS
from device_pipelines import connected_gpus
from xcode_dir import developer_dir

def settings_without_totk(root):
    """The pane for Breath of the Wild, in place: every row Root.plist marks TOTKApp=totk (its picture modes and scene
    sizes, the custom picture tier, MetalFX, and the aspect-ratio row, whose key means ultrawide in Breath of the Wild),
    and any group they leave empty. The Full HD switch stays: Breath of the Wild's docked branch renders the scene at
    1920x1080, so the row is described for it in build_ipa.py. The mark sits on the row itself, so this list is not kept
    a second time here; tests/test_ios_packaging.py checks that every marked row is gone from the pane."""
    specs=[s for s in root['PreferenceSpecifiers'] if s.get('TOTKApp')!='totk']
    kept=[]
    for i,s in enumerate(specs):
        if s.get('Type')=='PSGroupSpecifier':
            nxt=specs[i+1] if i+1<len(specs) else None
            if nxt is None or nxt.get('Type')=='PSGroupSpecifier': continue
        kept.append(s)
    root['PreferenceSpecifiers']=kept
    return root

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
    p.add_argument('--device-gpu',action='append',default=[],metavar='ARCH',help='compile the pipeline archive here for this device GPU (applegpu_g14p: iPhone 14); auto (default): the iPhones / iPads on the cable; none')
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
    flags=subprocess.check_output([sys.executable,ios/'tools/gen_embed.py',rewritten,gen/'embed.S','--text-dir',modules,'main','subsdk0','sdk'],text=True,env=env).strip()
    # The app's sources use version-specific patch and generated runtime tables.
    project=project_for_platform((ios/'project.yml').read_text(),a.platform)
    if botw:   # game_patches_botw.c instead of both Tears of the Kingdom files; GAME_BOTW for the runtime and the graphics layer
        project=project.replace('          - game_patches_botw.c','          - game_patches.c\n          - game_patches_v143.c')
        project=project.replace('-I../build\n','-I../build -DGAME_BOTW=1 -DGAME_'+a.version.upper()+'=1\n')
        # The Settings pane with the Full HD switch described for this game: its docked scene is 1920x1080, not 1600x900.
        # Then the rows Root.plist marks TOTKApp=totk leave (settings_without_totk above); the Full HD row is not one.
        settings=gen/'Settings.bundle'; shutil.rmtree(settings,ignore_errors=True); shutil.copytree(ios/'Settings.bundle',settings)
        root=plistlib.loads((settings/'Root.plist').read_bytes()); specs=root['PreferenceSpecifiers']
        at=next(i for i,s in enumerate(specs) if s.get('Key')=='TOTK_DOCKED')
        specs[at-1]['FooterText']=('開啟：遊戲的底座設定，輸出和畫面都是 1920×1080，需要較快的 iPad（M 系列）。關閉：手持 1280×720（內部 960×540）。'
                                   '重新啟動遊戲後生效 · on: the game\'s docked settings, 1920×1080 output and scene (for an M-series iPad). '
                                   'Off: handheld 1280×720 (960×540 inside). Takes effect after relaunching the game')
        root=settings_without_totk(root)
        (settings/'Root.plist').write_bytes(plistlib.dumps(root))
        project=project.replace('      - Settings.bundle','      - path: Gen/Settings.bundle',1)
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
    names=sorted(n for n in os.listdir(seed) if n not in ('seed-manifest.json','shader_map.tsv') and not n.startswith(('pipelines-','variant-')) and (seed/n).is_file())
    manifest['files']={n:known.get(n) or hashlib.sha256((seed/n).read_bytes()).hexdigest() for n in names}
    (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Generate the app project with resolved identity and the embedded-segment linker flags.
    env.update(TOTK_APP_NAME=a.app_name,TOTK_BUNDLE_ID=a.bundle_id)
    xcconfig=gen/'totk.xcconfig'
    frameworks=' '.join('-framework '+x for x in ['UIKit','Metal','MetalFX','Foundation','QuartzCore','ImageIO','CoreGraphics','CoreText','UniformTypeIdentifiers','CoreFoundation','AudioToolbox','GameController','CoreHaptics','CoreMotion','AVFAudio','Security'])
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
    # The device's pipeline archive, compiled here (2026-10-29): the Mac game binary makes every pipeline of the lists from
    # the device seed's metallibs (pipeline-archive mode), device_pipelines.py compiles them for the device GPU with metal-tt
    # into the seed (pipelines-<arch>-*). The first start then takes them from there instead of filling its own archive:
    # Breath of the Wild 1.6.0 on an iPhone 14 spent 567.8 s on that before the title. Without a GPU to build for, the
    # device fills it as before. Kept in build/ios-pipelines/<version>, so a build whose lists did not change is quick.
    gpus=[]
    for g in a.device_gpu or ['auto']:
        if g=='auto':
            found=connected_gpus(env)
            for name,product,arch in found: print(f'pipelines: {name} ({product}) on the cable: '+(arch or 'GPU not known here, its first start fills them'))
            if not found: print('pipelines: no iPhone / iPad on the cable, so its first start fills them (about 10 minutes); connect it, or --device-gpu <arch>')
            gpus+=[arch for _,_,arch in found if arch]
        elif g!='none': gpus.append(g)
    gpus=sorted(set(gpus))
    pipe_work=tree/f'build/ios-pipelines/{a.version}'
    # Two runs of the Mac binary's mode: the first records the patched variants the runtime compiles from source
    # (variant-scan/variants), device_pipelines.py --variants compiles them for the device into the seed, and the second
    # makes every pipeline from the seed's libraries, those variants included. Both caches are kept between builds.
    lists_args=[lists/'pso_title.tsv',lists/'pso_list.tsv',seed/'pso_manifest.tsv']
    want=hashlib.sha256((seed/'seed-manifest.json').read_bytes()+(lists/'pso_list.tsv').read_bytes()+(lists/'pso_title.tsv').read_bytes()).hexdigest()
    # Under its own name: the game lock takes any totk-native* process for a game that does not use it (runtime/game_slot.c
    # pid_is_game), and the player's game waited until this packaging step ended (2026-10-29). It is a tool, not a game.
    tool=host.with_name('ios-pipeline-archive'); tool.unlink(missing_ok=True); clone(host,tool)
    def mac_pipelines(folder,log):
        stamp=folder/'seed.txt'
        if not (stamp.is_file() and stamp.read_text().strip()==want): shutil.rmtree(folder,ignore_errors=True)   # pipelines of other shaders or lists
        folder.mkdir(parents=True,exist_ok=True)
        run([tool,rewritten,'pipeline-archive',*lists_args],tree,logs/log,dict(env,TOTK_SHADER_CACHE=str(folder),TOTK_SHADER_SEED_DIR=str(seed),TOTK_DEBUG_LOG='1'))
        stamp.write_text(want+'\n')
    t0=time.time()
    if gpus:   # without the last build's variant libraries, so the first run compiles and records every variant it meets
        for p in seed.glob('variant-*'): p.unlink()
        mac_pipelines(pipe_work/'variant-scan','device-pipelines-scan.log')
    run([sys.executable,ios/'tools/device_pipelines.py','--variants',pipe_work/('variant-scan/variants' if gpus else 'none'),'--seed',seed,'--cache',pipe_work,
         '--sdk',sdk_name],tree,logs/'device-variants.log',env)
    if gpus:
        v=json.loads((logs/'device-variants.log').read_text().strip().splitlines()[-1])
        mac_pipelines(pipe_work/'mac-cache','device-pipelines-mac.log')
        print(f"pipelines: made on the Mac in {time.time()-t0:.0f} s, with {v['variants']} variants compiled for the device ({v['compiled']} new, "
              f"{v['failed']} failed); compiling them for {', '.join(gpus)}",flush=True)
    t0=time.time()
    run([sys.executable,ios/'tools/device_pipelines.py','--mac',pipe_work/'mac-cache/pipelines','--seed',seed,'--cache',pipe_work,'--sdk',sdk_name,
         *[x for g in gpus for x in ('--arch',g)]],tree,logs/'device-pipelines.log',env)
    if gpus:
        done=json.loads((logs/'device-pipelines.log').read_text().strip().splitlines()[-1])
        for r in done['archs']: print(f"pipelines: {r['arch']}: {r['pipelines']} ({r['compiled']} compiled, {r['kept']} kept from earlier builds, "
                                      f"{r['failed']} failed) in {time.time()-t0:.0f} s; {done['library from source']} still use shaders the device compiles itself")
    # What the device reads: the seed and the two lists. With --image-outside it goes beside the app (<out>-shader-seed),
    # for install_device.py to put into Documents/GameData once per content (main.m); unchanged content is not copied again.
    fingerprint=hashlib.sha256(b''.join((d/n).read_bytes() for d,n in [(seed,'seed-manifest.json'),(seed,'shader_map.tsv'),
                                        (lists,'pso_list.tsv'),(lists,'pso_title.tsv')]+[(seed,f'pipelines-{g}.tsv') for g in gpus])
                         +''.join(sorted(p.name for p in seed.glob('variant-*'))).encode()).hexdigest()
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
