#!/usr/bin/env python3
"""Build Ori and the Blind Forest: Definitive Edition 1.0.1 for Apple silicon from your own game file, in one command.

    cd games/ori
    python3.14 scripts/build_ori.py --game <card image or base game> -k <prod.keys> [--update <1.0.1 update.nsp>]
                                    [--firmware <NCA folder>] [--out <folder>]

Steps (a second run reuses every finished one):
  1. code: the 1.0.1 ExeFS (from the card image, which holds the update, or from --update), checked against
     assets/expected-v101.json (hashes only), then every module rewritten for Apple silicon (third_party/d2r)
  2. data: the RomFS merged with the update, the system shared fonts (from the XCI, or --firmware for an NSP)
  3. runtime: make runtime (the native host, the NVN-on-Metal layer, the shader translator and SPIRV-Cross)
  4. app: "Ori and the Blind Forest.app" next to the game file (or in --out), which carries everything it runs with
Game data stays in this folder (extracted/, rewritten/, build/: git ignores them) and in the app; never share either.
Saves, the shader cache and the log live in ~/Library/Application Support/MacNativeGames/Ori and
~/Library/Logs/MacNativeGames/ori.log; a build reads the shader cache (see make_seed) and never changes it.
"""
import argparse, hashlib, json, os, plistlib, re, shutil, subprocess, sys, time
from pathlib import Path

ORI = Path(__file__).resolve().parents[1]
TITLE = '010061D00DB74000'
V = 'v101'
EXPECTED = json.loads((ORI / 'assets/expected-v101.json').read_text())
APP_NAME = 'Ori and the Blind Forest'
DATA = Path.home() / 'Library/Application Support/MacNativeGames/Ori'


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def run(cmd, log, env=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w') as f:
        code = subprocess.run([str(c) for c in cmd], cwd=ORI, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
    if code:
        tail = '\n'.join(log.read_text(errors='replace').splitlines()[-20:])
        raise BuildError(f'{Path(str(cmd[1] if Path(str(cmd[0])).name.startswith("python") else cmd[0])).name} failed '
                         f'(exit {code}); full output: {log}\n{tail}')


def venv():
    """This folder's Python environment (requirements.txt), made once."""
    py = ORI / '.venv/bin/python'
    if not py.exists():
        say('1/4 code: installing the Python packages the scripts need (once)')
        subprocess.run([sys.executable, '-m', 'venv', ORI / '.venv'], check=True)
        subprocess.run([py, '-m', 'pip', 'install', '-q', '-r', ORI / 'requirements.txt'], check=True)
    return py


def card_update(game, update, keys):
    """The file the 1.0.1 code and data come from: --update, or the game file itself when it is a card image that holds
    the update (an update Program NCA, with a BKTR section). Checks the base game is Ori."""
    sys.path.insert(0, str(ORI / 'scripts'))
    from extract_exefs import load_keys, container_entries, nca_inventory, is_bktr
    k = load_keys(keys)

    def programs(path):
        with path.open('rb') as f:
            _, entries = container_entries(f, path.stat().st_size)
            return [(r['title_id'].upper(), is_bktr(h)) for r, h in nca_inventory(f, entries, k) if r['content_type'] == 0]
    found = programs(game)
    if TITLE not in {t for t, _ in found}:
        raise BuildError(f'{game.name} is not Ori and the Blind Forest: Definitive Edition ({TITLE}); it holds {sorted({t for t, _ in found})}')
    if update:
        return update
    if any(bktr for _, bktr in found):
        return game
    raise BuildError(f'{game.name} holds the base game only: give the 1.0.1 update with --update')


def stale_rewrite():
    record = ORI / f'rewritten/{V}/build_src.sha256'
    return record.exists() and record.read_text().split()[0] != hashlib.sha256((ORI / 'third_party/d2r/patch_nso.py').read_bytes()).hexdigest()


# What the app carries, from this folder: the runtime and its translator tools (found next to the executable,
# build/..), the rewritten code, the game data, main's ExeFS (TOTK_PCHTXT) and the fonts.
APP_PARTS = ('build/ori-native', 'build/shader-service', 'build/spirv-cross/spirv-cross', f'rewritten/{V}',
             f'extracted/{V}/romfs', f'extracted/{V}/exefs', 'extracted/firmware/shared-fonts')
TITLE_ROWS = 1000   # warm-list rows the first frame waits for: the start, the title screen and its menu
SEED_FILE = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib))$')   # totk_shader_seed.h


