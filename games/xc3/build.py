#!/usr/bin/env python3
"""Build the Xenoblade Chronicles 3 Mac app in one command: the steps of README.md, each checked against
assets/expected-*.json, with the work folder reused across runs.

    python3.14 build.py <base.nsp> -k <prod.keys> [--update <2.2.1 update.nsp>] [--dlc <add-on.nsp> ...]
                        [--firmware DIR | --shared-fonts DIR] [-o OUT] [--work DIR]

Without --update the game is 1.0.0. With the 2.2.1 update the code comes from the update and the RomFS is the base's
merged with the update's; the add-ons (--dlc, the four Expansion Pass volumes) need 2.2.1. The app carries everything
the game reads (make app), so it plays without the work folder: the work folder can live on another drive and is needed
again only to build, update or rebuild. --json-progress prints one JSON event per line for a front end, and --inspect only
identifies the game files. Nothing here reads another game's folder.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
GAME = json.loads((HERE / 'game.json').read_text())
APP_NAME = GAME['name']
TITLE_ID = GAME['title_id']
UPDATE_TITLE_ID = f'{int(TITLE_ID, 16) | 0x800:016X}'
AOC_BASE = int(TITLE_ID, 16) + 0x1000
ADDONS = json.loads((HERE / 'assets/expected-aoc.json').read_text())['addons']
VERSIONS = {'v100': 'expected-v1.0.0.json', 'v221': 'expected-v2.2.1.json'}
SHADERS_GB = 4   # the shader tools, every shader translated ahead (build/shader-prepared) and the seed made from it
BUILD_INFO = 'Contents/Resources/build-info.json'
BUILD_INFO_SCHEMA = 1
DATA = Path.home() / 'Library/Application Support/MacNativeGames/XC3'   # the Makefile's DATA: the player's mods

EVENTS = False
EVENT_SCHEMA = 2   # build_app.py EVENT_SCHEMA: the same events
STAGE = re.compile(r'(\d+)/(\d+) (\w+): ')
STEPS = 5
stage = {'stage': 'prepare', 'step': 0, 'of': STEPS}


class BuildError(Exception):
    """As build_app.BuildError: a message for the player, a code for the Builder (ui/README.md), the failed tool's log."""
    def __init__(self, message, code='build', log=None):
        super().__init__(message)
        self.code, self.log = code, log


def expected(version):
    return json.loads((HERE / 'assets' / VERSIONS[version]).read_text())


def event(kind, /, **fields):
    print(json.dumps({'type': kind, 'time': round(time.time(), 3), **fields}), flush=True)


def say(msg):
    if not EVENTS:
        print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)
        return
    if msg.lstrip().startswith('... still running'):
        return event('heartbeat', **stage, message=msg.strip())
    if m := STAGE.match(msg):
        stage.update(stage=m[3], step=int(m[1]), of=int(m[2]))
        msg = msg[m.end():]
    event('progress', **stage, message=msg.strip())


def notice(code, msg):
    if not EVENTS:
        return say(msg)
    if m := STAGE.match(msg):
        msg = msg[m.end():]
    event('notice', **stage, code=code, message=msg.strip())


def run(cmd, log, cwd=HERE, env=None):
    """A step's tool with its whole output in log, a line a minute while it runs; on failure the end of the log."""
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
        tool = Path(cmd[1] if Path(cmd[0]).name.startswith('python') else cmd[0]).name
        raise BuildError(f'{tool} failed (exit {p.returncode}); full output: {log}\n' + '\n'.join(tail), log=log)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def matches(path, digest):
    return path.is_file() and sha256(path) == digest


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


