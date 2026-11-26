"""Programs a Mac build would compile, compiled once by the release from the same source (user 2026-11-17: "做 1": a player
needs no Xcode command line tools to package a game).

What a Mac build compiles is the repository's own code and pinned third-party sources, never anything of the game
(games/totk/Makefile: "The game binaries below are built from this repository alone and hold nothing of the game"):
the runtime of each game version, SPIRV-Cross, the warm-up tool and the update unpacker. The release compiles them in a
copy of the commit it ships and puts them in that source archive's .prebuilt folder with a manifest (the commit and each
file's SHA-256); the Launcher's sources and its signed code updates carry the folder. A build takes a program from it
only when the folder belongs to the source it runs from (the folder's commit is the source's .mng-source-commit) and the
file's digest is the manifest's; anything else compiles as before, so a checkout, or a source with local changes,
needs the command line tools as it always did. MNG_PREBUILT_ONLY=1 makes any compile that a prebuilt program should
have spared stop instead (tests: a build that runs with no compiler).

  python3 tools/prebuilt.py build <source folder>   compile every program into <source folder>/.prebuilt (the release)

Every program runs on macOS 26 whatever Mac compiles it (user 2026-11-26: "用测试保证 以后不准错 哪怕我用这个新机器打包
26依然支持"): Launcher 1.1.5, the first release made on a macOS 27 Mac, set no minimum, clang took the SDK's (27.2) and
macOS 26 refused the games (#2395). So build() compiles with MACOS_MIN, links the libraries of the offline store
(MNG_OFFLINE_STORE: Launcher 1.1.4's, minos 26.0 and libvpx with its NEON code; 1.1.5's were rebuilt on macOS 27, where
libvpx's configure fell back to plain C), takes spirv-cross from MNG_PREBUILT_REFERENCE (Launcher 1.1.4's program, never
compiled again: "绝对不可以动翻译器") and refuses a folder in which any program asks for a newer macOS
(tools/macho_minos.py; tests/test_macos_minimum.py)."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

FOLDER = '.prebuilt'
SCHEMA = 1
MACOS_MIN = '26.0'   # every game app's and the Launcher's LSMinimumSystemVersion (tools/macho_minos.py LIMIT)
STORE_ENV = 'MNG_OFFLINE_STORE'   # tools/deps/offline.py: the offline store whose built libraries the runtimes link
REFERENCE_ENV = 'MNG_PREBUILT_REFERENCE'   # a folder holding the recorded spirv-cross (the release passes it)
# The code (tools/translator_code.py code_sha256: any signature the same) of the spirv-cross every release carries:
# Launcher 1.1.4's, built from SPIRV_CROSS aa217aeb with SPIRV_CROSS_FLAGS on the macOS 26 Mac (minos 26.0). It turns
# SPIR-V into the MSL players' shaders are made of, so it is never compiled again for a release (a compile with another
# Xcode is other code: 1.1.5's was d4e17ece…). A change answers why and whether it is necessary first, as a translator's.
SPIRV_CROSS_CODE = '589d573ea94ac76e0721af38f33c9a6ead43b83c82dd35f8f84d2f1f05d7133c'
# Tears of the Kingdom 1.0.0 / 1.2.1 / 1.4.3, Breath of the Wild 1.6.0 / 1.9.0, Bananza, Pokopia (games/totk/Makefile)
TOTK_VERSIONS = ('v0', 'v121', 'v143', 'botw160', 'botw190', 'dkb', 'pkp')


def runtime_name(version):
    """The program games/totk/Makefile makes for a version (build/<name>), and its name in the folder."""
    return 'totk-native' if version == 'v0' else f'totk-native-{version}'


# Ocarina of Time's runtime and its pool sources reader (games/oot/Makefile: made from the repository alone, nothing of
# the game; user 2026-11-22: "时之笛的运行时改成发布时编好"). Its build still compiles the game's shader libraries with
# Xcode's Metal compiler (classify.oot_needs_metal): those are made from the player's game files.
OOT_PROGRAMS = {'oot-native': 'oot-native', 'oot-pool-sources-list': 'pool-sources-list'}   # name here: name in build/
NAMES = (*(runtime_name(v) for v in TOTK_VERSIONS), 'spirv-cross', 'warm-metal-cache', 'hactool', *OOT_PROGRAMS)

# The runtimes map the game's code into executable memory at start (games/totk/runtime/loader.c seal: mprotect to
# PROT_EXEC). A process signed with the hardened runtime that does this without the entitlements that permit it is killed
# by macOS (SIGKILL, exit 137), so a runtime signed hardened but without them fails every build's runtime probe: the
# update dies with "the new runtime does not load the app's game files". These are the runtimes (not spirv-cross, the
# warm-up tool, hactool or the pool-sources list, which map no guest code) and the entitlements they need.
RUNTIME_NAMES = (*[runtime_name(v) for v in TOTK_VERSIONS], 'oot-native')
RUNTIME_ENTITLEMENTS = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.cs.allow-jit</key><true/>
<key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
</dict></plist>'''


class Missing(Exception):
    """A compile MNG_PREBUILT_ONLY=1 forbids."""


def only():
    return os.environ.get('MNG_PREBUILT_ONLY') == '1'


def commit_of(source):
    try:
        return (Path(source) / '.mng-source-commit').read_text().strip() or None
    except OSError:
        return None


def manifest(source):
    """The folder's manifest when it belongs to this source (same commit), else None."""
    source = Path(source)
    try:
        data = json.loads((source / FOLDER / 'manifest.json').read_text())
    except (OSError, ValueError):
        return None
    commit = commit_of(source)
    if data.get('schema') != SCHEMA or not commit or data.get('commit') != commit:
        return None
    return data


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


