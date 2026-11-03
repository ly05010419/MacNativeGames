#!/usr/bin/env python3
"""Build Kirby.app from your own dump of Kirby and the Forgotten Land 1.0.0 in one command.

    python3.14 games/kirby/scripts/build_kirby.py --game KIRBY.nsp --keys prod.keys
                                                  (--firmware DIR | --shared-fonts DIR) [--work Kirby_build] [--out Kirby.app]

The game file is the base game 1.0.0 (01004D300C5AE000; NSP or XCI: an XCI carries the system fonts, an NSP needs --firmware
or --shared-fonts). Updates are not supported yet. Steps (each is skipped when its verified output is already there, so a
stopped run can be started again):
 1. the Python environment (<work>/.venv, from requirements.txt) and the pinned SPIRV-Cross sources
 2. the ExeFS and its three modules (main, subsdk0, sdk) rewritten for the Mac with third_party/d2r/patch_nso.py
 3. the RomFS (6.1 GB, all six IVFC levels verified), its runtime index and the six system fonts
 4. the runtime tables, SPIRV-Cross, the game binary and the shader translator
 5. the game's ~27,000 shaders translated (scripts/prepare_shader_seed.py) and compiled to Metal libraries
    (scripts/precompile_cached_shaders.py), so play does not stop the first time it meets one (--no-shaders skips it)
 6. the app (scripts/package_native_app.py: every input checked against assets/expected-v1.0.0.json, then --check)

Every extracted and rewritten file is compared with assets/expected-v1.0.0.json, the files of the port author's own build.
Two are not compared byte for byte: main.npdm (a dump tool may sign it again; the game does not use it) and
build/runtime_tables.h (generated from the pinned rewrites; package_native_app.py checks that it binds them).
Everything this writes stays in the work folder (default: Kirby_build next to the game file): it holds game data, so never
share or commit it. The app keeps saves, the shader cache and its log in "Kirby Data" next to the app.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[1]
EXPECTED = json.loads((GAME_DIR / 'assets/expected-v1.0.0.json').read_text())
FILES = EXPECTED['files']
TITLE_ID = '01004D300C5AE000'
MODULES = ('subsdk0', 'sdk', 'main')   # main last: the big one
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')
HOMEBREW = ('opus', 'libvpx')   # linked statically by the Makefile


class BuildError(Exception):
    pass


T0 = time.time()


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def run(cmd, log, cwd=GAME_DIR, env=None):
    """Run a step with its output in a log file; print a line every minute so a long step does not look stuck."""
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
        t0, done = time.time(), threading.Event()

        def tick():
            while not done.wait(60):
                say(f'   ... still running ({(time.time() - t0) / 60:.0f} min)')
        threading.Thread(target=tick, daemon=True).start()
        code = p.wait()
        done.set()
    if code:
        tail = log.read_text(errors='replace').strip().splitlines()[-5:]
        raise BuildError(f'{Path(str(cmd[1] if len(cmd) > 1 else cmd[0])).name} failed (log: {log}):\n   ' + '\n   '.join(tail))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def pinned(work, rel):
    path = work / rel
    return path.is_file() and path.stat().st_size == FILES[rel]['size'] and sha256(path) == FILES[rel]['sha256']


def check(work, rel):
    if not pinned(work, rel):
        got = sha256(work / rel)[:16] if (work / rel).is_file() else 'missing'
        raise BuildError(f'{rel}: {got} is not the expected {FILES[rel]["sha256"][:16]}... '
                         '(is this Kirby and the Forgotten Land 1.0.0, and the dump complete?)')


def input_kind(game):
    with game.open('rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xci'
    raise BuildError(f'{game}: expected an XCI card image or an NSP')


# ---- 1. tools ----
def git_fetch(url, commit, dst, log):
    if (dst / '.git').is_dir() and subprocess.run(['git', '-C', dst, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit:
        return
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)


def prepare_tools(work, python, logs):
    missing = [name for name in HOMEBREW if not Path(f'/opt/homebrew/opt/{name}/lib').is_dir()]
    if missing:
        raise BuildError(f'missing Homebrew packages: brew install {" ".join(missing)}')
    venv = work / '.venv'
    if not (venv / 'bin/python').exists():
        say('1. Python environment for the build tools')
        run([python, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', GAME_DIR / 'requirements.txt'], logs / 'pip.log')
    say('1. fetching the pinned SPIRV-Cross sources')
    git_fetch(*SPIRV_CROSS, work / 'src/SPIRV-Cross', logs / 'fetch-spirv-cross.log')
    return venv / 'bin/python'


# ---- 2. code ----
def step_code(work, py, game, keys, logs):
    exefs = work / 'extracted/exefs'
    if not all(pinned(work, f'extracted/exefs/{n}') for n in ('rtld', *MODULES)):
        say('2. code: extracting the ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        (work / 'extracted/extraction-manifest.json').unlink(missing_ok=True)
        run([py, 'scripts/extract_exefs.py', game, '--keys', keys, '--title-id', TITLE_ID, '--output', exefs],
            logs / 'extract-exefs.log')
    for name in ('rtld', *MODULES):
        check(work, f'extracted/exefs/{name}')
    if not pinned(work, 'extracted/exefs/main.npdm'):
        say('2. code: main.npdm differs from the reference dump (signed again by a dump tool?); the game does not use it')
    out = work / 'rewritten'
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    for name in MODULES:
        outputs = [f'rewritten/{name}.{kind}' for kind in ('patched.bin', 'fixups.tsv', 'gates.tsv')]
        if all(pinned(work, rel) for rel in outputs):
            continue
        say(f'2. code: rewriting {name} for the Mac' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite-{name}.log', env=env)
        for rel in outputs:
            check(work, rel)


# ---- 3. data ----
def fonts_valid(folder):
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        return (manifest.get('status') == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6))
                and all(sha256(folder / f"{f['type']}-{f['name']}.bfttf") == f['sha256'] for f in manifest['fonts']))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def step_data(work, py, game, keys, logs, firmware=None, shared_fonts=None):
    romfs = work / 'romfs'
    if not pinned(work, 'romfs/romfs.bin'):
        say(f"3. data: extracting and verifying the RomFS ({FILES['romfs/romfs.bin']['size'] / 1e9:.1f} GB, a minute or two)")
        if romfs.exists():
            shutil.rmtree(romfs)
        run([py, 'scripts/extract_romfs.py', '--keys', keys, '--manifest', work / 'extracted/extraction-manifest.json',
             '--output', romfs], logs / 'extract-romfs.log')
        check(work, 'romfs/romfs.bin')
    if not (pinned(work, 'romfs/runtime-index.bin') and pinned(work, 'build/romfs_config.h')):
        run([py, 'scripts/prepare_romfs_backend.py', '--work', work], logs / 'romfs-backend.log')
    check(work, 'romfs/runtime-index.bin')
    check(work, 'build/romfs_config.h')
    fonts = work / 'fonts'
    if shared_fonts and shared_fonts.resolve() != fonts.resolve():
        if not fonts_valid(shared_fonts):
            raise BuildError(f'{shared_fonts}: not a verified font folder (manifest.json and the six .bfttf files it names)')
        if fonts.exists():
            shutil.rmtree(fonts)
        shutil.copytree(shared_fonts, fonts)
    elif not fonts_valid(fonts):
        if not firmware and input_kind(game) != 'xci':
            raise BuildError('an NSP carries no system fonts: pass --firmware <unpacked firmware NCA folder> or '
                             '--shared-fonts <a verified font folder>')
        say('3. data: the six system fonts' + ('' if firmware else " from the card image's system update partition"))
        if fonts.exists():
            shutil.rmtree(fonts)
        source_args = ['--firmware', firmware] if firmware else ['--xci', game]
        run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source_args, '--output', fonts], logs / 'shared-fonts.log')
    if not fonts_valid(fonts):
        raise BuildError(f'the system fonts in {fonts} do not check out (see {logs / "shared-fonts.log"})')
    run([py, 'scripts/prepare_shared_fonts.py', '--work', work], logs / 'shared-fonts-config.log')


# ---- 4. binary ----
def build_spirv_cross(work, logs):
    """The spirv-cross program with clang (the sources and flags of SPIRV-Cross's CMake Release build of it)."""
    src, out = work / 'src/SPIRV-Cross', work / 'build/spirv-cross'
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


def step_binary(work, py, game, keys, logs):
    say('4. app: the runtime tables, the game binary and the shader translator')
    run([py, 'scripts/prepare_runtime.py', '--work', work], logs / 'runtime-tables.log')
    build_spirv_cross(work, logs)
    translator = work / 'build/shader-service/totk_xlate'
    translator.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GAME_DIR / 'prebuilt/macos-arm64/totk_xlate', translator)
    run(['make', f'-j{os.cpu_count() or 4}', f'WORK={work}'], logs / 'runtime.log')
    icon = work / 'build/AppIcon.icns'
    if not icon.exists():   # the game's own icon from its control data; optional
        try:
            run([py, 'scripts/extract_app_icon.py', game, '--keys', keys, '--title', TITLE_ID, '--output', icon],
                logs / 'app-icon.log')
        except BuildError as e:
            say(f'   no app icon, the app gets the default one ({str(e).splitlines()[0]})')
    return icon if icon.is_file() else None


