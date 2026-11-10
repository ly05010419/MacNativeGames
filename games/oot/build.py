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

def build_helpers():
    root=str(GAME.parents[1])
    if root not in sys.path:sys.path.insert(0,root)
    import build_app
    return build_app

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

def bundle_shaders():
    """The App carries the offline seed, the Metal libraries (every shader; OOT_SHADER_SUBSET=listed for test builds:
    shader_subset) and the pipeline archive. User 2026-11-08: no translated shaders while the port changes; 2026-11-09:
    "不要编译全部着色器 … 用我最近玩的 … 管线要全部预热", "只要 我一直用的 我收集的", "为什么不能和以前一样 自动";
    2026-11-10: "用户要编译 预热全部着色器。测试只预热我的". OOT_BUNDLE_SHADERS=0 leaves the shaders out (each translated
    at first use)."""
    return os.environ.get('OOT_BUNDLE_SHADERS','1')!='0'

def oot_shader_list():
    """games/oot/scripts/shader_list.py, imported by its path: "import shader_list" can give TOTK's module of the same name
    (another script put games/totk/scripts first, or imported it already), which does not know mesh (M) rows."""
    import importlib.util
    if 'oot_shader_list' not in sys.modules:
        spec=importlib.util.spec_from_file_location('oot_shader_list',GAME/'scripts/shader_list.py')
        module=importlib.util.module_from_spec(spec);sys.modules['oot_shader_list']=module;spec.loader.exec_module(module)
    return sys.modules['oot_shader_list']

def contrib_folder():
    """<clone>/shader-list-contribution (MNG_CONTRIB_FOLDER overrides), as build_app.py contrib_folder: where the launcher
    keeps the contribution file (shader-list-contribution-oot.txt, app/launcher.m contrib_file) and the iPhone / iPad
    reminder merges the devices' rows into it (ios/tools/shader_contrib_watch.py). Made here if missing."""
    folder=Path(os.environ.get('MNG_CONTRIB_FOLDER') or GAME.parents[1]/'shader-list-contribution')
    folder.mkdir(parents=True,exist_ok=True)
    return folder

def resolve_lists(tree, data, logs):
    """The App's warm-up lists beside build/shader-map.tsv, build/pso_manifest.tsv and build/pso_title.tsv: the shared
    list's rows of this game, each variant named as the seed serves its bind state (scripts/shader_list.py resolve), as
    build_app.py resolve_lists makes them for every game it packages. The Mac runtime reads the lists in the shader map's
    folder (nvn_mtl.m warm_start, pso_user_record, pso_contribute) and the launcher prunes the contribution file against them
    (app/launcher.m, totk_shader_contrib.h totk_contrib_prune). With them only in the seed (prepare_ns2_seed.py) every
    pipeline the game drew was written as one the list lacks (2026-11-10: 3,091 rows, 3,040 of them in the list) and the
    title list was never read (user: "要统一")."""
    seed=tree/'build/shader-seed/oot';shared=tree/'assets/shader_list.tsv'
    lists=(data/'build/pso_manifest.tsv',data/'build/pso_title.tsv')
    log=Path(logs)/'shader-list-resolve.log';log.parent.mkdir(parents=True,exist_ok=True)
    with log.open('w') as f:
        subprocess.run([sys.executable,'-I',GAME/'scripts/shader_list.py','resolve',seed,shared,*lists],cwd=GAME,stdout=f,stderr=subprocess.STDOUT,check=True)
    for path in lists:
        if not path.is_file():raise ValueError(f'the shader list resolve wrote no {path}; see {log}')
    print(log.read_text().strip(),flush=True)

def listed_shader_keys(seed, shared, compute):
    """The shaders whose Metal libraries the Mac App carries: every shader the seed's resolved warm-up lists use (a render,
    tile or mesh row names its stages inside its hex key: shader_list.row_variants), the shared list's own variants (its
    V rows) and every compute shader (the compute pipeline archive must be complete). The others are compiled by the game
    at first use. On 2026-11-09 this held all 5,923 shaders picked by hand (the ones played + the warm-up lists)."""
    row_variants=oot_shader_list().row_variants
    keys=set()
    for row in compute:keys.update(name[:64] for name in row_variants(row.split('\t')))
    for path in (Path(seed)/'pso_title.tsv',Path(seed)/'pso_manifest.tsv'):
        if not path.is_file():continue
        for line in path.read_text().splitlines():
            if line and not line.startswith('#'):keys.update(name[:64] for name in row_variants(line.split('\t')))
    if Path(shared).is_file():
        keys.update(line.split('\t')[1] for line in Path(shared).read_text().splitlines() if line.startswith('V\t'))
    return keys

