#!/usr/bin/env python3
"""Build Mario Kart World.app from your own copy of Mario Kart World 1.0.0 (Switch 2) in one command.

    python3.14 scripts/build_mkw.py --game "MKW.nsp" [--work MKW_build] [--out "Mario Kart World.app"]
                                    [--firmware DIR | --shared-fonts DIR]

The game file is an NSP of Mario Kart World 1.0.0 (0400C3F00006E000) dumped from your own console and decrypted with your
own keys (decrypted NCAs), the form scripts/extract_mkw.py reads and assets/expected-v1.0.0.json pins; decrypting during
the build is not supported yet.
Steps (each is skipped when its verified output is already there, so a stopped run can be started again):
 1. this folder copied into the work folder (<work>/mkw: the scripts write extracted/, rewritten/, analysis/ and build/
    next to themselves) and the Python environment (<work>/venv, from requirements.txt)
 2. the code (ExeFS) from the game file, verified, and its three modules (main, sdk, subsdk0) rewritten for the Mac, each
    checked against assets/expected-v1.0.0.json; the RomFS is indexed and verified (all six IVFC levels) where it lies
    in the NSP: the 23 GB image is never copied
 3. the system fonts, when --firmware or --shared-fonts is given (the game runs without them: it carries its own)
 4. the game binary, the shader translation chain (make shader-tools: SPIRV-Cross, the nxnative SM86 frontend and the
    Ampere service, from pinned sources; needs the .NET 10 SDK and network access once), the launcher
 5. the app: the NSP as a signed data bundle in Contents/Helpers (an APFS clone of your file: no extra space on the same
    volume; signed once, a few minutes for 23 GB), checked once by macOS

There is no startup shader set: each shader is translated the first time the game draws it (a short stutter), then
cached. Everything this writes stays in the work folder, which Git ignores: it holds game data, so never share or commit
it. The app keeps saves, the shader cache and its log in ~/Library/Application Support/MacNativeGames/MKW and
~/Library/Logs/MacNativeGames/mkw.log; the build never touches them.
"""
import argparse
from mkw_artifact_paths import outside_source
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
VERSION = 'mkw'   # runtime/game_version.h TOTK_GAME_DIR
EXPECTED = json.loads((GAME_DIR / 'assets/expected-v1.0.0.json').read_text())
APP_NAME, BUNDLE_ID, EXECUTABLE = 'Mario Kart World', 'org.macnativegames.mkw', 'MarioKartWorld'
DATA_FOLDER = 'MKW'   # runtime/game_version.h GAME_FOLDER
TITLE_ID = '0400c3f00006e000'
MODULES = ('main', 'sdk', 'subsdk0')
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
HOMEBREW = ('opus', 'libvpx')   # linked statically by the Makefile
SHADER_CHAIN = ('build/spirv-cross/spirv-cross', 'build/dmsh_xlate/dmsh_xlate', 'build/shader-service/ampere_service')


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