def clone(src, dst):
    """An APFS clone when on the same volume (no extra space), else a copy."""
    if subprocess.run(['cp', '-Rc', src, dst], capture_output=True).returncode:
        subprocess.run(['cp', '-R', src, dst], check=True)


def make_seed(cache, lists, out):
    """The app's prepared shaders (build/shader-seed, totk_shader_seed.h): the translations your own play collected in
    the shader cache and the pipelines it built, so a fresh install (another Mac) imports them on its first start and
    builds every known pipeline before the first frame instead of translating while the game plays. Nothing when
    the game has not been played yet."""
    chain = re.search(r'#define DMSH_CHAIN "([^"]+)"', (ORI / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text())[1]
    out.mkdir(parents=True)
    files = {}
    for f in sorted(cache.iterdir()):
        if SEED_FILE.match(f.name):
            clone(f, out / f.name)
            files[f.name] = hashlib.sha256((out / f.name).read_bytes()).hexdigest()
    rows, seen = [], set()
    for path in lists:   # every pipeline row once, in the order the game met them
        if path.exists():
            for r in path.read_text().splitlines():
                if r.strip() and r not in seen: seen.add(r); rows.append(r)
    (out / 'pso_manifest.tsv').write_text('\n'.join(rows) + '\n')
    # The first frame waits for the first warm list only (nvn_mtl.m: pso_title.tsv beside the executable).
    (out.parent / 'pso_title.tsv').write_text('\n'.join(rows[:TITLE_ROWS]) + '\n')
    files['pso_manifest.tsv'] = hashlib.sha256((out / 'pso_manifest.tsv').read_bytes()).hexdigest()
    shaders = sum(name.endswith('_b.metal') for name in files)
    (out / 'seed-manifest.json').write_text(json.dumps({'chain': chain, 'schema': 1, 'shaders': shaders, 'files': files}) + '\n')
    return shaders, len(rows)


def make_shader_libs(seed, cache, out):
    """The seed's shaders compiled ahead of time for the GPU with Xcode's Metal toolchain: build/shader-libs/<SHA-256
    of the MSL>.metallib, which nvn_mtl.m shader_lib_prebuilt loads instead of compiling the source. Same option as the
    runtime (safe math). Kept in cache (build/shader-libs-cache), so building again compiles only new shaders."""
    import concurrent.futures
    found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True)
    metal = found.stdout.strip()
    if found.returncode or not metal or subprocess.run([metal, '--version'], capture_output=True).returncode:
        say('4/4 app: no Metal toolchain (xcodebuild -downloadComponent MetalToolchain): the shaders are compiled on the first start instead')
        return 0
    files = json.loads((seed / 'seed-manifest.json').read_text())['files']
    todo = {digest: seed / name for name, digest in files.items() if name.endswith('_b.metal')}
    cache.mkdir(parents=True, exist_ok=True)
    missing = {d: s for d, s in todo.items() if not (cache / f'{d}.metallib').is_file()}
    say(f'4/4 app: compiling {len(missing)} of {len(todo)} shaders for the GPU ahead of time')

    def compile_one(item):
        digest, source = item
        partial = cache / f'{digest}.partial'
        if subprocess.run([metal, '-fmetal-math-mode=safe', '-o', partial, source], capture_output=True).returncode:
            partial.unlink(missing_ok=True); return source.name
        partial.replace(cache / f'{digest}.metallib')
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        failed = [f for f in pool.map(compile_one, missing.items()) if f]
    if failed: say(f'   {len(failed)} shaders did not compile ahead of time; they are compiled when first used')
    out.mkdir(parents=True, exist_ok=True)
    libs = [cache / f'{d}.metallib' for d in todo if (cache / f'{d}.metallib').is_file()]
    for lib in libs: clone(lib, out / lib.name)
    return len(libs)


