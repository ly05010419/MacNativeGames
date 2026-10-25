#!/usr/bin/env python3
"""Build Super Smash Bros. Ultimate.app from your own dump of Super Smash Bros. Ultimate in one command.

    python3.14 scripts/build_ssbu.py --game CARD.xci --update UPDATE-13.0.2.nsp --keys prod.keys [--work SSBU_build]
                                     [--out "Super Smash Bros. Ultimate.app"] [--firmware DIR | --shared-fonts DIR]

The game file is the 1.0.0 card image (XCI, or the base NSP with --firmware), the update is the 13.0.2 NSP, whose code
the app runs. Steps (each is skipped when its verified output is already there, so a stopped run can be started again):
 1. the Python environment (<work>/venv, from requirements.txt; Python 3.14 reads the NRO modules' zstd frames)
 2. the 13.0.2 code from the update and the RomFS of the card image plus the update (17.6 GB, all six IVFC levels
    verified), each against assets/expected-v13.0.2.json
 3. the three program modules rewritten for the Mac, then the NRO modules (fighters, stages, modes) found in data.arc by
    the digests the game's NRR lists and rewritten the same way
 4. the six system fonts (the card image's system update partition, --firmware or --shared-fonts) and the runtime tables
 5. the shader tools, the startup shaders (every shader in data.arc, the shared list assets/shader_list.tsv and what your
    own game noted while playing) and their Metal libraries
 6. the game binary, the launcher and the game's icon
 7. the app: the game data as a signed bundle in Contents/Helpers, checked once by macOS

Everything this writes stays in the work folder, which Git ignores: it holds game data, so never share or commit it. The
app keeps saves, the shader cache and its log in ~/Library/Application Support/MacNativeGames/SSBU and
~/Library/Logs/MacNativeGames/ssbu.log; the build never touches them.
"""
import argparse
import ctypes
import glob
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import tempfile
import sys
import threading
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[1]
VERSION = 'v1302'
EXPECTED = json.loads((GAME_DIR / 'assets/expected-v13.0.2.json').read_text())
APP_NAME, BUNDLE_ID, EXECUTABLE = 'Super Smash Bros. Ultimate', 'org.macnativegames.ssbu', 'SSBU'
DATA_FOLDER = 'SSBU'   # runtime/game_version.h GAME_FOLDER
TITLE_ID = '01006a800016e000'
MODULES = ('sdk', 'subsdk0', 'main')
NRO_FILES = ('patched.bin', 'fixups.tsv', 'gates.tsv')   # what the host's nn::ro reads per module (runtime/ro_host.c)
LIST = 'assets/shader_list.tsv'
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
HOMEBREW = ('opus', 'libvpx', 'libvorbis', 'libogg')   # linked statically by the Makefile


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def run(cmd, log, cwd=GAME_DIR, env=None):
    """Run a step with its whole output in log, a line a minute while it runs; on failure show the end of the log."""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    done = threading.Event()

    def beat():
        t0 = time.time()
        while not done.wait(60):
            say(f'   ... still running ({(time.time() - t0) / 60:.0f} min)')
    threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            p = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    finally:
        done.set()
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-25:]
        raise BuildError(f'{Path(cmd[1] if Path(cmd[0]).name.startswith("python") else cmd[0]).name} failed '
                         f'(exit {p.returncode}); full output: {log}\n' + '\n'.join(tail))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def check(what, got, want):
    if got != want:
        raise BuildError(f'{what}: SHA-256 {got[:16]}... is not the expected {want[:16]}... '
                         f'(is this the supported game version, and the dump complete?)')


# ---- files: APFS clones where possible (the app is a clone of the work folder, no extra space) ----
_libc = None


def clonefile(src, dst):
    global _libc
    if _libc is None:
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
    return _libc.clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0