# ---- 5. shaders ----
def step_shaders(work, py, logs):
    """Every shader of the game translated (prepare_shader_seed.py) and compiled to Metal libraries
    (precompile_cached_shaders.py) now, instead of in play the first time a scene needs it."""
    say('5. shaders: translating the game\'s 27,000 shaders (about two minutes the first time)')
    run([py, 'scripts/prepare_shader_seed.py', '--work', work], logs / 'shader-seed.log')
    say('5. shaders: compiling them to Metal libraries (22,000, about half an hour the first time; later builds reuse them)')
    run([py, 'scripts/precompile_cached_shaders.py', '--cache', work / 'build/shader-seed', '--output', work / 'build/shader-libs',
         '--jobs', str(min(16, os.cpu_count() or 4)), '--flipped-only'], logs / 'shader-libs.log')


# ---- 6. the app ----
def step_app(work, py, out, icon, logs):
    if out.exists():
        raise BuildError(f'{out} already exists: quit the game and move it away (saves stay in "Kirby Data"), or pass --out')
    say(f'6. app: assembling and checking {out}')
    run([py, 'scripts/package_native_app.py', '--work', work, '--shared-fonts', work / 'fonts', '--output', out,
         *(['--icon', icon] if icon else [])], logs / 'package.log')


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', type=Path, required=True, help='Kirby and the Forgotten Land 1.0.0 (NSP or XCI)')
    parser.add_argument('--keys', type=Path, required=True, help='prod.keys from your own Switch')
    fonts = parser.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder (an NSP carries no system fonts)')
    fonts.add_argument('--shared-fonts', type=Path, help='a verified font folder from an earlier build (manifest.json and six .bfttf)')
    parser.add_argument('--work', type=Path, help='work folder (default: Kirby_build next to the game file)')
    parser.add_argument('--out', type=Path, help='the app (default: Kirby.app next to the game file)')
    parser.add_argument('--no-shaders', action='store_true',
                        help='skip step 5: a faster build whose game stops briefly the first time it meets each shader')
    args = parser.parse_args(argv)
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    for path in (game, keys):
        if not path.is_file():
            raise BuildError(f'not found: {path}')
    input_kind(game)
    work = (args.work.expanduser() if args.work else game.parent / 'Kirby_build').resolve()
    out = (args.out.expanduser() if args.out else game.parent / 'Kirby.app').absolute()
    logs = work / 'logs'
    work.mkdir(parents=True, exist_ok=True)
    say(f'Kirby and the Forgotten Land {EXPECTED["version"]}; work folder {work}')
    py = prepare_tools(work, Path(sys.executable), logs)
    step_code(work, py, game, keys, logs)
    step_data(work, py, game, keys, logs, firmware=args.firmware and args.firmware.expanduser().resolve(),
              shared_fonts=args.shared_fonts and args.shared_fonts.expanduser().resolve())
    icon = step_binary(work, py, game, keys, logs)
    if args.no_shaders:
        say('5. shaders: skipped (--no-shaders); the game translates and compiles each shader the first time it meets it')
    else:
        step_shaders(work, py, logs)
    step_app(work, py, out, icon, logs)
    say(f'done in {(time.time() - T0) / 60:.0f} min: {out}')
    say('Open it from Finder. Saves, the shader cache and the log (last-run.log) are in "Kirby Data" next to the app.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as error:
        say(f'build stopped: {error}')
        sys.exit(1)