def make_app(app, icon, version):
    if app.exists():
        shutil.rmtree(app)
    game = app / 'Contents/Resources/game'
    for part in APP_PARTS:
        (game / part).parent.mkdir(parents=True, exist_ok=True)
        clone(ORI / part, game / part)
    cache = DATA / 'shader-cache'
    if cache.is_dir():
        shaders, pipelines = make_seed(cache, [DATA / 'profile/pso_user.tsv', cache / 'pso_collected.tsv'], game / 'build/shader-seed')
        say(f'4/4 app: {shaders} shaders and {pipelines} pipelines from your own play go into the app')
        libs = make_shader_libs(game / 'build/shader-seed', ORI / 'build/shader-libs-cache', game / 'build/shader-libs')
        if libs: say(f'4/4 app: {libs} shaders compiled for the GPU go into the app')
    (app / 'Contents/MacOS').mkdir(parents=True)
    info = {'CFBundleExecutable': 'game', 'CFBundleIdentifier': 'org.macnativegames.ori', 'CFBundleName': APP_NAME,
            'CFBundleDisplayName': APP_NAME, 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': version,
            'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
            'LSApplicationCategoryType': 'public.app-category.action-games'}
    if icon.exists():
        shutil.copy(icon, app / 'Contents/Resources/AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    with open(app / 'Contents/Info.plist', 'wb') as f:
        plistlib.dump(info, f)
    log = '"$HOME/Library/Logs/MacNativeGames/ori.log"'
    data = '"$HOME/Library/Application Support/MacNativeGames/Ori"'
    script = '\n'.join([
        '#!/bin/zsh',
        'cd "${0:A:h}/../Resources/game" || exit 1',
        f'mkdir -p "$HOME/Library/Logs/MacNativeGames" {data}/profile {data}/shader-cache',
        f'[ -f {log} ] && mv -f {log} {log.replace(".log", ".previous.log")}',
        f'export TOTK_SLOT_OWNER=user TOTK_PROFILE_DIR={data}/profile TOTK_SHADER_CACHE={data}/shader-cache TOTK_ROMFS_PATCHES=0',
        # nothing is written inside the app: its signature stays sealed, and it runs from a read-only place too
        f'export TOTK_GAME_SLOT_DIR={data}/game-slot TOTK_SHARED_FONTS="$PWD/extracted/firmware/shared-fonts"',
        'export TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_BUDGET_MB=8000',
        # Unity renders 1920x1080 docked whatever the window; MetalFX scales it to the window (0 = bilinear)
        'export TOTK_MFX_UPSCALE="${TOTK_MFX_UPSCALE:-1}" TOTK_MFX_OUTPUT="${TOTK_MFX_OUTPUT:-1}"',
        f'exec build/ori-native rewritten/{V} boot > {log} 2>&1', ''])
    launcher = app / 'Contents/MacOS/game'
    launcher.write_text(script)
    launcher.chmod(0o755)
    say('4/4 app: signing the app (macOS hashes the game data once)')
    subprocess.run(['codesign', '--force', '--sign', '-', app], check=True, capture_output=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', type=Path, required=True, help='the card image (XCI, holding 1.0.1) or the base game')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='prod.keys from your own Switch')
    ap.add_argument('--update', type=Path, help='the 1.0.1 update (NSP), when the game file does not hold it')
    ap.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder for the system fonts (an NSP, or a card without a system update)')
    ap.add_argument('--out', type=Path, help='folder for the app (default: next to the game file)')
    a = ap.parse_args(argv)
    game, keys = a.game.expanduser().resolve(), a.keys.expanduser().resolve()
    update = a.update.expanduser().resolve() if a.update else None
    for path in (game, keys, update):
        if path and not path.is_file():
            raise BuildError(f'no such file: {path}')
    if not a.firmware and game.suffix.lower() != '.xci':
        raise BuildError('an NSP has no system fonts: give --firmware <unpacked firmware NCA folder>')
    py = venv()
    if Path(sys.prefix).resolve() != (ORI / '.venv').resolve():
        os.execv(py, [str(py), __file__, *argv])   # this script, with the packages
    t0 = time.time()
    source = card_update(game, update, keys)
    logs = ORI / 'build/logs'

    exefs = ORI / f'extracted/{V}/exefs'
    if not (exefs / 'main').exists():
        say('1/4 code: extracting the 1.0.1 ExeFS')
        shutil.rmtree(exefs, ignore_errors=True)
        run([py, 'scripts/extract_exefs.py', source, '--update', '--keys', keys, '--output', exefs], logs / 'extract_exefs.log')
    for name, sha in EXPECTED['exefs'].items():
        if hashlib.sha256((exefs / name).read_bytes()).hexdigest() != sha:
            raise BuildError(f'{name} is not Ori 1.0.1 ({EXPECTED["version"]} is the version this port runs); '
                             f'delete extracted/{V} and give the 1.0.1 update')
    if stale_rewrite():
        say('1/4 code: the rewriter changed since the last rewrite; rewriting again')
        shutil.rmtree(ORI / f'rewritten/{V}', ignore_errors=True)
    (ORI / f'rewritten/{V}').mkdir(parents=True, exist_ok=True)
    for m in EXPECTED['exefs']:
        if not (ORI / f'rewritten/{V}/{m}.patched.bin').exists():
            say(f'1/4 code: rewriting {m} for Apple silicon' + (' (a few minutes)' if m == 'main' else ''))
            run([py, 'third_party/d2r/patch_nso.py', exefs / m, f'rewritten/{V}', m], logs / f'rewrite-{m}.log',
                dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0'))

    romfs = ORI / f'extracted/{V}/romfs'
    if not (romfs / 'index.json').exists():
        say('2/4 data: merging the RomFS with the update and verifying it')
        shutil.rmtree(romfs, ignore_errors=True)
        run(['make', 'hactool'], logs / 'hactool.log')
        run([py, 'scripts/extract_update_romfs.py', game, source, '--keys', keys, '--hactool', 'tooling/hactool/hactool',
             '--output', romfs], logs / 'extract_romfs.log')
    fonts = ORI / 'extracted/firmware/shared-fonts'
    if not (fonts / 'manifest.json').exists():
        say('2/4 data: extracting the system shared fonts')
        shutil.rmtree(fonts, ignore_errors=True)
        try: run([py, 'scripts/extract_shared_fonts.py', '--keys', keys,
             *(['--firmware', a.firmware.expanduser().resolve()] if a.firmware else ['--xci', game]), '--output', fonts,
             '--inventory', ORI / 'build/firmware-inventory.json'],
            logs / 'shared_fonts.log')
        except BuildError as e:
            if a.firmware or 'missing from the card' not in str(e): raise
            raise BuildError('this card image carries no system update with the fonts: give --firmware <unpacked firmware NCA folder>')
    icon = ORI / 'build/AppIcon.icns'
    if not icon.exists():
        try:
            run([py, 'scripts/extract_app_icon.py', source, '--keys', keys, '--output', icon], logs / 'icon.log')
        except BuildError:
            say('   no icon in the control data: the app gets the default one')

    say('3/4 runtime: building the runtime')
    for stale in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h'):   # remade from this build's files
        (ORI / 'build' / stale).unlink(missing_ok=True)
    run(['make', '-j8', 'runtime'], logs / 'runtime.log')

    app = (a.out.expanduser().resolve() if a.out else game.parent) / f'{APP_NAME}.app'
    say(f'4/4 app: assembling {app}')
    make_app(app, icon, EXPECTED['version'])
    say(f'done in {(time.time() - t0) / 60:.0f} min: {app}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (BuildError, subprocess.CalledProcessError) as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