def clone(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    if not clonefile(src, dst):
        shutil.copy2(src, dst)


def copy_tree(src, dst):
    if not dst.exists():
        dst.parent.mkdir(parents=True, exist_ok=True)
        if clonefile(src, dst):
            return
    for p in sorted(src.rglob('*')):
        if p.is_file():
            clone(p, dst / p.relative_to(src))


def remove_tree(path):
    """Finder can put .DS_Store back while a bundle is deleted, which makes the last rmdir fail."""
    for _ in range(5):
        try:
            shutil.rmtree(path)
            return
        except OSError as error:
            if error.errno != 66 or not path.exists():
                raise
            time.sleep(0.2)
    shutil.rmtree(path)


# ---- 1. inputs and the Python environment ----
def input_kind(game):
    with game.open('rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xci'
    raise BuildError(f'{game}: expected an XCI card image or an NSP')


def running_from(work):
    """Pids of games started from this work folder's binary (make boot, a test script): a build replaces files they run
    (the binary, the shader translator), so it waits for them to close instead (2026-10-24: a build during play ended
    the player's session; the save was intact)."""
    binary = (work / 'build/ssbu-native').resolve()
    found = subprocess.run(['pgrep', '-x', 'ssbu-native'], capture_output=True, text=True).stdout.split()
    pids = []
    for pid in found:
        lines = subprocess.run(['lsof', '-a', '-p', pid, '-d', 'txt', '-Fn'], capture_output=True, text=True).stdout.splitlines()
        if any(line[1:] and Path(line[1:]).resolve() == binary for line in lines if line.startswith('n')):
            pids.append(pid)
    return pids


def step_environment(work, python, logs):
    venv = work / 'venv'
    if not (venv / 'bin/python').exists():
        say('1. Python environment for the build tools')
        run([python, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', GAME_DIR / 'requirements.txt'], logs / 'pip.log')
    probe = subprocess.run([venv / 'bin/python', '-c', 'import compression.zstd'], capture_output=True)
    if probe.returncode:
        raise BuildError(f'{venv}: needs Python 3.14 (its standard library reads the NRO modules\' zstd frames); '
                         'start the build with python3.14, or delete the venv folder made by an older Python')
    return venv / 'bin/python'


# ---- 2.-3. code and data ----
def romfs_valid(romfs):
    try:
        index = json.loads((romfs / 'index.json').read_text())
        return (index.get('status') == 'pass' and index['romfs_sha256'] == EXPECTED['romfs']['sha256']
                and (romfs / 'romfs.bin').stat().st_size == EXPECTED['romfs']['size'])
    except (OSError, ValueError, KeyError):
        return False


def nros_extracted(nro):
    try:
        manifest = json.loads((nro / 'manifest.json').read_text())
        modules = manifest['modules']
        return (manifest.get('status') == 'pass' and manifest['found'] == manifest['listed'] == len(modules) > 0
                and all(sha256(nro / f"{m['sha256']}.nro") == m['sha256'] for m in modules))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def nro_record(nro, out):
    """What a finished NRO rewrite is checked by on the next build: the rewriter and the script that ran it, and the
    SHA-256 of every file the runtime reads, per module."""
    modules = json.loads((nro / 'manifest.json').read_text())['modules']
    return dict(format=1, rewriter_sha256=sha256(GAME_DIR / 'third_party/d2r/patch_nso.py'),
                script_sha256=sha256(GAME_DIR / 'scripts/rewrite_nros.py'),
                modules={m['sha256']: {kind: sha256(out / f"{m['sha256']}.{kind}") if (out / f"{m['sha256']}.{kind}").is_file()
                                       else None for kind in NRO_FILES} for m in modules})


def step_code(work, py, base, update, keys, logs):
    exefs, romfs, out = work / f'extracted/{VERSION}/exefs', work / f'extracted/{VERSION}/romfs', work / f'rewritten/{VERSION}'
    want = EXPECTED['exefs_sha256']
    if not all((exefs / n).is_file() and sha256(exefs / n) == h for n, h in want.items()):
        say('2. code: the 13.0.2 program from the update')
        if exefs.exists():
            shutil.rmtree(exefs)
        run([py, 'scripts/extract_code.py', update, '--keys', keys, '--output', exefs], logs / 'extract-code.log')
        for n, h in want.items():
            check(f'ExeFS {n}', sha256(exefs / n), h)
    if not romfs_valid(romfs):
        say('2. data: the RomFS of the card image and the 13.0.2 update, all six IVFC levels verified (17.6 GB, minutes)')
        if romfs.exists():
            shutil.rmtree(romfs)
        run([py, 'scripts/merge_romfs.py', base, update, '--keys', keys, '--output', romfs], logs / 'merge-romfs.log')
        check('13.0.2 RomFS', json.loads((romfs / 'index.json').read_text())['romfs_sha256'], EXPECTED['romfs']['sha256'])
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    for m in MODULES:
        image, h = out / f'{m}.patched.bin', EXPECTED['rewritten_sha256'][f'{m}.patched.bin']
        if image.is_file() and sha256(image) == h:
            continue
        say(f'3. code: rewriting {m} for the Mac' + (' (the big one: a few minutes)' if m == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / m, out, m], logs / f'rewrite-{m}.log', env=env)
        check(f'rewritten {m}', sha256(image), h)
    nro, nro_out = work / f'extracted/{VERSION}/nro', out / 'nro'
    if not nros_extracted(nro):
        say('3. code: finding the NRO modules in data.arc (each one SHA-256-equal to a digest of the game\'s NRR)')
        if nro.exists():
            shutil.rmtree(nro)
        run([py, 'scripts/extract_nros.py', '--romfs', romfs, '--output', nro], logs / 'extract-nros.log')
        if not nros_extracted(nro):
            raise BuildError(f'the NRO modules were not all found in data.arc (see {logs / "extract-nros.log"})')
    stamp = nro_out / 'rewrite.json'
    try:
        done = json.loads(stamp.read_text()) == nro_record(nro, nro_out)
    except (OSError, ValueError):
        done = False
    if not done:
        count = len(json.loads((nro / 'manifest.json').read_text())['modules'])
        say(f'3. code: rewriting the {count} NRO modules for the Mac')
        if nro_out.exists():
            shutil.rmtree(nro_out)
        run([py, 'scripts/rewrite_nros.py', '--nro', nro, '--output', nro_out], logs / 'rewrite-nros.log', env=env)
        record = nro_record(nro, nro_out)
        missing = [f'{d[:16]}.{kind}' for d, files in record['modules'].items() for kind, h in files.items() if h is None]
        if missing:
            raise BuildError(f'the NRO rewrite left out {len(missing)} files, first {missing[0]} (see {logs / "rewrite-nros.log"})')
        stamp.write_text(json.dumps(record, indent=2) + '\n')


def fonts_valid(folder):
    """scripts/extract_shared_fonts.py finished: six fonts, each <type>-<name>.bfttf with the SHA-256 the manifest names
    ("file" there is the font's name in the firmware, not in this folder)."""
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return (manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6))
                and all(sha256(folder / f"{f['type']}-{f['name']}.bfttf") == f['sha256'] for f in manifest['fonts']))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def step_data(work, py, game, keys, logs, firmware=None, shared_fonts=None):
    fonts = work / 'extracted/firmware/shared-fonts'
    if shared_fonts:
        source = shared_fonts
    elif fonts_valid(fonts):
        source = fonts
    else:
        if not firmware and input_kind(game) != 'xci':
            raise BuildError('an NSP carries no system fonts: pass --firmware <unpacked firmware NCA folder> or '
                             '--shared-fonts <a verified font folder>')
        say('4. data: the six system fonts' + ('' if firmware else " from the card image's system update partition"))
        if fonts.exists():
            shutil.rmtree(fonts)
        source_args = ['--firmware', firmware] if firmware else ['--xci', game]
        run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source_args, '--output', fonts], logs / 'shared-fonts.log')
        if not fonts_valid(fonts):
            raise BuildError(f'the system fonts in {fonts} do not check out (see {logs / "shared-fonts.log"})')
        source = fonts
    say('4. data: runtime tables, RomFS index and fonts')
    # fonts already in the work folder are read there; --shared-fonts copies another folder in
    copy = [] if Path(source).resolve() == fonts.resolve() else ['--shared-fonts', source]
    run([py, 'scripts/prepare_runtime.py', '--work', work, *copy], logs / 'prepare-runtime.log')