# ---- the game files, read without keys ----
def pfs0_entries(path):
    """(name, offset, size) of each file in a PFS0 package (NSP); [] for anything else."""
    try:
        with path.open('rb') as f:
            head = f.read(16)
            if len(head) < 16 or head[:4] != b'PFS0':
                return []
            count, strings_size = struct.unpack_from('<II', head, 4)
            table, strings = f.read(count * 24), f.read(strings_size)
            if len(table) < count * 24 or len(strings) < strings_size:
                return []
            start = 16 + count * 24 + strings_size
            entries = []
            for i in range(count):
                offset, size, name_at, _ = struct.unpack_from('<QQII', table, i * 24)
                end = strings.find(b'\0', name_at)
                entries.append((strings[name_at:end if end >= 0 else None].decode(errors='replace'), start + offset, size))
            return entries
    except (OSError, struct.error):
        return []


# Ticket body offsets by signature type (build_app.py TICKET_BODIES); only the rights ID is read, never the title key.
TICKET_BODIES = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}


def ticket_titles(path):
    """The title IDs in the rights IDs of the package's .tik files."""
    ids = []
    with path.open('rb') as f:
        for name, offset, size in pfs0_entries(path):
            if not name.lower().endswith('.tik') or size < 4:
                continue
            f.seek(offset)
            body = TICKET_BODIES.get(struct.unpack('<I', f.read(4))[0])
            if body is None or size < body + 0x168:
                continue
            f.seek(offset + body + 0x160)
            ids.append(f.read(8).hex().upper())
    return ids


def role(title):
    """What a title ID is to this game: 'base', 'update', ('dlc', index) or None for another game."""
    value = int(title, 16)
    if title == TITLE_ID:
        return 'base'
    if title == UPDATE_TITLE_ID:
        return 'update'
    if 1 <= value - AOC_BASE < 0x1000:
        return ('dlc', value - AOC_BASE)
    return None


def check_package(path, want):
    """An NSP whose tickets name its part of the game (read without keys): the base, the update or an add-on given in
    the wrong place, or another game, is refused here. The exact version is checked by the hashes of what is extracted.
    Returns the add-on index for an add-on, else None."""
    if not pfs0_entries(path):
        raise BuildError(f'{path.name}: expected an NSP of {APP_NAME}', code='input')
    titles = ticket_titles(path)
    roles = [role(t) for t in titles]
    found = next((r for r in roles if r is not None), None)
    if titles and found is None:
        raise BuildError(f'{path.name} is not {APP_NAME} ({TITLE_ID}); its tickets name {", ".join(titles)}', code='wrong_game')
    names = {'base': 'base game', 'update': 'update', 'dlc': 'add-on'}
    given = found[0] if isinstance(found, tuple) else found
    if found is not None and given != want:
        where = {'base': 'as the first file', 'update': 'with --update', 'dlc': 'with --dlc'}[given]
        raise BuildError(f'{path.name} is the {APP_NAME} {names[given]}, not the {names[want]}: give it {where}',
                         code='wrong_game')
    if want == 'dlc':
        if found is None:   # an add-on without a ticket cannot be told apart here; extract_aoc.py checks its NCA
            return None
        index = found[1]
        if str(index) not in ADDONS:
            raise BuildError(f'{path.name}: add-on {index} is not one this build knows (assets/expected-aoc.json)', code='input')
        return index
    return None


# ---- the Mac ----
def links(sdk=None):
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run(['xcrun', 'clang++', '-std=c++11', '-arch', 'arm64', '-x', 'c++', '-', '-o', Path(tmp) / 'probe'],
                           input='#include <string>\nint main() { return std::string("x").size() - 1; }\n',
                           capture_output=True, text=True, env=dict(os.environ, SDKROOT=str(sdk)) if sdk else None)
        return r.returncode == 0, r.stderr