def git_fetch(url, commit, dst, log):
    if (dst / '.git').is_dir() and subprocess.run(['git', '-C', dst, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit:
        return
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)


# ---- 1. inputs, the work tree and the Python environment ----
def check_game(game):
    with game.open('rb') as f:
        if f.read(4) != b'PFS0':
            raise BuildError(f'{game}: expected the game NSP (PFS0)')


def prepare_work(work, python, logs):
    """<work>/mkw: this folder with the generated files inside it. Files are compared by content and a changed file gets
    the time of this copy, so make rebuilds what changed even when the checkout is older than the last build's objects."""
    tree = work / 'mkw'
    tree.mkdir(parents=True, exist_ok=True)
    excludes = [f'--exclude=/{name}' for name in ('extracted', 'rewritten', 'build', 'analysis', '.venv', 'MKW_build')]
    run(['rsync', '-rlpD', '--checksum', '--exclude=__pycache__', '--exclude=*.app/', *excludes, f'{GAME_DIR}/', f'{tree}/'],
        logs / 'sources.log')
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


# ---- 2. code and the RomFS index ----
def romfs_valid(romfs, game):
    """extract_mkw.py finished for this game file: index.json verified, romfs.source naming this NSP."""
    try:
        index = json.loads((romfs / 'index.json').read_text())
        offset, path = (romfs / 'romfs.source').read_text().strip().split(' ', 1)
        return (index.get('status') == 'pass' and index.get('romfs_sha256') == EXPECTED['romfs']['sha256']
                and index.get('romfs_size') == EXPECTED['romfs']['size'] and Path(path) == game)
    except (OSError, ValueError, KeyError):
        return False


def step_code(tree, game, logs):
    py = tree / '.venv/bin/python'
    exefs, romfs = tree / f'extracted/{VERSION}/exefs', tree / f'extracted/{VERSION}/romfs'
    want = EXPECTED['exefs_sha256']
    exefs_ok = all((exefs / n).is_file() and sha256(exefs / n) == h for n, h in want.items())
    if not exefs_ok or not romfs_valid(romfs, game):
        say('2. code and data: the ExeFS and the RomFS index from the NSP, verified in place (a few minutes: 23 GB are hashed)')
        for d in (exefs, romfs):
            if d.exists():
                shutil.rmtree(d)
        run([py, 'scripts/extract_mkw.py', game, '--title-id', TITLE_ID, '--exefs', exefs, '--romfs', romfs],
            logs / 'extract-mkw.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not the supported version')
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
        say(f'2. code: rewriting {name} for the Mac' + (' (the big one: a few minutes)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite-{name}.log', cwd=tree, env=env)
        check(f'rewritten {name}', sha256(image), h)
    # The loader binds itself to these images (scripts/prepare_runtime.py reads this report).
    report = dict(format=1, status='pass', rom_version=VERSION, scope=f'hash match with the {EXPECTED["display_version"]} rewrite',
                  module_file_sha256={**{f'extracted/{VERSION}/exefs/{n}': want[n] for n in MODULES},
                                      **{f'rewritten/{VERSION}/{n}.patched.bin': EXPECTED['rewritten_sha256'][f'{n}.patched.bin'] for n in MODULES}})
    path = tree / f'analysis/{VERSION}-port/phase12-validation.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(report, indent=2) + '\n'
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)


# ---- 3. system fonts (optional) ----
def fonts_valid(folder):
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return (manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6))
                and all(sha256(folder / f"{f['type']}-{f['name']}.bfttf") == f['sha256'] for f in manifest['fonts']))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def step_fonts(tree, logs, keys=None, firmware=None, shared_fonts=None):
    """Returns whether the build carries the six system fonts (extracted/firmware/shared-fonts)."""
    fonts = tree / 'extracted/firmware/shared-fonts'
    if shared_fonts and shared_fonts.resolve() != fonts.resolve():
        if not fonts_valid(shared_fonts):
            raise BuildError(f'{shared_fonts}: not a verified font folder (manifest.json and the six .bfttf files it names)')
        if fonts.exists():
            shutil.rmtree(fonts)
        copy_tree(shared_fonts, fonts)
    elif firmware and not fonts_valid(fonts):
        if not keys:
            raise BuildError('--firmware needs --keys (the firmware NCAs are encrypted)')
        say('3. data: the six system fonts from the firmware')
        if fonts.exists():
            shutil.rmtree(fonts)
        run([tree / '.venv/bin/python', 'scripts/extract_shared_fonts.py', '--keys', keys, '--firmware', firmware, '--output', fonts],
            logs / 'shared-fonts.log', cwd=tree)
    return fonts_valid(fonts)