def make_path(work):
    """The work folder as make can name it. Make splits every path at spaces (2026-10-24: a work folder in
    "Super Smash Brothers Ultimate/" stopped the first make with "No rule to make target"), so a folder with a space is
    handed over as a link without one in the temporary folder; everything still lands in the work folder."""
    if ' ' not in str(work):
        return work
    alias = Path(tempfile.gettempdir()) / f'ssbu-work-{hashlib.sha256(str(work).encode()).hexdigest()[:12]}'
    if ' ' in str(alias):
        raise BuildError(f'the work folder {work} has a space and the temporary folder {alias.parent} too; pass --work '
                         'a folder without spaces')
    if alias.is_symlink() and alias.resolve() != work.resolve():
        alias.unlink()
    if not alias.is_symlink():
        alias.symlink_to(work)
    return alias


# ---- 5. shaders ----
def list_with_notes(work, py, logs):
    """The shared list plus what the player's own game noted that it lacks (the app keeps those rows in
    ~/Library/Application Support/MacNativeGames/SSBU/shader-list-contribution.txt): merged into a copy in the work
    folder, so what one session met is ready at the next build without sending anything anywhere."""
    notes = Path.home() / 'Library/Application Support/MacNativeGames' / DATA_FOLDER / 'shader-list-contribution.txt'
    count = work / 'build/notes-rows.txt'   # the app carries it; the launcher reminds when the notes run well past it
    count.unlink(missing_ok=True)
    try:
        rows = sum(1 for line in notes.read_text(errors='replace').splitlines() if line and not line.startswith('#'))
    except OSError:
        return GAME_DIR / LIST
    if not rows:
        return GAME_DIR / LIST
    count.parent.mkdir(parents=True, exist_ok=True)
    count.write_text(f'{rows}\n')
    merged = work / 'build/shader_list.with-notes.tsv'
    shutil.copyfile(GAME_DIR / LIST, merged)
    run([py, 'scripts/shader_list.py', 'merge', merged, notes], logs / 'shader-notes.log')
    say(f'5. shaders: the shared list plus the {rows} rows your game noted while playing')
    return merged


