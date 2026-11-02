#!/usr/bin/env python3
"""Build Mario vs. Donkey Kong.app from your own dump of Mario vs. Donkey Kong 1.0.1 in one command.

    python3.14 scripts/build_mvdk.py --game MVDK.nsp --update UPDATE-1.0.1.nsp --keys prod.keys
                                     (--firmware DIR | --shared-fonts DIR) [--work MvDK_build] [--out "Mario vs. Donkey Kong.app"]

The game file is the base game (NSP or XCI; an XCI carries the system fonts, an NSP needs --firmware or --shared-fonts),
the update is 1.0.1 (0100B99019412800, v65536): the app runs 1.0.1, its code from the update, its RomFS the base game's
merged with the update's. Steps (each is skipped when its verified output is already there, so a stopped run can be started again):
 1. this folder copied into the work folder (<work>/mvdk: the scripts write extracted/, rewritten/, analysis/ and build/
    next to themselves), the Python environment (<work>/venv, from requirements.txt) and the pinned SPIRV-Cross sources
 2. the update's code and its three modules (main, subsdk0, sdk) rewritten for the Mac, against assets/expected-v1.0.1.json
 3. the RomFS (1.3 GB, merged from both, all six IVFC levels verified) and the six system fonts
 4. SPIRV-Cross, the game binary, the launcher and the game's icon
 5. the app: the game data as a signed bundle in Contents/Helpers, checked once by macOS

There is no startup shader set for this game yet: each shader is translated the first time the game draws it, then cached.
Everything this writes stays in the work folder, which Git ignores: it holds game data, so never share or commit it. The
app keeps saves, the shader cache and its log in ~/Library/Application Support/MacNativeGames/MvDK and
~/Library/Logs/MacNativeGames/mvdk.log; the build never touches them.
"""
import argparse
import concurrent.futures
import ctypes
import glob
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[1]
# runtime/game_version.h TOTK_GAME_DIR (extracted/<version>, rewritten/<version>) -> the pinned hashes of that version
VERSIONS = {'mvdk101': json.loads((GAME_DIR / 'assets/expected-v1.0.1.json').read_text())}
VERSION = 'mvdk101'   # the only version: 1.0.1, base game + update
EXPECTED = VERSIONS[VERSION]
APP_NAME, BUNDLE_ID, EXECUTABLE = 'Mario vs. Donkey Kong', 'org.macnativegames.mvdk', 'MvDK'
DATA_FOLDER = 'MvDK'   # runtime/game_version.h GAME_FOLDER
TITLE_ID = '0100b99019412000'
MODULES = ('main', 'subsdk0', 'sdk')   # the modules 1.0.1 ships (assets/expected-v1.0.1.json rewritten_sha256)
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
HOMEBREW = ('opus', 'libvpx')   # linked statically by the Makefile


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