def choose_sdk():
    """As build_app.choose_sdk: when the default SDK does not link (an SDK newer than the linker), the newest macOS 26+
    SDK that does."""
    ok, why = links()
    if ok:
        return
    developer = Path(subprocess.run(['xcode-select', '-p'], capture_output=True, text=True).stdout.strip())
    found = {}
    for folder in (developer / 'SDKs', developer / 'Platforms/MacOSX.platform/Developer/SDKs'):
        for sdk in folder.glob('MacOSX*.sdk'):
            try:
                key = tuple(int(x) for x in sdk.name[len('MacOSX'):-len('.sdk')].split('.'))
            except ValueError:
                continue
            if key[0] >= 26:
                found.setdefault(sdk.resolve(), key)
    for sdk in sorted(found, key=found.get, reverse=True):
        if links(sdk)[0]:
            os.environ['SDKROOT'] = str(sdk)
            notice('sdk_fallback', f'the default macOS SDK does not link C++ programs, so the build uses {sdk}')
            return
    raise BuildError(f'clang++ links no C++ program with any macOS SDK here: {why.strip()[:300]}', code='toolchain')


def pinned_deps(*names):
    """OPUS_PREFIX and VPX_PREFIX in this process's environment, which the Makefile reads: the repository's tools/deps
    makes them once per Mac from pinned sources, so no Homebrew is needed."""
    sys.path.insert(0, str(REPO))
    from tools.deps import DepsError, env
    try:
        os.environ.update(env(*names))
    except DepsError as e:
        raise BuildError(str(e), code='toolchain') from None


def prerequisites():
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise BuildError('needs a Mac with Apple silicon', code='toolchain')
    if int(platform.mac_ver()[0].split('.')[0] or 0) < 26:
        raise BuildError(f'needs macOS 26 or later (Metal 4); this is macOS {platform.mac_ver()[0]}', code='toolchain')
    if sys.version_info[:2] < (3, 14):
        raise BuildError(f'needs Python 3.14 (this is {platform.python_version()}): https://www.python.org/downloads/macos/',
                         code='toolchain')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise BuildError('the Xcode command line tools are missing: xcode-select --install', code='toolchain')
    choose_sdk()
    for tool in ('git', 'make'):
        if not shutil.which(tool, path='/usr/bin'):
            raise BuildError(f'{tool} not found: xcode-select --install', code='toolchain')
    pinned_deps('opus', 'libvpx')


def checkout_commit():
    release = REPO / 'release.json'   # a Builder release's sources: no git (build_app.checkout_commit)
    if release.is_file():
        commit = (read_json(release) or {}).get('commit')
        return (commit, False) if isinstance(commit, str) and re.fullmatch(r'[0-9a-f]{40}', commit) else (None, None)
    try:
        commit = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        dirty = bool(subprocess.run(['git', '-C', REPO, 'status', '--porcelain', '--untracked-files=no'],
                                    capture_output=True, text=True, check=True).stdout.strip())
        return commit, dirty
    except (OSError, subprocess.CalledProcessError):
        return None, None


def build_info(version, inputs, work):
    """build_app.build_info's record (schema 1): what the app was built from, for the Builder's library. Never the keys."""
    commit, dirty = checkout_commit()
    return dict(schema=BUILD_INFO_SCHEMA, game=GAME['id'], app_name=APP_NAME, version=version,
                display_version=expected(version)['display_version'], platform='macos',
                built=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), commit=commit, dirty=dirty, work=str(work),
                inputs={k: (str(v) if not isinstance(v, list) else [str(x) for x in v]) for k, v in inputs.items() if v})