# ---- 4. binary, game data tables, shader chain, launcher ----
def step_binary(tree, logs, have_fonts):
    py = tree / '.venv/bin/python'
    say('4. app: the game binary')
    run(['make', f'-j{os.cpu_count() or 4}', f'VERSION={VERSION}', 'binary'], logs / 'binary.log', cwd=tree)
    say('4. app: the game data tables')
    run([py, 'scripts/prepare_runtime.py', '--version', VERSION], logs / 'prepare-runtime.log', cwd=tree)
    run([py, 'scripts/prepare_romfs_backend.py', '--version', VERSION], logs / 'prepare-romfs.log', cwd=tree)
    fonts_txt = tree / f'build/game-data/{VERSION}/shared-fonts.txt'
    if have_fonts:
        run([py, 'scripts/prepare_shared_fonts.py', '--version', VERSION], logs / 'prepare-fonts.log', cwd=tree)
    else:
        fonts_txt.write_text('')   # no system fonts: the game uses its own
    if not all((tree / p).is_file() for p in SHADER_CHAIN):
        say('4. app: the shader translation chain from pinned sources (the first time: several minutes, needs the .NET 10 SDK)')
    run(['make', 'shader-tools'], logs / 'shader-tools.log', cwd=tree)
    for p in SHADER_CHAIN:
        if not os.access(tree / p, os.X_OK):
            raise BuildError(f'{p} was not built (see {logs / "shader-tools.log"})')
    run(['xcrun', 'clang', '-mmacosx-version-min=26.0', '-fobjc-arc', '-O2', '-Wall', '-Ithird_party/d2r_gfx/gfx/nvn', '-Iruntime', '-DGAME_MKW=1',
         'app/launcher.m', '-framework', 'AppKit', '-framework', 'GameController', '-framework', 'IOKit',
         '-o', 'build/app-launcher'], logs / 'launcher.log', cwd=tree)