def link(path, target):
    if path.is_symlink() or path.exists():
        if path.is_symlink() and path.resolve() == Path(target).resolve():
            return
        path.unlink() if path.is_symlink() else shutil.rmtree(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.symlink_to(target)


# ---- 1. inputs, the work tree and the Python environment ----
def input_kind(game):
    with game.open('rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xci'
    raise BuildError(f'{game}: expected an XCI card image or an NSP')


def git_fetch(url, commit, dst, log):
    if (dst / '.git').is_dir() and subprocess.run(['git', '-C', dst, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit:
        return
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)


def prepare_work(work, python, logs):
    """<work>/mvdk: this folder with the generated files inside it. Files are compared by content and a changed file gets
    the time of this copy, so make rebuilds what changed even when the checkout is older than the last build's objects."""
    tree = work / 'mvdk'
    tree.mkdir(parents=True, exist_ok=True)
    excludes = [f'--exclude=/{name}' for name in ('extracted', 'rewritten', 'build', 'analysis', '.venv', 'tooling', 'MvDK_build')]
    run(['rsync', '-rlpD', '--checksum', '--exclude=__pycache__', '--exclude=*.app/', *excludes, f'{GAME_DIR}/', f'{tree}/'],
        logs / 'sources.log')
    say('1. fetching the pinned SPIRV-Cross sources')
    git_fetch(*SPIRV_CROSS, work / 'src/SPIRV-Cross', logs / 'fetch-spirv-cross.log')
    venv = work / 'venv'
    if not (venv / 'bin/python').exists():
        say('1. Python environment for the build tools')
        run([python, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', tree / 'requirements.txt'], logs / 'pip.log')
    probe = subprocess.run([venv / 'bin/python', '-c', 'import compression.zstd'], capture_output=True)
    if probe.returncode:
        raise BuildError(f'{venv}: needs Python 3.14; start the build with python3.14, or delete the venv folder made by an older Python')
    link(tree / '.venv', venv)
    for name in ('analysis', 'build'):
        (tree / name).mkdir(exist_ok=True)
    return tree


# ---- 2. code ----
def step_code(tree, game, keys, logs, update=None):
    py = tree / '.venv/bin/python'
    source = update or game   # the code of 1.0.1 is the update's
    exefs = tree / f'extracted/{VERSION}/exefs'
    want = EXPECTED['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'   # extract_romfs.py reads the game file named there
    try:
        same_source = json.loads(manifest.read_text()).get('source') == str(source)
    except (OSError, ValueError):
        same_source = False
    if not same_source or not all((exefs / n).is_file() and sha256(exefs / n) == h for n, h in want.items()):
        say('2. code: extracting the ExeFS from the ' + ('update' if update else 'game file'))
        if exefs.exists():
            shutil.rmtree(exefs)
        manifest.unlink(missing_ok=True)
        run([py, 'scripts/extract_exefs.py', source, '--keys', keys, '--output', exefs, '--title-id', EXPECTED['title_id'],
             *(['--update'] if update else [])],
            logs / 'extract-exefs.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not the supported version')
        if n == 'main.npdm' and sha256(exefs / n) != h:   # a dump tool may sign it again; the game does not use it
            say('2. code: main.npdm differs from the reference dump (signed again by a dump tool?); the game does not use it')
            continue
        check(f'ExeFS {n}', sha256(exefs / n), h)
    if not (tree / f'extracted/{VERSION}/elf/main-inventory.json').exists():
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{VERSION}/elf'], logs / 'prepare-modules.log', cwd=tree)
    out = tree / f'rewritten/{VERSION}'
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    for name in sorted(MODULES, key=lambda n: n == 'main'):   # main last: the big one
        image, h = out / f'{name}.patched.bin', EXPECTED['rewritten_sha256'][f'{name}.patched.bin']
        if image.is_file() and sha256(image) == h:
            continue
        say(f'2. code: rewriting {name} for the Mac' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite-{name}.log', cwd=tree, env=env)
        check(f'rewritten {name}', sha256(image), h)
    # The loader binds itself to these images (scripts/prepare_runtime.py reads this report).
    report = dict(format=1, status='pass', rom_version=VERSION, scope=f'hash match with the {EXPECTED["display_version"]} rewrite',
                  module_file_sha256={**{f'extracted/{VERSION}/exefs/{n}': want[n] for n in MODULES},
                                      **{f'rewritten/{VERSION}/{n}.patched.bin': EXPECTED['rewritten_sha256'][f'{n}.patched.bin'] for n in MODULES}})
    path = tree / f'analysis/{VERSION}-port/phase12-validation.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(report, indent=2) + '\n'
    if not path.is_file() or path.read_text() != text:   # unchanged: make keeps the tables
        path.write_text(text)


# ---- 3. data ----
def romfs_valid(romfs):
    try:
        index = json.loads((romfs / 'index.json').read_text())
        return index['romfs_sha256'] == EXPECTED['romfs']['sha256'] and (romfs / 'romfs.bin').stat().st_size == EXPECTED['romfs']['size']
    except (OSError, ValueError, KeyError):
        return False


def fonts_valid(folder):
    """scripts/extract_shared_fonts.py finished: six fonts, each <type>-<name>.bfttf with the SHA-256 the manifest names."""
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return (manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6))
                and all(sha256(folder / f"{f['type']}-{f['name']}.bfttf") == f['sha256'] for f in manifest['fonts']))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def step_data(tree, game, keys, logs, firmware=None, shared_fonts=None, update=None):
    py = tree / '.venv/bin/python'
    romfs = tree / f'extracted/{VERSION}/romfs'
    if not romfs_valid(romfs):
        say(f"3. data: extracting and verifying the RomFS ({EXPECTED['romfs']['size'] / 1e9:.1f} GB, a minute or two)"
            + (', the base game\'s merged with the update\'s' if update else ''))
        if romfs.exists():
            shutil.rmtree(romfs)
        if update:
            run([py, 'scripts/merge_romfs.py', game, update, '--keys', keys, '--title', EXPECTED['title_id'], '--output', romfs],
                logs / 'merge-romfs.log', cwd=tree)
        else:
            run([py, 'scripts/extract_romfs.py', '--keys', keys, '--manifest', romfs.parent / 'extraction-manifest.json',
                 '--output', romfs], logs / 'extract-romfs.log', cwd=tree)
        check('RomFS', json.loads((romfs / 'index.json').read_text())['romfs_sha256'], EXPECTED['romfs']['sha256'])
    fonts = tree / 'extracted/firmware/shared-fonts'
    if shared_fonts and shared_fonts.resolve() != fonts.resolve():
        if not fonts_valid(shared_fonts):
            raise BuildError(f'{shared_fonts}: not a verified font folder (manifest.json and the six .bfttf files it names)')
        if not fonts_valid(fonts) or (shared_fonts / 'manifest.json').read_bytes() != (fonts / 'manifest.json').read_bytes():
            if fonts.exists():
                shutil.rmtree(fonts)
            copy_tree(shared_fonts, fonts)
    elif not fonts_valid(fonts):
        if not firmware and input_kind(game) != 'xci':
            raise BuildError('an NSP carries no system fonts: pass --firmware <unpacked firmware NCA folder> or '
                             '--shared-fonts <a verified font folder>')
        say('3. data: the six system fonts' + ('' if firmware else " from the card image's system update partition"))
        if fonts.exists():
            shutil.rmtree(fonts)
        source_args = ['--firmware', firmware] if firmware else ['--xci', game]
        run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source_args, '--output', fonts], logs / 'shared-fonts.log', cwd=tree)
    if not fonts_valid(fonts):
        raise BuildError(f'the system fonts in {fonts} do not check out (see {logs / "shared-fonts.log"})')


# ---- 4. binary, launcher, icon ----
def build_spirv_cross(tree, logs):
    """The spirv-cross program with clang (the sources and flags of SPIRV-Cross's CMake Release build of it)."""
    src, out = tree.parent / 'src/SPIRV-Cross', tree / 'build/spirv-cross'
    if (out / 'spirv-cross').is_file():
        return
    say('4. app: building SPIRV-Cross with clang')
    objs = out / 'obj'
    objs.mkdir(parents=True, exist_ok=True)

    def compile_one(name):
        run(['xcrun', 'clang++', *SPIRV_CROSS_FLAGS, '-c', src / name, '-o', objs / (name + '.o')], logs / 'spirv-cross' / (name + '.log'))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(compile_one, SPIRV_CROSS_SOURCES))
    run(['xcrun', 'clang++', '-arch', 'arm64', *[objs / (n + '.o') for n in SPIRV_CROSS_SOURCES], '-o', out / 'spirv-cross'],
        logs / 'spirv-cross' / 'link.log')


def step_binary(tree, game, keys, logs):
    say('4. app: the game binary and the launcher')
    run(['make', f'-j{os.cpu_count() or 4}', f'VERSION={VERSION}', 'runtime'], logs / 'runtime.log', cwd=tree)
    # the launcher is per version (runtime/game_version.h: the version it checks, the folder it starts, the backup of saves)
    defines = ['-DGAME_MVDK=1', '-DGAME_WONDER=1']
    run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Ithird_party/d2r_gfx/gfx/nvn', '-Iruntime', *defines,
         'app/launcher.m', '-framework', 'AppKit', '-o', 'build/app-launcher'], logs / 'launcher.log', cwd=tree)
    icon = tree / 'build/AppIcon.icns'
    if not icon.exists():   # the game's own icon from its control data; optional
        try:
            run([tree / '.venv/bin/python', 'scripts/extract_app_icon.py', game, '--keys', keys, '--title', TITLE_ID,
                 '--output', icon], logs / 'app-icon.log', cwd=tree)
        except BuildError as e:
            say(f'   no app icon, the app gets the default one ({str(e).splitlines()[0]})')