# ---- the steps ----
def prepare_venv(work, logs):
    """<work>/venv with requirements.txt: the Python packages the scripts import."""
    venv = work / 'venv'
    if not (venv / 'bin/python').exists():
        say(f'creating the Python environment in {venv}')
        run([sys.executable, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', HERE / 'requirements.txt'], logs / 'pip.log')
    return venv / 'bin/python'


def code_done(work, version, source):
    pinned = expected(version)
    exefs = work / f'extracted/{version}/exefs'
    manifest = read_json(work / f'extracted/{version}/extraction-manifest.json') or {}
    return (manifest.get('source') == str(source)
            and all(matches(exefs / n, h) for n, h in pinned['exefs_sha256'].items()))


def step_code(py, work, version, source, keys, logs):
    """The ExeFS of the version (the base's for 1.0.0, the update's for 2.2.1), then sdk, subsdk0 and main rewritten for
    Apple silicon, each checked against the pinned hashes (make prepare checks the rewrite again)."""
    pinned = expected(version)
    shown = pinned['display_version']
    if code_done(work, version, source):
        say(f'1/{STEPS} code: the {shown} code is already extracted and verified')
    else:
        say(f'1/{STEPS} code: extracting and verifying the {shown} code')
        shutil.rmtree(work / f'extracted/{version}/exefs', ignore_errors=True)
        shutil.rmtree(work / 'extracted/.incoming', ignore_errors=True)
        try:
            run([py, 'scripts/extract_code.py', source, '--keys', keys, '--work', work], logs / 'extract-code.log')
        except BuildError as e:   # keys that do not open the NCAs, another game, another version or a damaged dump
            e.code = 'keys_or_dump'
            raise
        if not code_done(work, version, source):
            raise BuildError(f'the extracted code is not {shown} (assets/{VERSIONS[version]})', code='hash_mismatch')
    rewritten = work / f'rewritten/{version}'
    exefs = work / f'extracted/{version}/exefs'
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0', DM_ROOT=str(work))
    for module in ('sdk', 'subsdk0', 'main'):
        target = rewritten / f'{module}.patched.bin'
        if matches(target, pinned['rewritten_sha256'][target.name]):
            continue
        say(f'1/{STEPS} code: rewriting {module} for Apple silicon')
        rewritten.mkdir(parents=True, exist_ok=True)
        run([py, 'third_party/d2r/patch_nso.py', exefs / module, rewritten, module], logs / f'rewrite-{module}.log', env=env)
        if not matches(target, pinned['rewritten_sha256'][target.name]):
            raise BuildError(f'the rewritten {module} differs from the reference rewrite (assets/{VERSIONS[version]})',
                             code='hash_mismatch')
    say(f'1/{STEPS} code: verified (ExeFS and rewrite match assets/{VERSIONS[version]})')


def romfs_done(romfs, version):
    pinned = expected(version)['romfs']
    index = read_json(romfs / 'index.json') or {}
    try:
        size = (romfs / 'romfs.bin').stat().st_size
    except OSError:
        return False
    return index.get('status') == 'pass' and index.get('romfs_sha256') == pinned['sha256'] and size == pinned['size']


def fonts_valid(folder):
    return (read_json(folder / 'manifest.json') or {}).get('status') == 'pass'


def step_data(py, work, version, game, update, keys, fonts, logs):
    romfs = work / f'extracted/{version}/romfs'
    size = expected(version)['romfs']['size'] / 2**30
    if romfs_done(romfs, version):
        say(f'2/{STEPS} data: the RomFS is already verified')
    else:
        what = 'merging the base and update RomFS' if update else 'extracting the RomFS'
        say(f'2/{STEPS} data: {what} and verifying all six IVFC levels ({size:.0f} GB; several minutes)')
        shutil.rmtree(romfs, ignore_errors=True)
        shutil.rmtree(romfs.with_name('romfs.incoming'), ignore_errors=True)
        run([py, 'scripts/merge_romfs.py', game, *([update] if update else []), '--keys', keys, '--work', work],
            logs / 'romfs.log')
        if not romfs_done(romfs, version):
            raise BuildError(f'the RomFS differs from the reference {expected(version)["display_version"]} '
                             f'(assets/{VERSIONS[version]})', code='hash_mismatch')
    folder = work / 'extracted/firmware/shared-fonts'
    firmware, shared = fonts
    if firmware:
        say(f'2/{STEPS} data: extracting the system shared fonts from {firmware}')
        shutil.rmtree(folder, ignore_errors=True)
        try:
            run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, '--firmware', firmware, '--output', folder],
                logs / 'shared-fonts.log')
        except BuildError as e:
            e.code = 'fonts'
            raise
    elif shared:
        if not fonts_valid(shared):
            raise BuildError(f'{shared}: expected the extracted shared fonts with their manifest.json '
                             '(scripts/extract_shared_fonts.py writes both)', code='fonts')
        if shared.resolve() != folder.resolve():
            shutil.rmtree(folder, ignore_errors=True)
            shutil.copytree(shared, folder)
    say(f'2/{STEPS} data: ' + ('shared fonts ready' if fonts_valid(folder) else
                               'no system shared fonts (optional: the game uses its own fonts)'))


