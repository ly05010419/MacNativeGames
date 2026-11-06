"""Ocarina of Time's isolated native build, called by the repository build_app.py."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import time

GAME=Path(__file__).resolve().parent
VERSION='oot'
TITLE='0400486024DEC000'

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        while data:=f.read(8<<20):h.update(data)
    return h.hexdigest()

def clone_file(source, destination):
    """APFS copy-on-write keeps the verified game image independent without duplicating disk blocks."""
    import ctypes
    source=Path(source);destination=Path(destination)
    destination.parent.mkdir(parents=True,exist_ok=True)
    libc=ctypes.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True)
    clone=libc.clonefile;clone.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int];clone.restype=ctypes.c_int
    if clone(os.fsencode(source),os.fsencode(destination),0):
        if shutil.disk_usage(destination.parent).free<source.stat().st_size+(1<<30):
            raise OSError('APFS cloning unavailable and insufficient space for '+str(source))
        shutil.copy2(source,destination)
    return str(destination)

def package_app(work,tree,out,game_data_mode='app',game_data_folder=None):
    """Stage a sealed App containing its runtime data, then replace the closed App."""
    opening_overlay=tree/'extracted/oot/opening-patches'
    subprocess.run([sys.executable,GAME/'scripts/make_opening_overlay.py','--romfs',tree/'extracted/oot/romfs',
                    '--output',opening_overlay],check=True)
    temporary=out.with_name('.'+out.name+'.ready')
    if temporary.exists():raise ValueError('unfinished App staging directory exists; left untouched: '+str(temporary))
    mac=temporary/'Contents/MacOS';res=temporary/'Contents/Resources';data=res/'totk'
    mac.mkdir(parents=True);data.mkdir(parents=True)
    clone_file(work/'build/oot-native',mac/'OcarinaOfTime')
    for rel in ('build/game-data/oot','build/shader-service','extracted/oot/exefs',
                'extracted/firmware/shared-fonts'):
        shutil.copytree(tree/rel,data/rel,copy_function=clone_file)
    # Runtime libraries are addressed by the exact MSL source hash. Carry any
    # libraries prepared by the build so a new translation chain does not make
    # the player's first start compile those sources synchronously again.
    libraries=tree/'build/shader-libs'
    if libraries.is_dir():
        shutil.copytree(libraries,data/'build/shader-libs',copy_function=clone_file)
    shutil.copytree(tree/'extracted/oot/romfs',data/'extracted/oot/romfs',copy_function=clone_file,
                    ignore=shutil.ignore_patterns('romfs.bin','.romfs-verified*'))
    shutil.copytree(opening_overlay,data/'extracted/oot/romfs-patches',copy_function=clone_file)
    # Reuse the repository's existing game-data packaging. Its signed helper
    # avoids hashing an unchanged 26 GB image on every native runtime update.
    root=str(GAME.parents[1])
    if root not in sys.path:sys.path.insert(0,root)
    import build_app
    if game_data_mode=='library':
        game_data_folder=(Path(game_data_folder) if game_data_folder else Path.home()/'Library/Application Support/MacNativeGames/OcarinaOfTime/GameData').expanduser().resolve()
        repo=GAME.parents[1]
        if game_data_folder==repo or repo in game_data_folder.parents:raise ValueError('game library must remain outside the source checkout')
        expected=json.loads((GAME/'assets/expected-v131072.json').read_text())
        build_app.place_library_image(tree,'oot','oot',game_data_folder,expected_romfs=expected['romfs'])
    elif game_data_mode=='app':
        logs=work/'logs';logs.mkdir(exist_ok=True)
        bundle=build_app.data_bundle(tree,'oot',logs,bundle_id='org.macnativegames.oot',display_version='131072')
        build_app.place_data_bundle(bundle,temporary,data,'oot')
    else:raise ValueError('invalid Mac game-data mode')
    rewrites=data/'rewritten/oot';rewrites.mkdir(parents=True)
    for module in sorted((tree/'rewritten/oot').glob('*.patched.bin')):clone_file(module,rewrites/module.name)
    clone_file(tree/'build/shader-map.tsv',data/'build/shader-map.tsv')
    # VideoToolbox is the primary AV1 decoder; the same bounded dav1d fallback
    # used by the native host is bundled, independent of a Homebrew installation.
    dependencies=subprocess.check_output(['otool','-L',mac/'OcarinaOfTime'],text=True)
    dav1d=next(line.strip().split(' (')[0]for line in dependencies.splitlines()if '/libdav1d.'in line)
    framework=temporary/'Contents/Frameworks'/Path(dav1d).name
    clone_file(Path(dav1d).resolve(),framework)
    subprocess.run(['install_name_tool','-change',dav1d,'@executable_path/../Frameworks/'+framework.name,mac/'OcarinaOfTime'],check=True)
    subprocess.run(['codesign','--force','--sign','-',framework],check=True)
    clone_file(GAME/'third_party/dav1d-COPYING',res/'licenses/dav1d-COPYING')
    info=dict(CFBundleDevelopmentRegion='en',CFBundleExecutable='OcarinaOfTime',CFBundleIdentifier='org.macnativegames.oot',CFBundleName='Ocarina of Time',CFBundleDisplayName='Ocarina of Time',CFBundlePackageType='APPL',CFBundleShortVersionString='131072',CFBundleVersion='131072',LSMinimumSystemVersion='26.0',NSHighResolutionCapable=True,LSSupportsGameMode=True,GCSupportsControllerUserInteraction=True)
    if game_data_mode=='library':info['MNGGameDataFolder']=str(game_data_folder)
    (temporary/'Contents/Info.plist').write_bytes(plistlib.dumps(info));(temporary/'Contents/PkgInfo').write_bytes(b'APPL????')
    subprocess.run(['codesign','--force','--sign','-',temporary],check=True)
    subprocess.run(['codesign','--verify','--strict',temporary],check=True)
    # A previous native App is retained as an external build artifact, without a
    # second desktop entry or a second player profile. Its resource links are not followed.
    if out.exists():
        history=work/'app-history';history.mkdir(exist_ok=True)
        previous=history/(time.strftime('%Y%m%d-%H%M%S')+'.saved')
        out.rename(previous)
    temporary.rename(out)
    return out

def build(args, source):
    if args.platform!='macos':raise ValueError('Ocarina of Time currently has a macOS build path only')
    if args.update or args.dlc or args.mod:raise ValueError('use the complete original update 131072 in the dump/NUP')
    source=Path(source).resolve();work=args.work.expanduser().resolve() if args.work else source.parent/'OcarinaOfTime_build'
    repo=GAME.parents[1]
    if work==repo or repo in work.parents:raise ValueError('game build data must remain outside the source checkout')
    out=args.out.expanduser().resolve() if args.out else source.parent/'Ocarina of Time.app'
    if out.suffix!='.app':raise ValueError('output must end in .app')
    if out==repo or repo in out.parents:raise ValueError('App must remain outside the source checkout')
    if args.vr or args.native_4k or args.native_5k:raise ValueError('this title currently uses its original rendering options')
    # Close this title normally before changing its resource workspace or App.
    # The input command follows the same save-safe window close as normal play.
    executable=out/'Contents/MacOS/OcarinaOfTime'
    def running():
        processes=subprocess.check_output(['ps','-axo','comm='],text=True).splitlines()
        return str(executable) in {p.strip()for p in processes}
    if running():
        control=Path(os.environ.get('TOTK_INPUT_FILE',str(Path.home()/'Library/Application Support/MacNativeGames/OcarinaOfTime/input.txt')))
        with control.open('a')as f:f.write('close\n')
        deadline=time.monotonic()+120
        while running():
            if time.monotonic()>deadline:raise RuntimeError('game did not finish its normal close; App left untouched')
            time.sleep(1)
    work.mkdir(parents=True,exist_ok=True);tree=work/'oot';logs=work/'logs';logs.mkdir(exist_ok=True)
    shutil.copytree(GAME,tree,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc','.venv','build','extracted','rewritten','analysis'))
    env=dict(os.environ,DM_RULESET='native',DM_PRIVATE_GATES='0')
    def run(cmd,name,cwd=tree):
        print(name,flush=True)
        with (logs/(name+'.log')).open('w') as f:subprocess.run([str(x)for x in cmd],cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
    py=work/'venv/bin/python'
    if not py.exists():run([sys.executable,'-m','venv',work/'venv'],'python-environment',work)
    dependencies=subprocess.run([py,'-c','import lz4.block,capstone,zbic'],capture_output=True)
    if dependencies.returncode:run([py,'-m','pip','install','-r',GAME/'requirements.txt'],'python-dependencies',work)
    extracted=tree/'extracted/oot';index=extracted/'romfs/index.json'
    expected=json.loads((GAME/'assets/expected-v131072.json').read_text())
    if source.is_file():run([py,'scripts/import_nup.py',source,'--output',extracted],'import-original-nup')
    elif not index.exists():run([py,'scripts/import_dump.py',source,'--output',extracted,'--update','131072'],'import-original-dump')
    code=extracted/'exefs'
    for name,digest in expected['exefs_sha256'].items():
        if sha(code/name)!=digest:raise ValueError('unsupported or damaged original ExeFS: '+name)
    rom=json.loads(index.read_text())
    if rom['romfs_sha256']!=expected['romfs']['sha256'] or rom['romfs_size']!=expected['romfs']['size']:raise ValueError('unsupported RomFS image')
    if 'directories' not in rom:
        folder=source/'updates/131072/romfs'
        if not folder.is_dir():raise ValueError('directory metadata missing from original dump')
        rom['directories']=sorted(str(p.relative_to(folder))for p in folder.rglob('*')if p.is_dir());index.write_text(json.dumps(rom)+'\n')
    run([py,'scripts/prepare_modules.py',code,'--output',extracted/'elf'],'verify-original-modules')
    rewritten=tree/'rewritten/oot';rewritten.mkdir(parents=True,exist_ok=True)
    def rewrite(name):
        target=rewritten/(name+'.patched.bin');want=expected['rewritten_sha256'].get(target.name)
        if not target.exists() or sha(target)!=want:run([py,'third_party/d2r/patch_nso.py',code/name,rewritten,name],'rewrite-'+name)
        run([py,'third_party/d2r/structck.py',rewritten,name,code/name],'verify-rewrite-'+name)
        if sha(target)!=want:raise ValueError('rewrite differs from verified rules: '+name)
    with concurrent.futures.ThreadPoolExecutor(2)as pool:list(pool.map(rewrite,['main','subsdk0','sdk']))
    hashes={str(p.relative_to(tree)):sha(p)for p in [*(code/n for n in ('main','subsdk0','sdk')),*(rewritten/(n+'.patched.bin')for n in ('main','subsdk0','sdk'))]}
    report=tree/'analysis/oot-port/phase12-validation.json';report.parent.mkdir(parents=True,exist_ok=True)
    report.write_text(json.dumps(dict(format=1,status='pass',rom_version='oot',module_file_sha256=hashes),indent=2)+'\n')
    for script in ('prepare_runtime.py','prepare_romfs_backend.py'):run([py,'scripts/'+script,'--version','oot'],script[:-3])
    fonts=tree/'extracted/firmware/shared-fonts'
    if args.shared_fonts:shutil.copytree(args.shared_fonts.expanduser().resolve(),fonts,dirs_exist_ok=True)
    if not (fonts/'manifest.json').exists():raise ValueError('pass --shared-fonts with the six verified firmware fonts')
    run([py,'scripts/prepare_shared_fonts.py','--version','oot'],'prepare-shared-fonts')
    service=tree/'build/shader-service';service.mkdir(parents=True,exist_ok=True)
    binary=GAME/'prebuilt/macos-arm64/totk_xlate'
    if sha(binary)!=(binary.with_suffix('.sha256')).read_text().strip():raise ValueError('shader translator checksum mismatch')
    shutil.copy2(binary,service/'totk_xlate')
    (tree/'build/shader-map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    run(['make','-C',GAME,'WORK='+str(work),'-j2'],'native-runtime',work)
    package_app(work,tree,out,'library' if args.mac_game_data_folder else args.mac_game_data,args.mac_game_data_folder)
    print(f'Built {out}; native gameplay validation is recorded separately.',flush=True)
    chain=(GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text().split('"')[1]
    return out,tree,expected,chain