_MACHO_MAGICS = (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
                 b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce')


def runtime_signature_ok(path):
    """True when a runtime binary may map guest code: it is not signed with the hardened runtime without the
    entitlements that permit it (com.apple.security.cs.allow-jit and allow-unsigned-executable-memory). The release signs
    these (build); the Launcher's own Developer ID re-signing of the source it ships must keep the entitlements, else
    every update is killed at its runtime probe (SIGKILL, exit 137). A file that is not a Mach-O (a test's stand-in) is
    left alone."""
    with open(path, 'rb') as f:
        if f.read(4) not in _MACHO_MAGICS:
            return True   # not a Mach-O: nothing to check
    r = subprocess.run(['codesign', '-d', '--verbose=2', str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        return True
    flags = next((ln for ln in (r.stdout + r.stderr).splitlines() if 'flags=' in ln), '')
    if 'runtime' not in flags:
        return True   # not the hardened runtime: mapped guest code needs no entitlement
    r = subprocess.run(['codesign', '-d', '--entitlements', '-', str(path)], capture_output=True, text=True)
    ents = r.stdout + r.stderr
    return 'allow-jit' in ents and 'allow-unsigned-executable-memory' in ents


def program(source, name):
    """The prebuilt program `name` of this source, its digest checked (and a runtime's signature: it must not be the
    hardened runtime without the entitlements its mapped guest code needs), or None."""
    data = manifest(source)
    entry = (data or {}).get('files', {}).get(name)
    path = Path(source) / FOLDER / name
    if not entry or not path.is_file() or _sha256(path) != entry.get('sha256'):
        return None
    if name in RUNTIME_NAMES and not runtime_signature_ok(path):
        return None
    return path


def place(source, name, dest, why=''):
    """Put the prebuilt `name` at dest (executable) and say so; False when there is none, so the caller compiles. With
    MNG_PREBUILT_ONLY=1 a missing one raises Missing instead."""
    path = program(source, name)
    if path is None:
        if only():
            raise Missing(f'{name}: no prebuilt program for this source (MNG_PREBUILT_ONLY=1){": " + why if why else ""}')
        return False
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.prebuilt-partial')
    shutil.copy2(path, tmp)
    os.chmod(tmp, 0o755)
    os.replace(tmp, dest)
    print(f'   {name}: the release\'s prebuilt program (no compile)', flush=True)
    return True


def keep_placed(source, name, dest):
    """place(), but a dest already holding the same bytes is left as it is (its time too, so what make builds from it
    is not made again); True when the release's program is there."""
    path = program(source, name)
    if path is None:
        return place(source, name, dest)
    dest = Path(dest)
    if dest.is_file() and _sha256(dest) == _sha256(path):
        print(f'   {name}: the release\'s prebuilt program (already in place)', flush=True)
        return True
    return place(source, name, dest)


def complete(source):
    """Every program is there for this source: a build of any game needs no compiler (build_app.prerequisites)."""
    return all(program(source, n) is not None for n in NAMES)


def compiler_needed(what):
    """A compile is about to run: allowed, unless MNG_PREBUILT_ONLY=1."""
    if only():
        raise Missing(f'{what}: would compile, which MNG_PREBUILT_ONLY=1 forbids')


def _tool(name):
    """A module of this file's tools folder (macho_minos, translator_code), whatever the caller's sys.path."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(f'_prebuilt_{name}', Path(__file__).with_name(f'{name}.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def reference_spirv_cross():
    """MNG_PREBUILT_REFERENCE/spirv-cross when its code is SPIRV_CROSS_CODE; else SystemExit, saying where it is."""
    folder = os.environ.get(REFERENCE_ENV)
    path = Path(folder) / 'spirv-cross' if folder else None
    if path is None or not path.is_file():
        raise SystemExit(f'{REFERENCE_ENV} must name the folder of the recorded spirv-cross (Launcher 1.1.4\'s, code '
                         f'{SPIRV_CROSS_CODE[:16]}…): a release never compiles it again')
    code = _tool('translator_code').code_sha256(path)
    if code != SPIRV_CROSS_CODE:
        raise SystemExit(f'{path}: code {code[:16]}…, not the recorded spirv-cross {SPIRV_CROSS_CODE[:16]}…')
    return path


def check_macos_min(folder):
    """SystemExit when a program in folder asks for a macOS newer than MACOS_MIN."""
    bad = _tool('macho_minos').too_new([folder], MACOS_MIN)
    if bad:
        raise SystemExit(f'{len(bad)} prebuilt program(s) need a macOS newer than {MACOS_MIN}, which players on macOS 26 '
                         'cannot open:\n' + '\n'.join(f'  {m}  {what}' for what, m in bad))


def build(source):
    """Compile every program in `source` (a disposable copy of the commit: its games/ get build folders) into
    source/.prebuilt, with the manifest. Needs the command line tools; the release runs it."""
    source = Path(source).resolve()
    commit = commit_of(source)
    if not commit:
        raise SystemExit(f'{source}: no .mng-source-commit; build in a copy of the commit the release ships')
    spirv_cross = reference_spirv_cross()   # before anything compiles: a release without it stops at once
    sys.path.insert(0, str(source))
    os.environ['MNG_REPO'] = str(source)
    # Everything below compiles for MACOS_MIN, whatever this Mac's SDK (clang and ld read it; make passes it on)
    os.environ['MACOSX_DEPLOYMENT_TARGET'] = MACOS_MIN
    if os.environ.get(STORE_ENV):
        # the libraries the runtimes link come from the store (made at a release on a macOS 26 Mac), never from this
        # Mac's deps cache, which an earlier build may have filled on this Mac's macOS
        os.environ['MNG_DEPS_CACHE'] = tempfile.mkdtemp(prefix='mng-prebuilt-deps-')
    import build_app   # this source's own build code
    out = source / FOLDER
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    logs = Path(tempfile.mkdtemp(prefix='mng-prebuilt-logs-'))
    jobs = str(os.cpu_count() or 4)
    totk = source / 'games/totk'
    build_app.codec_libraries('macos')   # opus, libvpx, dav1d the runtimes link (tools/deps)
    env = dict(os.environ)
    # checked in, made from the runtime sources: a fresh checkout's file times must not ask make to make it again (that
    # needs the build's Python environment)
    (totk / 'runtime/botw_pchtxt_sites.h').touch()
    with tempfile.TemporaryDirectory(prefix='mng-runtime-ent-') as entdir:
        ent = Path(entdir) / 'runtime.entitlements'
        ent.write_text(RUNTIME_ENTITLEMENTS)

        def sign_runtime(name):
            build_app.run(['codesign', '--force', '--sign', '-', '--options', 'runtime',   # nested: not the app (a runtime program; the app it goes into is signed by build_app.sign_app)
                           '--entitlements', str(ent), str(out / name)], logs / f'{name}-sign.log', env=env)

        for version in TOTK_VERSIONS:
            name = runtime_name(version)
            build_app.run(['make', '-C', totk, f'-j{jobs}', f'build/{name}'], logs / f'{name}.log', env=env)
            shutil.copy2(totk / 'build' / name, out / name)
            sign_runtime(name)
        shutil.copy2(spirv_cross, out / 'spirv-cross')   # recorded, never compiled again (SPIRV_CROSS_CODE)
        os.chmod(out / 'spirv-cross', 0o755)
        build_app.build_warm_tool(totk, logs, env)
        shutil.copy2(totk / 'build/warm-metal-cache', out / 'warm-metal-cache')
        shutil.copy2(build_app.build_hactool(totk, logs), out / 'hactool')
        oot_work = Path(tempfile.mkdtemp(prefix='mng-prebuilt-oot-'))   # make's WORK: outside the source, with no space
        build_app.run(['make', '-C', source / 'games/oot', f'-j{jobs}', f'WORK={oot_work}', 'runtime'], logs / 'oot.log', env=env)
        for name, made in OOT_PROGRAMS.items():
            shutil.copy2(oot_work / 'build' / made, out / name)
            if name in RUNTIME_NAMES:
                sign_runtime(name)
        shutil.rmtree(oot_work, ignore_errors=True)
    check_macos_min(out)
    files ={n: {'sha256': _sha256(out / n), 'bytes': (out / n).stat().st_size} for n in NAMES}
    (out / 'manifest.json').write_text(json.dumps({'schema': SCHEMA, 'commit': commit, 'files': files}, indent=1) + '\n')
    print(f'{len(files)} prebuilt programs for {commit[:12]} in {out}', flush=True)
    return out


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    else:
        raise SystemExit(__doc__)