def addon_done(folder, source):
    report = read_json(folder / 'index.json') or {}
    return report.get('status') == 'pass' and report.get('source') == str(source)


def step_addons(py, work, version, dlcs, keys, logs):
    """extracted/aoc holds exactly the add-ons given: each extracted and verified once (extract_aoc.py), kept while its
    NSP is the same file, and removed when it is no longer given, so the app carries what the player chose."""
    aoc = work / 'extracted/aoc'
    if version != 'v221':
        if aoc.exists():
            shutil.rmtree(aoc)
        say(f'3/{STEPS} add-ons: none (they need the 2.2.1 update)')
        return
    wanted = {index: path for path, index in dlcs.items() if index is not None}
    for folder in (aoc.iterdir() if aoc.is_dir() else []):
        if folder.name.startswith('.') or folder.name not in {str(i) for i in wanted} or not addon_done(folder, wanted[int(folder.name)]):
            shutil.rmtree(folder, ignore_errors=True)
    for path, index in dlcs.items():
        if index is not None and addon_done(aoc / str(index), path):
            say(f'3/{STEPS} add-ons: add-on {index} is already extracted and verified')
            continue
        say(f'3/{STEPS} add-ons: extracting and verifying {path.name}')
        try:
            run([py, 'scripts/extract_aoc.py', path, '--keys', keys, '--work', work], logs / f'aoc-{index or path.stem}.log')
        except BuildError as e:
            e.code = 'keys_or_dump'
            raise
    count = len([f for f in aoc.iterdir() if not f.name.startswith('.')]) if aoc.is_dir() else 0
    say(f'3/{STEPS} add-ons: {count} ready' if count else f'3/{STEPS} add-ons: none given')


def make_args(py, work, version):
    """WORK, PY and VERSION for make, with the work folder (and the venv in it) under a path without spaces."""
    sys.path.insert(0, str(REPO))
    from tools.port_app.make_path import make_path
    try:
        seen = make_path(work, 'xc3')
    except ValueError as e:
        raise BuildError(str(e), code='input') from None
    return [f'WORK={seen}', f'PY={seen / Path(py).relative_to(work)}', f'VERSION={version}']


def step_shaders(py, work, version, logs):
    """The translator (checked against its SHA-256) and SPIRV-Cross, then every shader of the game and the add-ons
    translated ahead of play (make shader-prepare, about a minute), merged into the app's startup seed and compiled
    into Metal libraries (make shader-libs)."""
    common = make_args(py, work, version)
    say(f'4/{STEPS} shaders: the shader translator (checked against its SHA-256) and SPIRV-Cross')
    run(['make', 'shader-tools', *common], logs / 'shader-tools.log')
    say(f'4/{STEPS} shaders: translating every shader of the game ahead of play (about a minute)')
    run(['make', 'shader-prepare', *common], logs / 'shader-prepare.log')
    run(['make', 'shader-seed', *common], logs / 'shader-seed.log')
    say(f'4/{STEPS} shaders: compiling them into Metal libraries, so the game does not compile them while playing '
        '(10-20 minutes the first time; kept for the next build)')
    try:
        run(['make', 'shader-libs', *common], logs / 'shader-libs.log')
    except BuildError as e:   # no Metal compiler: xcodebuild -downloadComponent MetalToolchain
        e.code = 'toolchain'
        raise