# ---- 5. the app ----
def data_bundle(tree, logs):
    """The RomFS image as a bundle of its own, signed once and kept in the work folder (build/data-bundles): the app
    carries a clone of it in Contents/Helpers and its seal records only the bundle's signature, so a packaging does not
    hash the image again. The stamp names the image (size, modification time and the SHA-256 the extraction verified).
    The game opens the image through a link at extracted/<version>/romfs/romfs.bin and checks it itself."""
    image = tree / f'extracted/{VERSION}/romfs/romfs.bin'
    bundle = tree / f'build/data-bundles/GameData-{VERSION}.bundle'
    stamp = bundle.with_name(bundle.name + '.json')
    st = image.stat()
    want = dict(format=2, size=st.st_size, mtime_ns=st.st_mtime_ns,
                sha256=json.loads((image.parent / 'index.json').read_text()).get('romfs_sha256'))
    try:
        if (bundle / 'Contents/_CodeSignature/CodeResources').is_file() and json.loads(stamp.read_text()) == want:
            return bundle
    except (OSError, ValueError):
        pass
    say('5. app: signing the game data once (later builds reuse it)')
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
    RomFS check (keyed by them) still matches and the first start does not hash the image again."""
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


def step_app(tree, out, logs):
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        remove_tree(tmp)
    res = tmp / 'Contents/Resources/totk'   # the launcher's game folder (app/launcher.m)
    clone(tree / 'build/mvdk-native', res / 'build/totk-native')
    files = ['build/spirv-cross/spirv-cross', f'extracted/{VERSION}/romfs/runtime-index.bin']   # romfs.bin: data_bundle
    files += [f'rewritten/{VERSION}/{m}.patched.bin' for m in MODULES]
    for rel in files:
        clone(tree / rel, res / rel)
    clone(tree / 'prebuilt/macos-arm64/totk_xlate', res / 'build/shader-service/totk_xlate')   # where totk_shader_rt.m looks for it
    copy_tree(tree / 'extracted/firmware/shared-fonts', res / 'extracted/firmware/shared-fonts')
    bundle = data_bundle(tree, logs)   # after the copies above: signing it is slow
    copy_tree(bundle, tmp / 'Contents/Helpers' / bundle.name)
    link_path = res / f'extracted/{VERSION}/romfs/romfs.bin'
    link_path.parent.mkdir(parents=True, exist_ok=True)
    link_path.symlink_to(os.path.relpath(tmp / 'Contents/Helpers' / bundle.name / 'Contents/Resources/romfs.bin', link_path.parent))
    clone(tree / 'build/app-launcher', tmp / f'Contents/MacOS/{EXECUTABLE}')
    info = dict(CFBundleDevelopmentRegion='en', CFBundleExecutable=EXECUTABLE, CFBundleIdentifier=BUNDLE_ID, CFBundleName=APP_NAME,
                CFBundleDisplayName=APP_NAME, CFBundlePackageType='APPL', CFBundleShortVersionString=EXPECTED['display_version'],
                CFBundleVersion=str(EXPECTED['version'] + 1), LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSApplicationCategoryType='public.app-category.puzzle-games', LSSupportsGameMode=True,
                GCSupportsControllerUserInteraction=True)
    icon = tree / 'build/AppIcon.icns'
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
            say('5. app: the new app is ready; waiting for the running game to close before replacing it')
            notified = time.monotonic()
        time.sleep(2)
    if install_app(tmp, out):
        say('5. app: the game data is unchanged; kept the installed copy, so the first start need not check it again')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', type=Path, required=True, help='the base game (NSP, or XCI)')
    ap.add_argument('--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('--update', type=Path, required=True, help='the 1.0.1 update NSP (0100B99019412800)')
    ap.add_argument('--work', type=Path, help='work folder (default: MvDK_build next to the game file)')
    ap.add_argument('--out', type=Path, help='the app to write (default: next to the game file)')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='an unpacked firmware NCA folder with the six system fonts (needed for an NSP)')
    fonts.add_argument('--shared-fonts', type=Path, help='a font folder an earlier build verified (manifest.json + six .bfttf)')
    a = ap.parse_args(argv)
    t0 = time.time()
    game, keys = (p.expanduser().resolve() for p in (a.game, a.keys))
    update = a.update and a.update.expanduser().resolve()
    for p in (game, keys, *([update] if update else [])):
        if not p.is_file():
            raise BuildError(f'no such file: {p}')
    input_kind(game)
    if update and input_kind(update) != 'nsp':
        raise BuildError(f'{update}: the 1.0.1 update is an NSP')
    work = a.work.expanduser().resolve() if a.work else game.parent / 'MvDK_build'
    out = a.out.expanduser().resolve() if a.out else game.parent / f'{APP_NAME}.app'
    if out.suffix != '.app':
        raise BuildError(f'the output must end in .app: {out}')
    missing = [p for p in HOMEBREW if not (Path('/opt/homebrew/opt') / p / 'lib').is_dir()]
    if missing:
        raise BuildError(f'Homebrew packages missing: brew install {" ".join(missing)}')
    work.mkdir(parents=True, exist_ok=True)
    logs = work / 'logs/build'
    if shutil.disk_usage(work).free / 2**30 < 10 and not romfs_valid(work / f'mvdk/extracted/{VERSION}/romfs'):
        raise BuildError('needs about 10 GB free for the work folder (the app itself is an APFS clone of it on the same volume)')
    say(f'{APP_NAME} {EXPECTED["display_version"]}; work folder {work}')
    tree = prepare_work(work, sys.executable, logs)
    step_code(tree, game, keys, logs, update=update)
    step_data(tree, game, keys, logs, firmware=a.firmware and a.firmware.expanduser().resolve(),
              shared_fonts=a.shared_fonts and a.shared_fonts.expanduser().resolve(), update=update)
    build_spirv_cross(tree, logs)
    step_binary(tree, game, keys, logs)
    say(f'5. app: assembling {out}')
    step_app(tree, out, logs)
    run([LSREGISTER, '-f', out], logs / 'register-app.log')
    say('5. app: macOS checks the app once (seconds when this app folder was checked before, a few minutes for a new one)')
    r = subprocess.run([out / f'Contents/MacOS/{EXECUTABLE}', '--check'], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
    say('Open it from Finder. The first start verifies the game data. There is no startup shader set for this game yet: '
        'each shader is translated the first time the game uses it (a short stutter) and cached for later starts. Saves, '
        f'the shader cache and the log go to ~/Library/Application Support/MacNativeGames/{DATA_FOLDER} and '
        '~/Library/Logs/MacNativeGames/mvdk.log.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