def shader_subset(tree, shared, env=None):
    """Which Metal libraries a full build compiles (user 2026-11-10: "用户要编译 预热全部着色器。测试只预热我的"): a
    player's build compiles every shader (unset); OOT_SHADER_SUBSET=listed, for the test builds, the shaders the list uses
    (listed_shader_file); a path, the keys in that file. None means every shader."""
    want=(os.environ if env is None else env).get('OOT_SHADER_SUBSET','')
    if want in ('','all'):return None
    return listed_shader_file(tree,shared) if want=='listed' else want

def listed_shader_file(tree, shared):
    """listed_shader_keys of this build's seed, written for build_shader_libs.py --only."""
    scripts=str(GAME/'scripts')
    if scripts not in sys.path:sys.path.insert(0,scripts)
    from prepare_compute_pipelines import compute_rows
    seed=tree/'build/shader-seed/oot'
    keys=listed_shader_keys(seed,shared,compute_rows(seed))
    path=tree/'build/shader-libs-listed.txt';path.write_text(''.join(k+'\n' for k in sorted(keys)))
    print(f'Metal libraries: {len(keys)} shaders the list uses (OOT_SHADER_SUBSET=listed; unset compiles every one)',flush=True)
    return str(path)

def verify_shader_resources(tree):
    """Check every prepared variant/library and bind both manifests into the normal build record."""
    seed=tree/'build/shader-seed/oot';libraries=tree/'build/shader-libs'
    if not seed.exists():return None   # explicit development packages from before offline preparation
    metadata=json.loads((seed/'seed-manifest.json').read_text())
    chain=(GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text().split('"')[1]
    expected=json.loads((GAME/'assets/expected-v131072.json').read_text())
    if (metadata.get('version')!='oot' or metadata.get('source_romfs_sha256')!=expected['romfs']['sha256'] or
        metadata.get('translator_sha256')!=sha(GAME/'prebuilt/macos-arm64/totk_xlate') or
        metadata.get('chain')!=chain or not metadata.get('all_default') or
        metadata.get('nvn_depth_mode')!='minus-w' or not metadata.get('corpus_shaders') or
        metadata.get('translated_default')!=metadata['corpus_shaders']):
        raise ValueError('OOT shader seed is incomplete or belongs to another translation context')
    libs=json.loads((libraries/'libraries-manifest.json').read_text())
    if not libs.get('complete'):raise ValueError('OOT Metal libraries are incomplete')
    if '-fpreserve-invariance' not in libs.get('compiler','').split():raise ValueError('OOT Metal libraries lack position invariance')
    shared=build_helpers()
    known_seed=shared.seed_digests(seed);known_libs=shared.seed_digests(libraries)
    for name,want in metadata['files'].items():
        if Path(name).name!=name or shared.seed_sha256(seed,name,known_seed)!=want:raise ValueError('damaged shader seed: '+name)
        if name.endswith('_b.metal'):
            if libs.get('subset') and name not in libs['sources']:continue   # OOT_SHADER_SUBSET builds: compiled by the game at first use
            if libs['sources'].get(name)!=want:raise ValueError('library source differs from seed: '+name)
            library=want+'.metallib'
            if shared.seed_sha256(libraries,library,known_libs)!=libs['files'].get(library):raise ValueError('damaged Metal library: '+library)
    for library,want in libs['files'].items():
        if Path(library).name!=library or shared.seed_sha256(libraries,library,known_libs)!=want:
            raise ValueError('damaged Metal library: '+library)
    scripts=str(GAME/'scripts')
    if scripts not in sys.path:sys.path.insert(0,scripts)
    from prepare_shader_seed import record_verified
    record_verified(seed,metadata['files']);record_verified(libraries,libs['files'])
    record=dict(schema=1,chain=chain,nvn_depth_mode=metadata['nvn_depth_mode'],
                corpus_shaders=metadata['corpus_shaders'],shaders=metadata['shaders'],
                seed_manifest_sha256=sha(seed/'seed-manifest.json'),
                libraries_manifest_sha256=sha(libraries/'libraries-manifest.json'),compiler=libs['compiler'])
    # record_ns2_device_build already hashes every build/game-data/oot file.
    (tree/'build/game-data/oot/precompiled-shaders.json').write_text(json.dumps(record,indent=2)+'\n')
    return record

def shader_bundle(tree,record):
    """Seal the large immutable seed once, using the repository's nested-bundle layout."""
    shared=build_helpers();bundle=tree/'build/shader-bundles/ShaderData-oot.bundle'
    stamp=bundle.with_name(bundle.name+'.json')
    want={k:record[k]for k in ('seed_manifest_sha256','libraries_manifest_sha256')}
    if (bundle/'Contents/_CodeSignature/CodeResources').is_file() and stamp.exists() and json.loads(stamp.read_text())==want:
        return bundle
    temporary=bundle.with_name(bundle.name+'.partial-'+str(os.getpid()))
    if temporary.exists():raise ValueError('unfinished shader bundle: '+str(temporary))
    resources=temporary/'Contents/Resources'
    shared.copy_tree(tree/'build/shader-seed/oot',resources/'shader-seed')
    libraries=tree/'build/shader-libs';manifest=json.loads((libraries/'libraries-manifest.json').read_text())
    for name in manifest['files']:clone_file(libraries/name,resources/'shader-libs'/name)
    clone_file(libraries/'libraries-manifest.json',resources/'shader-libs/libraries-manifest.json')
    (temporary/'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.macnativegames.oot.shaders.oot',CFBundleName='Startup shaders',
        CFBundlePackageType='BNDL',CFBundleShortVersionString='131072',CFBundleVersion='1')))
    subprocess.run(['codesign','--force','--sign','-',temporary],check=True)
    subprocess.run(['codesign','--verify','--strict',temporary],check=True)
    old=bundle.with_name(bundle.name+'.previous-'+str(time.time_ns()))
    if bundle.exists():bundle.rename(old)
    temporary.rename(bundle);stamp.write_text(json.dumps(want)+'\n')
    return bundle