def step_app(py, work, version, icon_source, keys, out, logs, inputs):
    say(f'5/{STEPS} app: generating the runtime tables, compiling the runtime and the launcher, packing the app')
    partial = out.with_name(out.name + '.partial')
    shutil.rmtree(partial, ignore_errors=True)
    common = make_args(py, work, version)
    run(['make', 'prepare', *common], logs / 'prepare.log')
    if (DATA / f'mods/{version}').is_dir():
        run(['make', 'mods', *common], logs / 'mods.log')
    run(['make', 'app', *common, f'APP={partial}', f'GAME={icon_source}', f'KEYS={keys}'], logs / 'app.log')
    (partial / BUILD_INFO).parent.mkdir(parents=True, exist_ok=True)
    (partial / BUILD_INFO).write_text(json.dumps(build_info(version, inputs, work), indent=2) + '\n')
    run(['codesign', '--force', '--sign', '-', partial], logs / 'codesign.log')
    if out.exists():
        old = out.with_name(f'{out.name}.replaced-{os.getpid()}')
        out.rename(old)
        partial.rename(out)
        shutil.rmtree(old, ignore_errors=True)
    else:
        partial.rename(out)
    say(f'5/{STEPS} app: {out}')


def free_gb(path):
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free / 2**30


def check_space(work, out, version):
    """The work folder needs the RomFS and the shaders. The app clones the RomFS on the same volume (APFS, no space) and
    copies it to another one, so an app on another drive than the work folder needs the RomFS's size there too."""
    romfs = expected(version)['romfs']['size'] / 2**30
    need_work = SHADERS_GB + (0 if romfs_done(work / f'extracted/{version}/romfs', version) else romfs + 1)
    if free_gb(work) < need_work:
        raise BuildError(f'needs about {need_work:.0f} GB free for the work folder {work} ({free_gb(work):.0f} GB free)',
                         code='disk_space')
    probe = out.parent
    while not probe.exists():
        probe = probe.parent
    if os.stat(probe).st_dev != os.stat(work).st_dev and free_gb(probe) < romfs + 1:
        raise BuildError(f'needs about {romfs + 1:.0f} GB free next to the app {out} ({free_gb(probe):.0f} GB free)',
                         code='disk_space')


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', type=Path, help=f'your {APP_NAME} base game NSP 1.0.0 ({TITLE_ID})')
    ap.add_argument('--update', type=Path, help=f'optional: the 2.2.1 update NSP ({UPDATE_TITLE_ID}); without it the game is 1.0.0')
    ap.add_argument('--dlc', type=Path, action='append', default=[],
                    help='optional, repeatable, 2.2.1 only: an Expansion Pass add-on NSP')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('-o', '--out', type=Path, help='the app to write (default: next to the game file)')
    ap.add_argument('--work', type=Path, help='work folder (default: XC3_build next to the game file); the app does not need it to play')
    ap.add_argument('--platform', choices=('macos', 'ios', 'visionos'), default='macos', help='macos only')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='optional: an unpacked firmware NCA directory for the system shared fonts')
    fonts.add_argument('--shared-fonts', type=Path, help='optional: shared fonts extracted before, with their manifest.json')
    # build_app.py's options for other games: refused with a reason, so a hand-off never ignores one
    ap.add_argument('--mod', type=Path, help=argparse.SUPPRESS)
    ap.add_argument('--install-mod-romfs', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--no-shader-reminder', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--json-progress', action='store_true', help='one JSON event per line on stdout')
    ap.add_argument('--inspect', action='store_true', help='identify the game files and what the build needs, then stop')
    return ap.parse_args(argv)