def step_shaders(work, py, logs):
    mk = make_path(work)
    make = ['make', f'WORK={mk}', f'PY={mk / "venv/bin/python"}']
    run([*make, 'shader-tools'], logs / 'shader-tools.log')
    lst = list_with_notes(work, py, logs)
    say('5. shaders: the startup shaders from data.arc and the shared list (long the first time; a rerun reuses them)')
    build = work / 'build'
    run([py, 'scripts/prepare_shader_seed.py', '--game', 'ssbu', '--romfs', work / f'extracted/{VERSION}/romfs',
         '--output', build / 'shader-seed', '--translator', build / 'shader-service/totk_xlate',
         '--spirv-cross', build / 'spirv-cross/spirv-cross', '--version', VERSION, '--modules', work / f'rewritten/{VERSION}',
         '--shader-list', lst], logs / 'shader-seed.log')
    run([py, 'scripts/shader_list.py', 'export', lst, build / 'pso_manifest.tsv', build / 'pso_title.tsv'],
        logs / 'shader-list-export.log')
    say('5. shaders: compiling the startup shaders into Metal libraries (a rerun compiles only what is new)')
    run([*make, 'shader-libs'], logs / 'shader-libs.log')


# ---- 6. binary, launcher, icon ----
def step_binary(work, py, game, keys, logs):
    say('6. app: the game binary and the launcher')
    mk = make_path(work)
    run(['make', f'-j{os.cpu_count() or 4}', f'WORK={mk}', f'PY={mk / "venv/bin/python"}', 'runtime'], logs / 'runtime.log')
    run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Ithird_party/d2r_gfx/gfx/nvn', '-Iruntime', 'app/launcher.m',
         '-framework', 'AppKit', '-o', work / 'build/app-launcher'], logs / 'launcher.log')
    icon = work / 'build/AppIcon.icns'
    if not icon.exists():   # the game's own icon from its control data; optional
        try:
            run([py, 'scripts/extract_app_icon.py', game, '--keys', keys, '--title', TITLE_ID, '--output', icon], logs / 'app-icon.log')
        except BuildError as e:
            say(f'   no app icon, the app gets the default one ({str(e).splitlines()[0]})')


