#!/usr/bin/env python3
"""Build an unsigned, sideloadable iPhone/iPad IPA from a prepared TOTK work tree."""
import argparse, json, os, shutil, subprocess, sys
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
def copy_tree(src,dst):
    src=Path(src);dst=Path(dst)
    if dst.exists(): shutil.rmtree(dst)
    dst.mkdir(parents=True,exist_ok=True)
    if sys.platform=='darwin':
        result=subprocess.run(['/bin/cp','-cR',str(src)+os.sep+'.',str(dst)],capture_output=True)
        if result.returncode==0: return
        shutil.rmtree(dst);dst.mkdir(parents=True,exist_ok=True)
    for p in src.rglob('*'):
        if p.is_file(): clone(p,dst/p.relative_to(src))

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--tree',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--version',choices=['v0','v112','v121','v143'],required=True);p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--app-name',default='Tears of the Kingdom');p.add_argument('--bundle-id',default='org.macnativegames.totk')
    a=p.parse_args(); tree=a.tree.resolve(); ios=tree/'ios';gen=ios/'Gen';logs=a.logs.resolve(); out=a.out.resolve()
    if out.suffix.lower()!='.ipa': raise RuntimeError('iOS output path must end in .ipa')
    env=dict(os.environ,DEVELOPER_DIR=developer_dir())
    sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
    subprocess.run(['xcodebuild','-version'],env=env,check=True,stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'): raise RuntimeError('install xcodegen: brew install xcodegen')
    gen.mkdir(parents=True,exist_ok=True)
    version_dir=tree/'build'/({'v0':'','v112':'v112','v121':'v121','v143':'v143'}[a.version])
    if a.version=='v0': version_dir=tree/'build'
    for name in ('runtime_tables.h','romfs_config.h'):
        clone(version_dir/name,gen/name)
    clone(tree/'build/shared_fonts_config.h',gen/'shared_fonts_config.h')
    # Export the final code after all verified runtime patches have been applied, then embed it in executable segments.
    host=tree/('build/totk-native' if a.version=='v0' else f'build/totk-native-{a.version}')
    rewritten=tree/f'rewritten/{a.version}'; modules=gen/'modules';modules.mkdir(exist_ok=True)
    e=dict(env,TOTK_EXPORT_IOS=str(modules))
    run([host,rewritten,'export-ios'],tree,logs/'export-ios.log',e)
    flags=subprocess.check_output([sys.executable,ios/'tools/gen_embed.py',rewritten,gen/'embed.S','--text-dir',modules,'main','subsdk0','sdk'],text=True,env=env).strip()
    # The app's sources use version-specific patch and generated runtime tables.
    project=(ios/'project.yml').read_text()
    if a.version=='v143': project=project.replace('          - loader.c','          - loader.c\n          - game_patches.c')
    else: project=project.replace('          - loader.c','          - loader.c\n          - game_patches_v143.c')
    if a.version!='v0': project=project.replace('-I../build\n','-I../build -DTOTK_GAME_'+a.version.upper()+'=1\n')
    (ios/'project.generated.yml').write_text(project)
    # Build device dependencies separately: Homebrew's Mac archives cannot be linked into an iOS application.
    deps=tree/'build/ios-deps'
    run([sys.executable,ios/'tools/build_deps.py',deps],tree,logs/'ios-dependencies.log',env)
    shutil.copytree(deps/'include',gen/'deps/include',dirs_exist_ok=True)
    (gen/'deps/lib').mkdir(parents=True,exist_ok=True)
    for lib in ('libopus.a','libvpx.a'): clone(deps/'lib'/lib,gen/'deps/lib'/lib)
    seed_src=tree/f'build/shader-seed/{a.version}'; seed=gen/'totk/shader-seed'
    if not (seed_src/'seed-manifest.json').is_file(): raise RuntimeError(f'missing verified shader seed: {seed_src}')
    import hashlib
    source_manifest_hash=hashlib.sha256((seed_src/'seed-manifest.json').read_bytes()).hexdigest()
    expected_metallibs=sum(name.endswith('_b.metal') for name in json.loads((seed_src/'seed-manifest.json').read_text())['files'])
    cached_manifest=seed/'seed-manifest.json'
    reuse_seed=False
    if cached_manifest.is_file():
        cached=json.loads(cached_manifest.read_text())
        reuse_seed=(cached.get('ios_source_manifest_sha256')==source_manifest_hash and
                    cached.get('ios_metallibs')==expected_metallibs and
                    cached.get('ios_metal_flags')==' '.join(METAL_FLAGS) and   # e.g. not the old fast-math libraries
                    all((seed/(name.removesuffix('.metal')+'.metallib')).is_file()
                        for name in json.loads((seed_src/'seed-manifest.json').read_text())['files'] if name.endswith('_b.metal')))
    if not reuse_seed:
        if seed.exists(): shutil.rmtree(seed)
        copy_tree(seed_src,seed)
        run([sys.executable,ios/'tools/compile_seed.py',seed,'iphoneos'],tree,logs/'ios-shaders.log',env)
    seed_manifest=json.loads((seed/'seed-manifest.json').read_text())
    seed_manifest['ios_source_manifest_sha256']=source_manifest_hash
    (seed/'seed-manifest.json').write_text(json.dumps(seed_manifest,indent=2)+'\n')
    (seed/'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    # Regenerate file hashes after adding iOS metallibs and the tiny runtime map.
    manifest=json.loads((seed/'seed-manifest.json').read_text())
    # shader_map.tsv is read in place (DM_NVN_SHMAP); the importer accepts only shader files, so it stays out of the list.
    manifest['files']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(seed.iterdir())
                       if f.is_file() and f.name not in ('seed-manifest.json','shader_map.tsv')}
    (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Generate the app project with resolved identity and the embedded-segment linker flags.
    env.update(TOTK_APP_NAME=a.app_name,TOTK_BUNDLE_ID=a.bundle_id)
    xcconfig=gen/'totk.xcconfig'
    frameworks=' '.join('-framework '+x for x in ['UIKit','Metal','MetalFX','Foundation','QuartzCore','ImageIO','CoreGraphics','CoreText','UniformTypeIdentifiers','CoreFoundation','AudioToolbox','GameController','AVFAudio','Security'])
    xcconfig.write_text(f'OTHER_LDFLAGS = $(inherited) {frameworks} Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a {flags}\n')
    run(['xcodegen','generate','--spec','project.generated.yml'],ios,logs/'xcodegen.log',env)
    derived=tree/'build/ios-derived';run(['xcodebuild','-project','TOTKIOS.xcodeproj','-scheme','TOTKIOS','-configuration','Release',
        '-destination','generic/platform=iOS','-derivedDataPath',derived,'CODE_SIGNING_ALLOWED=NO','CODE_SIGNING_REQUIRED=NO','CODE_SIGN_IDENTITY='],
        ios,logs/'xcodebuild-ios.log',env)
    app=derived/'Build/Products/Release-iphoneos/TOTK.app'
    if not app.is_dir(): raise RuntimeError(f'Xcode did not produce {app}')
    # Carry the increased-memory-limit entitlement in the executable's ad-hoc signature so re-signing tools keep it;
    # the game's 3.2 GiB pool exceeds the default iOS footprint limit.
    run(['codesign','--force','--sign','-','--entitlements',ios/'Sources/TOTK.entitlements',app/'TOTK'],ios,logs/'ios-entitlements.log',env)
    res=app/'GameData';res.mkdir(exist_ok=True)
    romfs=tree/f'extracted/{a.version}/romfs'
    copy_tree(romfs,res/'romfs')
    for src,dst in [(tree/f'extracted/{a.version}/romfs-patches',res/'romfs-patches'),
                    (tree/f'extracted/{a.version}/romfs-patches-out',res/'romfs-patches-out'),
                    (tree/'extracted/firmware/shared-fonts',res/'shared-fonts')]: copy_tree(src,dst)
    copy_tree(seed,res/'shader-seed')
    # The first frame waits only for the title screen's shaders (pso_title.tsv); the runtime looks for the list next to
    # DM_NVN_SHMAP, which is in shader-seed on iOS. Without it the first frame waits for the whole warm-up.
    # Exported from the one shared shader list (games/totk/assets/shader_list.tsv, docs/shader-list.md).
    run([sys.executable,tree/'scripts/shader_list.py','export',tree/'assets/shader_list.tsv',logs/'pso_manifest.tsv',
         res/'shader-seed/pso_title.tsv'],tree,logs/'shader-list.log',env)
    # Keep the player's first install self-contained; the app stores saves and writable shader cache in Documents.
    payload=tree/'build/ios-payload';shutil.rmtree(payload,ignore_errors=True);(payload/'Payload').mkdir(parents=True)
    copy_tree(app,payload/'Payload/TOTK.app')
    out.parent.mkdir(parents=True,exist_ok=True);out.unlink(missing_ok=True)
    run(['zip','-qry','-0',out,'Payload'],payload,logs/'ipa-zip.log',env)
    print(f'IPA ready: {out} ({out.stat().st_size/2**30:.1f} GiB); unsigned, sideload with your signing tool')

if __name__=='__main__':
    try: main()
    except (RuntimeError,subprocess.CalledProcessError,OSError) as e: print(f'ERROR: {e}',file=sys.stderr);sys.exit(1)