def resolve_inputs(args):
    """The checked paths: (game, update, {dlc: index}, keys, firmware, shared, out, work, version)."""
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    update = args.update.expanduser().resolve() if args.update else None
    dlcs = [p.expanduser().resolve() for p in args.dlc]
    for path in (game, keys, update, *dlcs):
        if path and not path.is_file():
            raise BuildError(f'no such file: {path}', code='input')
    if args.platform != 'macos':
        raise BuildError(f'{APP_NAME} is built for the Mac only: leave out --platform {args.platform}', code='platform')
    if args.mod or args.install_mod_romfs:
        raise BuildError(f'{APP_NAME} takes its RomFS mods from {DATA}/mods (README.md), not --mod', code='input')
    if dlcs and not update:
        raise BuildError(f'the {APP_NAME} add-ons need the 2.2.1 update: give it with --update', code='input')
    if len(set(dlcs)) != len(dlcs):
        raise BuildError('the same add-on NSP is given twice', code='input')
    firmware = args.firmware.expanduser().resolve() if args.firmware else None
    shared = args.shared_fonts.expanduser().resolve() if args.shared_fonts else None
    for folder in (firmware, shared):
        if folder and not folder.is_dir():
            raise BuildError(f'no such folder: {folder}', code='input')
    check_package(game, 'base')
    if update:
        check_package(update, 'update')
    indexed = {path: check_package(path, 'dlc') for path in dlcs}
    known = [i for i in indexed.values() if i is not None]
    if len(set(known)) != len(known):
        raise BuildError('two add-on NSPs hold the same Expansion Pass volume', code='input')
    out = args.out.expanduser().resolve() if args.out else game.parent / f'{APP_NAME}.app'
    if out.suffix.lower() != '.app':
        raise BuildError(f'the output must end in .app: {out}', code='input')
    work = args.work.expanduser().resolve() if args.work else game.parent / 'XC3_build'
    if work == REPO or REPO in work.parents or out == work or work in out.parents or out in work.parents:
        raise BuildError('the work folder and the app must stay outside the checkout and outside each other', code='input')
    version = 'v221' if update else 'v100'
    return game, update, indexed, keys, firmware, shared, out, work, version


def cancelled(signum, frame):
    raise KeyboardInterrupt


def main(argv):
    global EVENTS
    args = parse_args(argv)
    if args.json_progress:
        EVENTS = True
        signal.signal(signal.SIGTERM, cancelled)
        if not os.environ.pop('MACNATIVEGAMES_HELLO_SENT', None):
            event('hello', schema=EVENT_SCHEMA)
    game, update, dlcs, keys, firmware, shared, out, work, version = resolve_inputs(args)
    shown = expected(version)['display_version']
    if args.inspect:
        if EVENTS:
            event('inspect', game=GAME['id'], app_name=APP_NAME, version=version, display_version=shown, kind='nsp',
                  needs_fonts=False, needs_update=False, platforms=['macos'], dlc=True, mod=False,
                  space_gb={'macos': round(expected(version)['romfs']['size'] / 2**30 + SHADERS_GB + 1)},
                  work=str(work), default_out=str(out))
        else:
            say(f'{APP_NAME} {shown}, {len(dlcs)} add-on(s); work folder {work}; output {out}')
        return
    prerequisites()
    t0 = time.time()
    logs = work / 'logs'
    work.mkdir(parents=True, exist_ok=True)
    check_space(work, out, version)
    say(f'{APP_NAME} {shown}; work folder {work}')
    py = prepare_venv(work, logs)
    step_code(py, work, version, update or game, keys, logs)
    step_data(py, work, version, game, update, keys, (firmware, shared), logs)
    step_addons(py, work, version, dlcs, keys, logs)
    step_shaders(py, work, version, logs)
    step_app(py, work, version, update or game, keys, out, logs,
             dict(game=game, update=update, dlc=list(dlcs), firmware=firmware, shared_fonts=shared))
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}. It carries the game: the work folder {work} is needed again '
        'only to build, update or rebuild.')
    if EVENTS:
        event('done', out=str(out), game=GAME['id'], version=version, platform='macos')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        if EVENTS:
            event('error', code=e.code, message=str(e), log=str(e.log) if e.log else None)
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        if EVENTS:
            event('cancelled')
        sys.exit(130)