# ---- 7. the app ----
def data_bundle(work, logs):
    """The RomFS image (17.6 GB) as a bundle of its own, signed once and kept in the work folder (build/data-bundles):
    the app carries a clone of it in Contents/Helpers and its seal records only the bundle's signature, so a packaging
    does not hash 17.6 GB again. The stamp names the image (size, modification time and the SHA-256 the extraction
    verified). The game opens the image through a link at extracted/<version>/romfs/romfs.bin and checks it itself."""
    image = work / f'extracted/{VERSION}/romfs/romfs.bin'
    bundle = work / f'build/data-bundles/GameData-{VERSION}.bundle'
    stamp = bundle.with_name(bundle.name + '.json')
    st = image.stat()
    want = dict(format=2, size=st.st_size, mtime_ns=st.st_mtime_ns,
                sha256=json.loads((image.parent / 'index.json').read_text()).get('romfs_sha256'))
    try:
        if (bundle / 'Contents/_CodeSignature/CodeResources').is_file() and json.loads(stamp.read_text()) == want:
            return bundle
    except (OSError, ValueError):
        pass
    say('7. app: signing the game data once (a minute or two; later builds reuse it)')
    tmp = bundle.with_name(bundle.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    clone(image, tmp / 'Contents/Resources/romfs.bin')
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier=f'{BUNDLE_ID}.data.{VERSION}', CFBundleName='Game data', CFBundlePackageType='BNDL',
        CFBundleShortVersionString=EXPECTED['display_version'], CFBundleVersion='1')))
    run(['codesign', '--force', '--sign', '-', tmp], logs / 'sign-data.log')
    run(['codesign', '--verify', '--strict', tmp], logs / 'verify-data.log')
    stamp.unlink(missing_ok=True)
    if bundle.exists():
        shutil.rmtree(bundle)
    tmp.rename(bundle)
    stamp.write_text(json.dumps(want))
    return bundle


def app_seal_valid(app):
    return subprocess.run(['codesign', '--verify', '--strict', app], capture_output=True).returncode == 0


def keep_game_data(new, old):
    """When the installed app carries the same signed game data (same signature, Info.plist and image size), move its
    data bundle into the new app in place of the fresh clone: the image keeps its inode and times, so the runtime's
    RomFS check (keyed by them) still matches and the first start does not hash 17.6 GB again."""
    image = 'Contents/Resources/romfs.bin'
    kept = False
    for bundle in sorted((new / 'Contents/Helpers').glob('GameData-*.bundle')):
        before = old / 'Contents/Helpers' / bundle.name
        if not all((before / rel).is_file() and (before / rel).read_bytes() == (bundle / rel).read_bytes()
                   for rel in ('Contents/_CodeSignature/CodeResources', 'Contents/Info.plist')):
            continue
        if not (before / image).is_file() or (before / image).stat().st_size != (bundle / image).stat().st_size:
            continue
        fresh = new.with_name(f'{new.name}.{bundle.name}.fresh')
        if fresh.exists():
            remove_tree(fresh)
        bundle.rename(fresh)
        try:
            before.rename(bundle)
        except OSError:
            fresh.rename(bundle)
            continue
        if not app_seal_valid(new):
            bundle.rename(before)
            fresh.rename(bundle)
            continue
        remove_tree(fresh)
        kept = True
    return kept