# ---- 5. the app ----
def data_bundle(tree, game, logs):
    """The game's NSP as a bundle of its own, signed once and kept in the work folder (build/data-bundles): the app
    carries a clone of it in Contents/Helpers and its seal records only the bundle's signature, so a packaging does not
    hash 23 GB again. The runtime reads the RomFS in place from it (romfs.source) and checks it itself."""
    bundle = tree / 'build/data-bundles/GameData-mkw.bundle'
    stamp = bundle.with_name(bundle.name + '.json')
    st = game.stat()
    want = dict(format=1, size=st.st_size, mtime_ns=st.st_mtime_ns, romfs_sha256=EXPECTED['romfs']['sha256'])
    try:
        if (bundle / 'Contents/_CodeSignature/CodeResources').is_file() and json.loads(stamp.read_text()) == want:
            return bundle
    except (OSError, ValueError):
        pass
    say('5. app: signing the game data once (23 GB: a few minutes; later builds reuse it)')
    tmp = bundle.with_name(bundle.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    clone(game, tmp / 'Contents/Resources/game.nsp')
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier=f'{BUNDLE_ID}.data', CFBundleName='Game data', CFBundlePackageType='BNDL',
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
    image = 'Contents/Resources/game.nsp'
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


def step_app(tree, game, out, logs, have_fonts):
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        remove_tree(tmp)
    res = tmp / 'Contents/Resources/totk'   # the launcher's game folder (app/launcher.m)
    clone(tree / 'build/mkw-native', res / 'build/totk-native')
    files = ['build/shader-tools.json', 'build/spirv-cross/spirv-cross', f'extracted/{VERSION}/romfs/runtime-index.bin', f'extracted/{VERSION}/romfs/index.json',
             'tools/wrap_mesh_stage.py', 'tools/dmsh_meshstage.py', 'tools/prepare_ampere_jobs.py']   # the mesh-stage wrapper and its imports
    files += [f'rewritten/{VERSION}/{m}.patched.bin' for m in MODULES]
    files += [f'build/game-data/{VERSION}/{name}' for name in ('runtime-tables.bin', 'rewriter.txt', 'romfs-config.txt', 'shared-fonts.txt')]
    for rel in files:
        clone(tree / rel, res / rel)
    for d in ('build/dmsh_xlate', 'build/shader-service'):   # self-contained .NET publishes (make shader-tools)
        copy_tree(tree / d, res / d)
    if have_fonts:
        copy_tree(tree / 'extracted/firmware/shared-fonts', res / 'extracted/firmware/shared-fonts')
    bundle = data_bundle(tree, game, logs)   # after the copies above: signing it is slow
    copy_tree(bundle, tmp / 'Contents/Helpers' / bundle.name)
    romfs_dir = res / f'extracted/{VERSION}/romfs'
    offset = (tree / f'extracted/{VERSION}/romfs/romfs.source').read_text().split(' ', 1)[0]
    rel = os.path.relpath(tmp / 'Contents/Helpers' / bundle.name / 'Contents/Resources/game.nsp', romfs_dir)
    (romfs_dir / 'romfs.source').write_text(f'{offset} {rel}\n')   # runtime/romfs.c: relative to the RomFS folder
    clone(tree / 'build/app-launcher', tmp / f'Contents/MacOS/{EXECUTABLE}')
    info = dict(CFBundleDevelopmentRegion='en', CFBundleExecutable=EXECUTABLE, CFBundleIdentifier=BUNDLE_ID, CFBundleName=APP_NAME,
                CFBundleDisplayName=APP_NAME, CFBundlePackageType='APPL', CFBundleShortVersionString=EXPECTED['display_version'],
                CFBundleVersion='1', LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSApplicationCategoryType='public.app-category.racing-games', LSSupportsGameMode=True,
                GCSupportsControllerUserInteraction=True)
    commit = subprocess.run(['git', '-C', GAME_DIR, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if commit:
        info['MNGSourceCommit'] = commit
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
        say('5. app: the game data is unchanged; kept the installed copy')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', type=Path, required=True, help='your Mario Kart World 1.0.0 NSP, decrypted with your own keys')
    ap.add_argument('--work', type=Path, help='work folder (default: MKW_build next to the game file)')
    ap.add_argument('--out', type=Path, help='the app to write (default: next to the game file)')
    ap.add_argument('--keys', type=Path, help='prod.keys: only needed with --firmware')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='optional: an unpacked firmware NCA folder with the six system fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='optional: a font folder an earlier build verified (manifest.json + six .bfttf)')
    a = ap.parse_args(argv)
    t0 = time.time()
    game = a.game.expanduser().resolve()
    if not game.is_file():
        raise BuildError(f'no such file: {game}')
    check_game(game)
    keys = a.keys and a.keys.expanduser().resolve()
    work = a.work.expanduser().resolve() if a.work else game.parent / 'MKW_build'
    out = a.out.expanduser().resolve() if a.out else game.parent / f'{APP_NAME}.app'
    work, out = outside_source(work), outside_source(out)
    if out.suffix != '.app':
        raise BuildError(f'the output must end in .app: {out}')
    missing = [p for p in HOMEBREW if not (Path('/opt/homebrew/opt') / p / 'lib').is_dir()]
    if missing:
        raise BuildError(f'Homebrew packages missing: brew install {" ".join(missing)}')
    work.mkdir(parents=True, exist_ok=True)
    if os.stat(work).st_dev != os.stat(game).st_dev or os.stat(out.parent).st_dev != os.stat(game).st_dev:
        raise BuildError('the work folder and the app must be on the same volume as the game file (the app clones the 23 GB NSP)')
    logs = work / 'logs/build'
    if shutil.disk_usage(work).free / 2**30 < 6:
        raise BuildError('needs about 6 GB free for the work folder (the NSP itself is cloned, not copied)')
    say(f'{APP_NAME} {EXPECTED["display_version"]}; work folder {work}')
    tree = prepare_work(work, sys.executable, logs)
    step_code(tree, game, logs)
    have_fonts = step_fonts(tree, logs, keys=keys, firmware=a.firmware and a.firmware.expanduser().resolve(),
                            shared_fonts=a.shared_fonts and a.shared_fonts.expanduser().resolve())
    step_binary(tree, logs, have_fonts)
    say(f'5. app: assembling {out}')
    step_app(tree, game, out, logs, have_fonts)
    run([LSREGISTER, '-f', out], logs / 'register-app.log')
    say('5. app: macOS checks the app once (seconds when this app folder was checked before, a few minutes for a new one)')
    r = subprocess.run([out / f'Contents/MacOS/{EXECUTABLE}', '--check'], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
    say('Open it from Finder. The first start verifies the game data (23 GB). There is no startup shader set yet: each '
        'shader is translated the first time the game uses it (a short stutter) and cached for later starts. Saves, the '
        f'shader cache and the log go to ~/Library/Application Support/MacNativeGames/{DATA_FOLDER} and '
        '~/Library/Logs/MacNativeGames/mkw.log.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (BuildError, ValueError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