def compute_pipeline_archive(work, tree):
    """Build every seed compute pipeline and, with them, the render pipelines the seed's warm-up lists name (pso_title.tsv /
    pso_manifest.tsv, resolved from games/oot/assets/shader_list.tsv by prepare_ns2_seed.py), then package only a complete
    verified archive: every planned compute pipeline must be in the index; the render rows are reported."""
    scripts=str(GAME/'scripts')
    if scripts not in sys.path:sys.path.insert(0,scripts)
    from prepare_compute_pipelines import compute_rows
    from dev.game_slot import GameSlot
    seed=tree/'build/shader-seed/oot'
    rows=compute_rows(seed)
    folder=tree/'build/compute-pipelines';folder.mkdir(parents=True,exist_ok=True)
    plan=folder/'compute.tsv';plan.write_text('\n'.join(rows)+'\n')
    chain=json.loads((seed/'seed-manifest.json').read_text())['chain']
    # The runtime's archive metadata also checks GPU, OS and chain before reuse.
    cache=folder/chain
    # The tool keeps what its list keys name without looking at the descriptors, so an archive of another runtime must not be reused.
    target_record=tree/'build/pipelines/compute-manifest.json'
    if cache.exists() and target_record.is_file() and json.loads(target_record.read_text()).get('runtime_sha256')!=sha(work/'build/oot-native'):shutil.rmtree(cache)
    logs=work/'logs';logs.mkdir(exist_ok=True)
    log=logs/'compute-pipelines.log'
    env=dict(os.environ,TOTK_NATIVE_DIR=str(tree/'build'),TOTK_SHADER_CACHE=str(cache),
             TOTK_SHADER_SEED_DIR=str(seed),TOTK_PIPELINE_ARCHIVE_HOST='1',TOTK_DEBUG_LOG='1')
    lists=[p for p in (seed/'pso_title.tsv',seed/'pso_manifest.tsv') if p.is_file() and any(l and not l.startswith('#') for l in p.read_text().splitlines())]
    render_rows=sum(1 for p in lists for l in p.read_text().splitlines() if l.startswith(('R\t','B\t','T\t')))
    print(f'Preparing {len(rows)} compute pipelines and {render_rows} listed render pipelines before packaging',flush=True)
    with GameSlot(owner='build',purpose='OOT offline pipelines',expect_seconds=3600),log.open('w') as f:
        tool=subprocess.run([work/'build/oot-native',tree/'rewritten/oot','pipeline-archive',plan,*lists],
                       cwd=tree,env=env,stdout=f,stderr=subprocess.STDOUT)   # exit 1 when any row failed: judged from its report below
    reports=[json.loads(line) for line in log.read_text().splitlines() if line.startswith('{"render":')]
    if not reports:raise ValueError(f'pipeline archive exit {tool.returncode} without a completion report; see {log}')
    report=reports[-1]
    # A listed render row that cannot be built only misses the warm-up (it is built at its draw); every compute pipeline must be there (the index check below).
    if report['write_failed'] or report['compute']+report['kept']<len(rows) or (report['failed'] and not lists):
        raise ValueError('incomplete pipeline archive: '+str(report))
    if report['failed']:print(f"[RED] {report['failed']} listed pipelines were not built; see {log}",flush=True)
    target=tree/'build/pipelines'
    count=build_helpers().mac_pipeline_files(cache/'pipelines',target,report['arch'],chain)
    expected={'c'+hashlib.sha256(row[2:].encode()).hexdigest()[:40]for row in rows}
    index=target/f"pipelines-{report['arch']}.tsv"
    actual={line.split('\t')[0]for line in index.read_text().splitlines()} if index.exists() else set()
    if not expected<=actual:raise ValueError('compute archive index omits planned pipelines')
    report.update(planned=len(rows),listed_render=render_rows,lists=[p.name for p in lists],indexed=count,chain=chain,
                  plan_sha256=sha(plan),runtime_sha256=sha(work/'build/oot-native'),
                  files={p.name:sha(p)for p in sorted(target.iterdir()) if p.is_file() and p.name!='compute-manifest.json'})
    (target/'compute-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return target

def package_app(work,tree,out,game_data_mode='app',game_data_folder=None):
    """Stage a sealed App containing its runtime data, then replace the closed App."""
    shader_record=verify_shader_resources(tree) if bundle_shaders() else None
    pipelines=compute_pipeline_archive(work,tree) if shader_record else None
    prepared=shader_bundle(tree,shader_record) if shader_record else None
    opening_overlay=tree/'extracted/oot/opening-patches'
    subprocess.run([sys.executable,GAME/'scripts/make_opening_overlay.py','--romfs',tree/'extracted/oot/romfs',
                    '--output',opening_overlay],check=True)
    temporary=out.with_name('.'+out.name+'.ready')
    if temporary.exists():raise ValueError('unfinished App staging directory exists; left untouched: '+str(temporary))
    mac=temporary/'Contents/MacOS';res=temporary/'Contents/Resources';data=res/'totk'
    mac.mkdir(parents=True);data.mkdir(parents=True)
    clone_file(work/'build/oot-native',mac/'OcarinaOfTime')
    if pipelines:shutil.copytree(pipelines,data/'build/pipelines',copy_function=clone_file)
    for rel in ('build/game-data/oot','build/shader-service','extracted/oot/exefs',
                'extracted/firmware/shared-fonts'):
        shutil.copytree(tree/rel,data/rel,copy_function=clone_file)
    # Runtime libraries are addressed by the exact MSL source hash. Carry any
    # libraries prepared by the build so a new translation chain does not make
    # the player's first start compile those sources synchronously again.
    libraries=tree/'build/shader-libs'
    if prepared:
        build_helpers().place_shader_bundle(prepared,temporary,data)
    elif libraries.is_dir():
        shutil.copytree(libraries,data/'build/shader-libs',copy_function=clone_file)
    shutil.copytree(tree/'extracted/oot/romfs',data/'extracted/oot/romfs',copy_function=clone_file,
                    ignore=shutil.ignore_patterns('romfs.bin','.romfs-verified*'))
    shutil.copytree(opening_overlay,data/'extracted/oot/romfs-patches',copy_function=clone_file)
    # Reuse the repository's existing game-data packaging. Its signed helper
    # avoids hashing an unchanged 26 GB image on every native runtime update.
    build_app=build_helpers()
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
    resolve_lists(tree,data,work/'logs')   # beside the shader map, where the runtime and the launcher look (build_app.py resolve_lists)
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
    info['MNGContributionFolder']=str(contrib_folder())   # app/launcher.m contrib_file: where players find it, as build_app.py sets it
    (temporary/'Contents/Info.plist').write_bytes(plistlib.dumps(info));(temporary/'Contents/PkgInfo').write_bytes(b'APPL????')
    subprocess.run(['codesign','--force','--sign','-',temporary],check=True)
    subprocess.run(['codesign','--verify','--strict',temporary],check=True)
    # The installed .app folder stays and takes the new contents: macOS keeps its first-run check of an app with the folder.
    while app_running(out):time.sleep(5)   # the build may have taken a while: the App is replaced only once it is closed
    build_app.install_app(temporary,out)
    return out

def app_running(app):
    executable=str((Path(app)/'Contents/MacOS/OcarinaOfTime').resolve())
    return executable in {p.strip()for p in subprocess.check_output(['ps','-axo','comm='],text=True).splitlines()}

def update_runtime(app,work=None):
    """Put a new runtime into an App this build made, keeping its shaders: rebuild the runtime and its pipeline archive only.
    The translations and Metal libraries of the last full build are reused; a new translation chain, translator or request
    code needs the full build. A running game is waited for, not closed."""
    app=Path(app).resolve();work=Path(work).expanduser().resolve() if work else app.parent/'OcarinaOfTime_build'
    repo=GAME.parents[1]
    if work==repo or repo in work.parents:raise ValueError('game build data must remain outside the source checkout')
    tree=work/'oot';seed=tree/'build/shader-seed/oot';manifest=seed/'seed-manifest.json'
    for needed in (manifest,tree/'build/shader-libs/libraries-manifest.json',tree/'extracted/oot/romfs/index.json',tree/'rewritten/oot'):
        if not needed.exists():raise ValueError(f'{needed} is missing: run the full build once with this work folder')
    told=False
    while app_running(app):
        if not told:print('waiting for the game to close',flush=True);told=True
        time.sleep(5)
    start=time.monotonic()
    shutil.copytree(GAME,tree,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc','.venv','build','extracted','rewritten','analysis'))
    scripts=str(GAME/'scripts')
    if scripts not in sys.path:sys.path.insert(0,scripts)
    from prepare_ns2_seed import translation_context
    metadata=json.loads(manifest.read_text())
    if metadata.get('translation_context_sha256')!=translation_context(GAME/'prebuilt/macos-arm64/totk_xlate',metadata.get('nvn_depth_mode')):
        raise ValueError('the translation chain, translator or request code changed: the shaders need the full build')
    expected=json.loads((GAME/'assets/expected-v131072.json').read_text())
    for name,want in expected['rewritten_sha256'].items():
        if sha(tree/'rewritten/oot'/name)!=want:raise ValueError('rewritten module differs from this repository: run the full build: '+name)
    logs=work/'logs';logs.mkdir(exist_ok=True)
    print('building the runtime',flush=True)
    with (logs/'native-runtime.log').open('w') as f:
        subprocess.run(['make','-C',GAME,'WORK='+str(work),'-j'+str(os.cpu_count() or 4)],cwd=work,stdout=f,stderr=subprocess.STDOUT,check=True,
                       env=dict(os.environ,DM_RULESET='native',DM_PRIVATE_GATES='0'))
    os.environ['OOT_BUNDLE_SHADERS']='1'
    folder=plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('MNGGameDataFolder')
    package_app(work,tree,app,'library' if folder else 'app',folder)
    print(f'updated {app} in {time.monotonic()-start:.0f} s',flush=True)

def shader_jobs():
    """Translation workers and Metal compiles at once: the cores minus two (the runtime's own threads and the Metal
    compiler service), 8 on this 10-core M4; 4 left most of the machine idle for 9 minutes of translation."""
    return max(4, min(8, (os.cpu_count() or 4) - 2))

def build(args, source):
    if args.platform not in ('macos','ios'):raise ValueError('Ocarina of Time has macOS and iOS build paths only')
    device=args.platform=='ios'
    if args.update or args.dlc or args.mod:raise ValueError('use the complete original update 131072 in the dump/NUP')
    source=Path(source).resolve();work=args.work.expanduser().resolve() if args.work else source.parent/'OcarinaOfTime_build'
    repo=GAME.parents[1]
    if work==repo or repo in work.parents:raise ValueError('game build data must remain outside the source checkout')
    out=args.out.expanduser().resolve() if args.out else source.parent/'Ocarina of Time.app'
    if out.suffix!='.app' and not (device and out.suffix=='.ipa'):raise ValueError('output must end in .app'+(' or .ipa' if device else ''))
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
    temporary_service=service/'totk_xlate.new'
    shutil.copy2(binary,temporary_service);os.replace(temporary_service,service/'totk_xlate')
    proof=GAME/'prebuilt/macos-arm64/oot_write_proof.dylib'
    if sha(proof)!=proof.with_suffix('.dylib.sha256').read_text().strip():raise ValueError('shader proof analyzer checksum mismatch')
    shutil.copy2(proof,service/proof.name)
    shutil.copy2(proof.with_name('SPIRV-Tools.LICENSE'),service/'SPIRV-Tools.LICENSE')
    (tree/'build/shader-map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    run(['make','-C',GAME,'WORK='+str(work),'-j2'],'native-runtime',work)
    icon_spec=GAME/'assets/app-icon.json'   # the dump has no Control data: the pinned official image, as Donkey Kong Bananza
    if icon_spec.is_file():run([py,'scripts/extract_app_icon.py','--source-spec',icon_spec,'--output','build/AppIcon.icns'],'app-icon')
    if not device and not bundle_shaders():print('Shaders are not bundled (OOT_BUNDLE_SHADERS=0): translated at first use',flush=True)
    else:
     shared=GAME/'assets/shader_list.tsv'   # recorded bind states and pipelines (scripts/shader_list.py merge): listed variants and the warm-up lists
     run([py,GAME/'scripts/prepare_ns2_seed.py','--version','oot','--depth-mode','minus-w',
         '--binary',work/'build/oot-native','--jobs',str(shader_jobs()),'--logs',logs,
         *(['--list',shared] if shared.is_file() else [])],'prepare-shader-seed')
     if not device:
      only=shader_subset(tree,shared)
      run([py,GAME/'scripts/build_shader_libs.py','--cache',tree/'build/shader-seed/oot',
         '--out',tree/'build/shader-libs','--jobs',str(shader_jobs()),*(['--only',only] if only else [])],'compile-shader-libraries')
    if device:
     # The phone's own Metal libraries and pipeline archive: the seed above is compiled for the device in ios/tools/build_ipa.py.
     run([py,tree/'ios/tools/build_ipa.py','--tree',tree,'--out',out,'--version','oot','--logs',logs,'--app-name','OcarinaOfTime','--bundle-id','org.macnativegames.oot',
         *([] if os.environ.get('OOT_IOS_ALL_SHADERS')=='1' else ['--listed-shaders-only']),   # user 2026-11-09: only the shaders the render pipelines use, for now
         *(['--image-outside'] if args.ios_game_data=='documents' else []),*[x for g in args.device_gpu or [] for x in ('--device-gpu',g)]],'build-ipa')
     # The iOS app never asks during play; while the device is connected, this Mac asks to send the shaders the shared list
     # lacks (ios/tools/shader_contrib_watch.py, Ocarina of Time's own), as build_app.py installs it. Not essential.
     try:run([py,tree/'ios/tools/shader_contrib_watch.py','install','--store',contrib_folder()],'shader-contrib-watch')
     except Exception as e:print(f'the Mac reminder for iPhone / iPad shader contributions was not installed: {e}',flush=True)
    else:
     package_app(work,tree,out,'library' if args.mac_game_data_folder else args.mac_game_data,args.mac_game_data_folder)
    print(f'Built {out}; native gameplay validation is recorded separately.',flush=True)
    chain=(GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text().split('"')[1]
    return out,tree,expected,chain