def install_app(new, out):
    """Put the signed app `new` at `out`, its seal checked before the installed app is touched. The .app folder itself
    stays (macOS keeps its first-run check of an app with the folder) and takes the new contents; the replaced contents
    are deleted in the background."""
    kept = out.exists() and keep_game_data(new, out)
    if not kept and not app_seal_valid(new):
        raise BuildError(f'{new}: the new app\'s signature does not verify (codesign --verify --strict); the installed app is unchanged')
    leftovers = sorted(out.parent.glob(f'{glob.escape(out.name)}.replaced-*'))
    if out.exists():
        old = out.with_name(f'{out.name}.replaced-{os.getpid()}')
        if old.exists():
            remove_tree(old)
        old.mkdir()
        for entry in list(out.iterdir()):
            entry.rename(old / entry.name)
        for entry in list(new.iterdir()):
            entry.rename(out / entry.name)
        new.rmdir()
        leftovers.append(old)
    else:
        new.rename(out)
    for path in leftovers:
        subprocess.Popen(['rm', '-rf', str(path)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    return kept


def step_app(work, out, logs):
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        remove_tree(tmp)
    res = tmp / 'Contents/Resources/totk'   # the launcher's game folder (app/launcher.m)
    clone(work / 'build/ssbu-native', res / 'build/totk-native')
    files = ['build/spirv-cross/spirv-cross', 'build/shader-service/totk_xlate', f'extracted/{VERSION}/romfs/runtime-index.bin',
             'build/pso_manifest.tsv', 'build/pso_title.tsv']
    files += [f'rewritten/{VERSION}/{m}.patched.bin' for m in MODULES]
    for digest in json.loads((work / f'rewritten/{VERSION}/nro/rewrite.json').read_text())['modules']:
        files += [f'rewritten/{VERSION}/nro/{digest}.{kind}' for kind in NRO_FILES]
    if (work / 'build/notes-rows.txt').is_file():
        files.append('build/notes-rows.txt')
    for rel in files:
        clone(work / rel, res / rel)
    copy_tree(work / 'extracted/firmware/shared-fonts', res / 'extracted/firmware/shared-fonts')
    seed = work / 'build/shader-seed'
    if not (seed / 'seed-manifest.json').is_file():
        raise BuildError('the startup shaders were not prepared (step 5); refusing an app that would translate every shader in play')
    copy_tree(seed, res / 'build/shader-seed')
    if (work / 'build/shader-libs').is_dir():
        copy_tree(work / 'build/shader-libs', res / 'build/shader-libs')
    bundle = data_bundle(work, logs)   # after the checks above: signing it is slow
    copy_tree(bundle, tmp / 'Contents/Helpers' / bundle.name)
    link = res / f'extracted/{VERSION}/romfs/romfs.bin'
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(os.path.relpath(tmp / 'Contents/Helpers' / bundle.name / 'Contents/Resources/romfs.bin', link.parent))
    clone(work / 'build/app-launcher', tmp / f'Contents/MacOS/{EXECUTABLE}')
    info = dict(CFBundleDevelopmentRegion='en', CFBundleExecutable=EXECUTABLE, CFBundleIdentifier=BUNDLE_ID, CFBundleName=APP_NAME,
                CFBundleDisplayName=APP_NAME, CFBundlePackageType='APPL', CFBundleShortVersionString=EXPECTED['display_version'],
                CFBundleVersion=EXPECTED['display_version'], LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSApplicationCategoryType='public.app-category.action-games', LSSupportsGameMode=True,
                GCSupportsControllerUserInteraction=True)
    icon = work / 'build/AppIcon.icns'
    if icon.exists():
        clone(icon, tmp / 'Contents/Resources/AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    (tmp / 'Contents/PkgInfo').write_bytes(b'APPL????')
    run(['codesign', '--force', '--sign', '-', tmp], logs / 'sign-app.log')   # a failed seal leaves the installed app alone
    old_binary = out / 'Contents/Resources/totk/build/totk-native'
    notified = 0
    while old_binary.is_file():   # replacing the resources of a running game is not safe: wait for it to close
        processes = subprocess.run(['ps', '-axo', 'comm='], capture_output=True, text=True, check=True).stdout.splitlines()
        if str(old_binary.resolve()) not in (p.strip() for p in processes):
            break
        if time.monotonic() - notified >= 60:
            say('7. app: the new app is ready; waiting for the running game to close before replacing it')
            notified = time.monotonic()
        time.sleep(2)
    if install_app(tmp, out):
        say('7. app: the game data is unchanged; kept the installed copy, so the first start need not check it again')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', type=Path, required=True, help='the 1.0.0 card image (XCI), or the base NSP with --firmware')
    ap.add_argument('--update', type=Path, required=True, help='the 13.0.2 update NSP')
    ap.add_argument('--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('--work', type=Path, default=GAME_DIR / 'SSBU_build', help='work folder (default games/ssbu/SSBU_build)')
    ap.add_argument('--out', type=Path, help='the app to write (default: next to the game file)')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='an unpacked firmware NCA folder with the six system fonts (needed for an NSP)')
    fonts.add_argument('--shared-fonts', type=Path, help='a font folder an earlier build verified (manifest.json + six .bfttf)')
    a = ap.parse_args(argv)
    t0 = time.time()
    game, update, keys = (p.expanduser().resolve() for p in (a.game, a.update, a.keys))
    for p in (game, update, keys):
        if not p.is_file():
            raise BuildError(f'no such file: {p}')
    input_kind(game)
    if input_kind(update) != 'nsp':
        raise BuildError(f'{update}: the 13.0.2 update is an NSP')
    work = a.work.expanduser().resolve()
    out = (a.out.expanduser().resolve() if a.out else game.parent / f'{APP_NAME}.app')
    if out.suffix != '.app':
        raise BuildError(f'the output must end in .app: {out}')
    missing = [p for p in HOMEBREW if not (Path('/opt/homebrew/opt') / p / 'lib').is_dir()]
    if missing:
        raise BuildError(f'Homebrew packages missing: brew install {" ".join(missing)}')
    work.mkdir(parents=True, exist_ok=True)
    logs = work / 'logs/build'
    if shutil.disk_usage(work).free / 2**30 < 30 and not romfs_valid(work / f'extracted/{VERSION}/romfs'):
        raise BuildError('needs about 30 GB free for the work folder (the app itself is an APFS clone of it on the same volume)')
    say(f'{APP_NAME} {EXPECTED["display_version"]}; work folder {work}')
    notified = 0
    while pids := running_from(work):
        if time.monotonic() - notified >= 60:
            say(f'the game runs from this work folder (pid {", ".join(pids)}); the build waits until it is closed')
            notified = time.monotonic()
        time.sleep(5)
    py = step_environment(work, sys.executable, logs)
    step_code(work, py, game, update, keys, logs)
    step_data(work, py, game, keys, logs, firmware=a.firmware and a.firmware.expanduser().resolve(),
              shared_fonts=a.shared_fonts and a.shared_fonts.expanduser().resolve())
    step_shaders(work, py, logs)
    step_binary(work, py, game, keys, logs)
    say(f'7. app: assembling {out}')
    step_app(work, out, logs)
    run([LSREGISTER, '-f', out], logs / 'register-app.log')
    say('7. app: macOS checks the app once (seconds when this app folder was checked before, a few minutes for a new one)')
    r = subprocess.run([out / f'Contents/MacOS/{EXECUTABLE}', '--check'], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
    say(f'Open it from Finder. Saves, the shader cache and the log go to ~/Library/Application Support/MacNativeGames/'
        f'{DATA_FOLDER} and ~/Library/Logs/MacNativeGames/ssbu.log.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
