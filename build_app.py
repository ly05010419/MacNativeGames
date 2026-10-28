#!/usr/bin/env python3
"""MacNativeGames: turn your own XCI or NSP into a native Mac app or iPhone/iPad IPA.

    python3 build_app.py <game.xci|game.nsp> -k <prod.keys> [--platform macos|ios|visionos] [-o <output>] [--work <folder>]
                         [--update <update.nsp>] [--dlc <dlc.nsp> ...] [--mod <mod folder> [--install-mod-romfs]]

Supported: TOTK base 1.0.0 (0100F2C0115B6000), XCI or NSP; optional --update with a supported cumulative update.
Also Breath of the Wild (01007EF00011E000, games/botw), in two forms: 1.6.0 as one card image holding the base game, the update
and the add-on content, which needs --shared-fonts (the six .bfttf files a Tears of the Kingdom build extracted into its work
folder, extracted/firmware/shared-fonts); or 1.9.0 as a base NSP with --update <the 1.9.0 update NSP> and fonts from --firmware
or --shared-fonts as for a Tears of the Kingdom NSP. With a separate update (1.9.0, or the 1.6.0 update NSP) the add-on content
(DLC) comes from --dlc <pack.nsp>, repeated for the two packs (01007EF00011F001, 01007EF00011F002), each one optional and
checked against its pin. Its startup shaders come from its game data and its own shared shader list
(games/botw/assets/shader_list.tsv). The game is recognised by the title IDs in the file, never by the file name.
NSPs need common tickets (unless rights-free) and a separate shared-font source. iOS builds are unsigned IPAs for sideloading and require Xcode 26+, xcodegen, CMake, and network access to build device libraries.
Nothing from the game is in this repository; everything is made from your files on your Mac. Key values are never printed.

Steps (each one checked against games/totk/assets/expected-v<version>.json, so a damaged dump or another version stops the
build at the first wrong file; a different dump of the same version is accepted — main.npdm by Nintendo's ACID
signature, or signed again by a dump tool once all four code modules match, shared fonts by valid BFTTF structure):
  1. code: ExeFS out of the XCI or NSP, a mod's main-module code patches merged when --mod is given
     (games/totk/scripts/mod_exefs.py, after checking them against the sites the app patches itself,
     games/totk/scripts/patch_sites.py), the three code modules rewritten for Apple silicon
     (games/totk/third_party/d2r)
  2. data: RomFS (17 GB), the system shared fonts (from XCI firmware or an explicit font source), the RomFS overlays
  3. shaders: SPIRV-Cross (fetched at a pinned commit, compiled with clang) and the shader translator, which ships
     ready-made as a native program (games/totk/prebuilt/macos-arm64: vendor/ryujinx + patches + Service.cs, made with
     games/totk/tools/totk_xlate/publish_native.sh); startup shaders are extracted from your selected RomFS and
     translated into a bundled seed, ready for GPU warm-up even with an empty cache on the first launch
  4. the game binary and either a macOS app bundle or an unsigned iPhone/iPad IPA (large game data is cloned when possible);
     for the Mac the startup shaders are then compiled once into this Mac's Metal cache (about 12 minutes for 1.4.3 on an M4 Max), so the
     first start does not compile them in the background (games/totk/tools/warm_metal_cache.m); with --mod
     --install-mod-romfs the player's mod folder for this version is replaced by the mod's RomFS files and its manifest
     (games/totk/scripts/mod_folder.py, games/totk/scripts/make_mod_manifest.py), so the build is playable with the mod as
     it stands - the flag is required because that folder may hold files of the player's own (they are kept next to it
     as <version>.backup-<date>-<time>)

Every intermediate file is in the work folder (default: "<game file folder>/TOTK_build"); a second run reuses
what is already done and correct. A newer runtime from this repository goes into an app this script made without its game files:
update_runtime.py "<the app>". Mac saves, shader cache and logs go to the user's Library folders; iOS keeps saves
and the writable shader cache in the app's Documents container.

Needs for Mac: macOS 26 or later on Apple silicon, the Xcode command line tools, Homebrew with
    brew install opus libvpx python@3.14
For iPhone/iPad: Xcode 26+ with iOS/Metal toolchains, plus xcodegen, CMake and Ninja.
Optional: Xcode's Metal toolchain (xcodebuild -downloadComponent MetalToolchain) compiles the startup shaders into the
app, so moving or rebuilding it does not repeat that work at launch.
"""
import argparse, concurrent.futures, glob, hashlib, json, os, platform, shutil, struct, subprocess, sys, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parent
GAME = REPO / 'games/totk'
sys.path.insert(0, str(GAME/'scripts'))
import mod_folder                  # noqa: E402  the player's RomFS mod folder: replaced only when --install-mod-romfs is given
EXPECTED = json.loads((GAME / 'assets/expected-v1.0.0.json').read_text())
UPDATED = json.loads((GAME / 'assets/expected-v1.1.2.json').read_text())
UPDATED121 = json.loads((GAME / 'assets/expected-v1.2.1.json').read_text())
UPDATED143 = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())
VERSIONS = {'v0': EXPECTED, 'v112': UPDATED, 'v121': UPDATED121, 'v143': UPDATED143}
UPDATES = ('v112', 'v121', 'v143')
# Breath of the Wild (games/botw; botw.md in the Tears of the Kingdom project): 1.6.0 as one card image with the base game, the
# update and the add-on content; 1.9.0 as a base NSP plus the 1.9.0 update NSP. The game is chosen by the title IDs of the NCAs
# in the game file, never by its name.
BOTW = REPO / 'games/botw'
BOTW160 = json.loads((BOTW / 'assets/expected-v1.6.0.json').read_text())
BOTW190 = json.loads((BOTW / 'assets/expected-v1.9.0.json').read_text())
BOTW_LIST = BOTW / 'assets/shader_list.tsv'   # the game's shared shader list (docs/shader-list.md)
VERSIONS['botw160'] = BOTW160
VERSIONS['botw190'] = BOTW190
TITLES = {'0100F2C0115B6000': 'totk', '01007EF00011E000': 'botw'}
# Per game: app name, bundle identifier, executable name (= the player's data folder under
# ~/Library/Application Support/MacNativeGames, games/totk/runtime/game_version.h GAME_FOLDER)
APPS = {'totk': ('Tears of the Kingdom', 'org.macnativegames.totk', 'TOTK'), 'botw': ('Breath of the Wild', 'org.macnativegames.botw', 'BotW')}
HACTOOL = ('https://github.com/SciresM/hactool.git', '1d64a83450e025622f3468c28fc4164dad2c5ef6')
APP_NAME = 'Tears of the Kingdom'
IOS_APP_NAME = 'TOTK'   # the iPhone / iPad home screen name: "Tears of the Kingdom" was cut to "Tears of the…" (user 2026-10-22)
BUNDLE_ID = 'org.macnativegames.totk'
# The languages the runtime gives the game (games/totk/runtime/services.c desired_language), as Info.plist
# CFBundleLocalizations: macOS offers a per-app language in System Settings only for an app that lists them.
# pt-BR only where the RomFS has the official USpt text (the 1.4.3 build, Breath of the Wild 1.9.0).
GAME_LANGUAGES = ('en', 'ja', 'fr', 'de', 'es', 'it', 'nl', 'ru', 'ko', 'zh-Hans', 'zh-Hant')
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
# Pinned upstream sources fetched at build time (repository, commit)
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')
# The spirv-cross program: the sources and flags of SPIRV-Cross's own CMake Release build of it, so no cmake is needed
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')
# Homebrew first; then the folder of the running interpreter (the Python uv selected, or an activated virtual environment),
# last so that a system Python's /usr/bin never shadows the Homebrew tools for the build's subprocesses.
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', str(Path(sys.executable).parent)]


def contrib_folder():
    """<clone>/shader-list-contribution (MNG_CONTRIB_FOLDER overrides): the one file players send, from the Mac and their
    iPhones / iPads (app/launcher.m contrib_file, ios/tools/shader_contrib_watch.py). Made at build time if missing."""
    folder = Path(os.environ.get('MNG_CONTRIB_FOLDER') or REPO / 'shader-list-contribution')
    folder.mkdir(parents=True, exist_ok=True)
    return folder


# Nintendo's public ACID signing key (generation 0): a dump's main.npdm is genuine when this key verifies
# its ACID signature, which is the check the Horizon loader itself makes before it starts a title
# (Atmosphere libraries/libstratosphere/source/fssystem/fssystem_crypto_configuration.cpp).
NINTENDO_ACID_MODULUS = bytes.fromhex(
    'DDC8DDF24E6DF0CA9EC75DC77BADFE7D238969B6F206A20288E15591ABCB4D50'
    '2EFC9D9476D64CD8FF10FA5E930AB457AC51C71666F41A54C2C5043D1BFE3020'
    '8AAC6F6FF5C7B668B8C9406B42AD1121E78BE9750186E4489B0A0AF87FE887F2'
    '8201E6A30FE466AE833F4E9F5E0130A400B99AAE5F03CC1860E5EF3B5E1516FE'
    '1C8278B52F477C0666885D35A2672010E76C4368D3E45A682A5AE26D73B03153'
    '1C200944F51A9D22BE12A17711E2A1CD409AA28B609BEFA0D34863A2F8A32C08'
    '56522E6019675AA79FDC3F3F692B316AB7884A148480333C9D44B73F4CE175EA'
    '37EAE81E7C77B7C61AA2F09F1061CD7B5B324C37EFB17168530AED517D3522FD'
)


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def run(cmd, log, cwd=None, env=None, quiet=True):
    """Run a step with its whole output in log; on failure show the end of it. quiet: a line a minute instead of the output."""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    done = threading.Event()

    def beat():
        t0 = time.time()
        while not done.wait(60):
            say(f'   ... still running ({(time.time() - t0) / 60:.0f} min)')
    if quiet:
        threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            p = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    finally:
        done.set()
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-25:]
        raise BuildError(f'{Path(cmd[1] if Path(cmd[0]).name.startswith("python") else cmd[0]).name} failed (exit {p.returncode}); '
                         f'full output: {log}\n' + '\n'.join(tail))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def seed_digests(seed):
    """name -> SHA-256 for the seed files whose identity (device, inode, size, modification and change time) is still the
    one prepare_shader_seed.py recorded when it checked them (<seed>.verified.json, its verified_digests). Hashing all
    65,436 files in each packaging took tens of seconds (2026-10-21); a changed file is hashed as before."""
    try:
        record = json.loads(seed.with_name(seed.name + '.verified.json').read_text())
        if record.get('schema') != 1:
            return {}
        known = {}
        for name, entry in record['files'].items():
            try:
                s = os.stat(seed / name)
            except OSError:
                continue
            if Path(name).name == name and entry[:-1] == [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]:
                known[name] = entry[-1]
        return known
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def seed_sha256(seed, name, known):
    return known.get(name) or sha256(seed / name)


def check(what, got, want):
    if got != want:
        raise BuildError(f'{what}: SHA-256 {got[:16]}... is not the expected {want[:16]}... '
                         f'(is this the supported game version, and the dump complete?)')




# ---- genuine dumps ----
def _mgf1_sha256(seed, length):
    blocks = b''
    for counter in range((length + 31) // 32):
        blocks += hashlib.sha256(seed + counter.to_bytes(4, 'big')).digest()
    return blocks[:length]


def _pss_sha256_valid(signature, modulus, message):
    """RSA-2048 PSS/SHA-256 (32-byte salt, MGF1-SHA256), the scheme of an ACID block's signature."""
    size = len(modulus)
    if size < 256 or len(signature) != size:
        return False
    modulus_number = int.from_bytes(modulus, 'big')
    number = int.from_bytes(signature, 'big')
    if number >= modulus_number:
        return False
    encoded = pow(number, 0x10001, modulus_number).to_bytes(size, 'big')
    if encoded[-1] != 0xBC:
        return False
    hashed, mask = encoded[size - 33:size - 1], _mgf1_sha256(encoded[size - 33:size - 1], size - 33)
    db = bytes([(encoded[0] ^ mask[0]) & 0x7F]) + bytes(a ^ b for a, b in zip(encoded[1:size - 33], mask[1:]))
    if db[:-33].strip(b'\x00') or db[-33] != 0x01:
        return False
    return hashlib.sha256(b'\x00' * 8 + hashlib.sha256(message).digest() + db[-32:]).digest() == hashed


def npdm_signed(path):
    """True when the npdm's ACID block carries a valid Nintendo signature over its own access control.

    The pinned SHA-256 identifies the reference dump; the signature identifies every genuine dump of the
    version, which is why this is the one ExeFS file that does not have to match byte for byte.
    """
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b'META' or int.from_bytes(data[0x04:0x08], 'little') != 0:
        return False
    acid, acid_size = struct.unpack_from('<II', data, 0x78)
    if acid_size < 0x200 or acid + acid_size > len(data) or data[acid + 0x200:acid + 0x204] != b'ACID':
        return False
    size = struct.unpack_from('<I', data, acid + 0x204)[0]
    if size < 0x100 or acid + 0x100 + size > len(data):
        return False
    return _pss_sha256_valid(data[acid:acid + 0x100], NINTENDO_ACID_MODULUS, data[acid + 0x100:acid + 0x100 + size])


def npdm_program_id(path):
    """The program ID in a well-formed npdm's ACI0 block (16 upper-case hex digits), else None."""
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b'META':
        return None
    aci, aci_size = struct.unpack_from('<II', data, 0x70)
    if aci_size < 0x20 or aci + aci_size > len(data) or data[aci:aci + 4] != b'ACI0':
        return None
    return f'{struct.unpack_from("<Q", data, aci + 0x10)[0]:016X}'


def npdm_kind(path, title):
    """How a main.npdm that is not the reference dump's file is accepted: 'signed' (Nintendo's ACID signature: another
    genuine dump), 'resigned' (a well-formed npdm of this title whose ACID a dump tool signed again, as tools that change
    an NCA's key generation do), else None. The game never reads main.npdm here, and the code modules are checked by
    hash, so a re-signed one is accepted only alongside matching code (exefs_matches, step_code)."""
    if npdm_signed(path):
        return 'signed'
    return 'resigned' if npdm_program_id(path) == title.upper() else None


def exefs_matches(exefs, expected, title):
    """True when the extracted ExeFS holds the expected code modules and a main.npdm of this version (npdm_kind)."""
    for name, digest in expected.items():
        if name != 'main.npdm' and (not (exefs / name).is_file() or sha256(exefs / name) != digest):
            return False
    npdm = exefs / 'main.npdm'
    return 'main.npdm' not in expected or (npdm.is_file() and (sha256(npdm) == expected['main.npdm'] or npdm_kind(npdm, title) is not None))


def _bfttf_valid(path):
    """A structurally valid shared font, by the same size check scripts/extract_shared_fonts.py makes."""
    data = path.read_bytes()
    if len(data) < 12:
        return False
    magic, size = struct.unpack_from('>II', data)
    key = magic ^ 0x7f9a0218
    if (size ^ key) + 8 != len(data):
        return False
    return struct.unpack_from('>I', data, 8)[0] ^ key in (0x00010000, 0x4F54544F)


# ---- prerequisites ----
def prerequisites(args):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise BuildError('needs a Mac with Apple silicon')
    major = int(platform.mac_ver()[0].split('.')[0] or 0)
    if major < 26:
        raise BuildError(f'needs macOS 26 or later (Metal 4); this is macOS {platform.mac_ver()[0]}')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise BuildError('the Xcode command line tools are missing: xcode-select --install')
    # SPIRV-Cross is C++: without the SDK's libc++ headers the build stops much later at "'string' file not found" (#41)
    cxx = subprocess.run(['xcrun', 'clang++', '-std=c++11', '-x', 'c++', '-fsyntax-only', '-'], input='#include <string>\n',
                         capture_output=True, text=True)
    if cxx.returncode:
        raise BuildError('clang++ finds no C++ standard library headers, so the developer tools or their SDK are incomplete. '
                         'With Xcode: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer; with the command line '
                         'tools only: sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install '
                         f'(xcode-select -p and xcrun --show-sdk-path show what is used). clang++ said: {cxx.stderr.strip()[:300]}')
    brew = Path('/opt/homebrew')
    missing = [p for p in ('opus', 'libvpx') if not (brew / 'opt' / p / 'lib').is_dir()]
    if missing:
        raise BuildError(f'Homebrew packages missing: brew install {" ".join(missing)}')
    if not shutil.which('git', path=os.pathsep.join(TOOL_PATHS + ['/usr/bin'])):
        raise BuildError('git not found: xcode-select --install')
    py = next((p for p in (shutil.which(n, path=os.pathsep.join(TOOL_PATHS)) for n in ('python3.14', 'python3.15')) if p), None)
    if not py:
        raise BuildError('Python 3.14 not found (the build reads zstd with the standard library): brew install python@3.14')
    return Path(py)


# ---- work folder ----
def git_fetch(url, commit, dst, log):
    if (dst / '.git').is_dir() and subprocess.run(['git', '-C', dst, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() == commit:
        return
    if dst.is_symlink():
        dst.unlink()   # replace only the cache link, never its external target
    elif dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    run(['git', '-C', dst, 'init', '-q'], log)
    run(['git', '-C', dst, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', dst, 'checkout', '-q', 'FETCH_HEAD'], log)


def link(path, target):
    if path.is_symlink() or path.exists():
        if path.is_symlink() and path.resolve() == Path(target).resolve():
            return
        path.unlink() if path.is_symlink() else shutil.rmtree(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.symlink_to(target)


def prepare_work(work, py, logs):
    """work/totk: the game folder of this repository with the generated files inside it (the build scripts write
    extracted/, rewritten/, build/ and analysis/ next to themselves); its tooling/ points at the fetched sources."""
    tree = work / 'totk'
    tree.mkdir(parents=True, exist_ok=True)
    # Match both directories AND symlinks to directories. A trailing slash
    # allows rsync -a to copy a tooling symlink from a development checkout.
    excludes = [f'--exclude=/{name}' for name in ('extracted', 'rewritten', 'build', 'analysis', '.venv', 'tooling', 'user-data')]
    # Files are compared by content and a changed file gets the time of this copy, not its own: make decides by times, and
    # a checkout older than the objects of the last build (another branch built in this folder after it was checked out)
    # would otherwise keep the stale objects. 2026-10-25: nvn_mtl.m.o of one branch linked with nvn_refl.m.o of another
    # (different nvn_rbind layouts) aborted every start in the shader warm-up ("参数块布局异常"). Unchanged files keep
    # their times, so nothing is rebuilt needlessly.
    sync = ['rsync', '-rlpD', '--checksum', *excludes]
    run([*sync, f'{GAME}/', f'{tree}/'], logs / 'sources.log')
    # games/botw: its scripts find the shared ones at ../totk/scripts (work/botw next to work/totk)
    run([*sync, f'{BOTW}/', f'{work / "botw"}/'], logs / 'sources-botw.log')
    tooling = tree / 'tooling'
    # Migrate a work folder made by the old copier before writing child links:
    # link(tooling/spirv-cross, ...) must not traverse into a shared checkout.
    if tooling.is_symlink():
        tooling.unlink()
    tooling.mkdir(exist_ok=True)
    src = work / 'src'
    say('fetching the pinned SPIRV-Cross sources')
    git_fetch(*SPIRV_CROSS, src / 'SPIRV-Cross', logs / 'fetch-spirv-cross.log')
    link(tooling / 'spirv-cross', src / 'SPIRV-Cross')
    venv = work / 'venv'
    if not (venv / 'bin/python').exists():
        say('Python environment for the build tools')
        run([py, '-m', 'venv', venv], logs / 'venv.log')
    run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', tree / 'requirements.txt'], logs / 'pip.log')
    link(tree / '.venv', venv)
    (tree / 'analysis').mkdir(exist_ok=True)
    (tree / 'build').mkdir(exist_ok=True)
    return tree


# ---- steps ----
def input_kind(game):
    with game.open('rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsz' if game.suffix.lower() == '.nsz' else 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xci'
    raise BuildError('expected an XCI or NSP (PFS0) game file, or a supported NSZ update')


def source_matches(manifest, game):
    try:
        data = json.loads(manifest.read_text())
        stat = game.stat()
        return (data['source'] == str(game.resolve()) and data['source_size'] == stat.st_size
                and data['source_mtime_ns'] == stat.st_mtime_ns)
    except (OSError, ValueError, KeyError):
        return False


def prepare_update_container(tree, game, keys, logs):
    """Expand and cache a compressed update using NSZ's hash-verified NCA conversion."""
    if game.suffix.lower() != '.nsz':
        return game
    folder = tree / 'extracted/containers'
    output = folder / f'{game.stem}.nsp'
    manifest = folder / f'{game.stem}.json'
    if source_matches(manifest, game) and output.is_file():
        return output
    folder.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    if manifest.exists():
        manifest.unlink()
    say('1/4 code: decompressing the NSZ update and verifying its NCA hashes')
    run([tree / '.venv/bin/nsz', '-D', '--keys', keys, '-o', folder, game],
        logs / f'decompress-{game.stem}.log', cwd=tree)
    generated = folder / f'{game.stem}.nsp'
    if not generated.is_file():
        raise BuildError(f'NSZ decompressor did not produce an NSP: {generated}')
    if generated != output:
        generated.replace(output)
    stat = game.stat()
    manifest.write_text(json.dumps(dict(source=str(game.resolve()), source_size=stat.st_size,
                                        source_mtime_ns=stat.st_mtime_ns), indent=2) + '\n')
    return output


def identify_update(tree, game, keys, logs):
    """Choose the runtime by verified module bytes, never by the package filename."""
    source = prepare_update_container(tree, game, keys, logs)
    for version in UPDATES:
        exefs = tree / f'extracted/{version}/exefs'
        if source_matches(exefs.parent / 'extraction-manifest.json', source) and exefs_matches(
                exefs, VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id']):
            return version, source
    staging = tree / 'extracted/update-detection'
    if staging.exists(): shutil.rmtree(staging)
    run([tree / '.venv/bin/python', 'scripts/extract_exefs.py', source, '--keys', keys,
         '--update', '--output', staging / 'exefs'], logs / 'identify-update.log', cwd=tree)
    version = next((version for version in UPDATES
                    if exefs_matches(staging / 'exefs', VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id'])), None)
    if version is None:
        raise BuildError('unsupported update module hashes; expected 1.1.2, 1.2.1 or 1.4.3 (the file name is not used to choose a version)')
    target = tree / f'extracted/{version}'
    target.mkdir(parents=True, exist_ok=True)
    if (target / 'exefs').exists(): shutil.rmtree(target / 'exefs')
    (staging / 'exefs').rename(target / 'exefs')
    (staging / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
    staging.rmdir()
    say(f'update identified by verified ExeFS hashes: {VERSIONS[version]["display_version"]}')
    return version, source


def identify_botw_update(tree, update, keys, logs):
    """Breath of the Wild 1.6.0 or 1.9.0, by the verified ExeFS hashes of the update (never by the file name)."""
    for version in ('botw160', 'botw190'):
        exefs = tree / f'extracted/{version}/exefs'
        if source_matches(exefs.parent / 'extraction-manifest.json', update) and exefs_matches(
                exefs, VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id']):
            return version
    staging = tree / 'extracted/update-detection-botw'
    if staging.exists(): shutil.rmtree(staging)
    run([tree / '.venv/bin/python', tree.parent / 'botw/scripts/extract_exefs.py', update, '--keys', keys,
         '--output', staging / 'exefs'], logs / 'identify-update-botw.log', cwd=tree)
    version = next((version for version in ('botw160', 'botw190')
                    if exefs_matches(staging / 'exefs', VERSIONS[version]['exefs_sha256'], VERSIONS[version]['title_id'])), None)
    if version is None:
        shutil.rmtree(staging)
        raise BuildError('unsupported Breath of the Wild update module hashes; expected 1.6.0 or 1.9.0 (the file name is not used to choose a version)')
    target = tree / f'extracted/{version}'   # the verified ExeFS is kept, as identify_update does: step_code_botw reuses it
    target.mkdir(parents=True, exist_ok=True)
    if (target / 'exefs').exists(): shutil.rmtree(target / 'exefs')
    (staging / 'exefs').rename(target / 'exefs')
    (staging / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
    staging.rmdir()
    say(f'update identified by verified ExeFS hashes: Breath of the Wild {VERSIONS[version]["display_version"]}')
    return version


def font_report(folder):
    """The fonts bound into the runtime: the pinned hashes identify the reference dump, and a valid font from
    another firmware version is bound by the SHA-256 of its actual bytes (the manifest keeps that value, so
    scripts/prepare_shared_fonts.py writes the real bytes into the runtime)."""
    rows = []
    for kind, (name, expected) in enumerate(EXPECTED['shared_fonts_sha256'].items()):
        path = folder / f'{kind}-{name}.bfttf'
        if not path.is_file():
            raise BuildError(f'missing shared font: {path}')
        digest = sha256(path)
        if digest != expected:
            if not _bfttf_valid(path):
                raise BuildError(f'shared font {name}: not the expected bytes and not a valid BFTTF font '
                                 f'(SHA-256 {digest[:16]}...)')
            say(f'2/4 data: shared font {name} comes from another firmware version; its bytes are bound instead')
        size = path.stat().st_size
        rows.append(dict(type=kind, name=name, bfttf_bytes=size, font_bytes=size - 8, sha256=digest))
    return dict(status='pass', integrity='Each shared font is a valid BFTTF; the reference dump matches its pinned SHA-256',
                fonts=rows)


def validate_fonts(folder):
    report = font_report(folder)
    try:
        manifest = json.loads((folder / 'manifest.json').read_text())
        rows = manifest['fonts']
        if manifest['status'] != 'pass' or len(rows) != 6 or any(
                any(row.get(k) != value for k, value in expected.items())
                for row, expected in zip(rows, report['fonts'])):
            raise ValueError('font manifest differs from its files')
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        raise BuildError(f'invalid shared-font manifest in {folder}: {e}') from None


def fonts_valid(folder):
    try:
        validate_fonts(folder)
        return True
    except (BuildError, OSError):
        return False


def merge_mod_exefs(tree, mod, version, logs):
    """Merge a mod's main-module code patches into a copy of the verified retail main (scripts/mod_exefs.py): the
    game's own patch sites are checked first, and the merged module is what the rewrite and the app are built from.
    The retail extracted/<version>/exefs is never touched, so version identification and its pinned hashes stay put."""
    py = tree / '.venv/bin/python'
    staged = tree / f'extracted/{version}/mod-exefs'
    if staged.exists():
        shutil.rmtree(staged)
    main, report_path = staged / 'main', tree / f'extracted/{version}/mod-exefs.json'
    previous = {}                      # the report of the last build: the same patches keep the same rewrite
    if report_path.is_file():
        try:
            previous = json.loads(report_path.read_text())
        except ValueError:
            previous = {}
    run([py, 'scripts/mod_exefs.py', 'apply', '--exefs', tree / f'extracted/{version}/exefs', '--mod', mod,
         '--version', version, '--output', main, '--report', report_path],
        logs / f'mod-exefs-{version}.log', cwd=tree)
    merged = json.loads(report_path.read_text())
    if merged.get('status') == 'skipped':           # an exefs/ with nothing to merge into main (a code module the app
        for note in merged.get('notes', []):        # does not load, a main.npdm): the romfs/ half still goes in
            say(f'1/4 code: {note}')
        say(f'1/4 code: nothing to merge for {mod.name}; {version} is built from the retail main')
        return None, None
    if merged.get('status') != 'pass' or merged.get('version') != version:
        raise BuildError(f'{report_path} is not a merge report for {version}')
    # The rewrite of the merged module is cached on the patched module's hash, and on the rewriter itself: a changed
    # patch_nso.py (a new GAME_ADDR patch, a new cut) makes a different rewrite out of the same bytes. patch_nso.py
    # records the same digest in build_src.sha256 next to each image.
    rewriter = sha256(tree/'third_party/d2r/patch_nso.py')
    if previous.get('patched_main_sha256') == merged['patched_main_sha256'] \
            and previous.get('rewritten_main_sha256') and previous.get('rewriter_sha256') == rewriter:
        merged['rewritten_main_sha256'] = previous['rewritten_main_sha256']     # same patches: the rewrite still holds
    merged['rewriter_sha256'] = rewriter
    report_path.write_text(json.dumps(merged, indent=2) + '\n')
    edits = sum(p['edit_count'] for p in merged['patches'])
    say(f"1/4 code: merging {mod.name}'s main-module patch ({edits} edits, {merged['edit_bytes']} bytes of main) "
        f"before the rewrite")
    for note in merged.get('notes', []):
        say(f'   {note}')
    return merged, report_path


def step_code(tree, game, keys, env, logs, *, version="v0", mod=None):
    expected = VERSIONS[version]
    update_args = [] if version == "v0" else ["--update"]
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    want = expected['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'
    if not source_matches(manifest, game) or not exefs_matches(exefs, want, expected['title_id']):
        say('1/4 code: extracting the ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        source = prepare_update_container(tree, game, keys, logs) if update_args else game
        run([py, 'scripts/extract_exefs.py', source, '--keys', keys, '--output', exefs, *update_args], logs / f'extract_exefs-{version}.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not the supported version')
        got = sha256(exefs / n)
        if got == h:
            continue
        kind = npdm_kind(exefs / n, expected['title_id']) if n == 'main.npdm' else None
        if kind == 'signed':
            say(f"1/4 code: main.npdm is a Nintendo-signed {expected['display_version']} file from another dump; using it")
            continue
        if kind == 'resigned':   # the code modules are checked in this same loop: any mismatch there still stops the build
            say(f"1/4 code: main.npdm of this dump was signed again by a dump tool (not Nintendo's signature, e.g. a changed "
                f"key generation); the game does not use it and the code must match {expected['display_version']}; using it")
            continue
        check(f'ExeFS {n}', got, h)
    merged, merged_path = merge_mod_exefs(tree, mod, version, logs) if mod else (None, None)
    if not (tree / f'extracted/{version}/elf/main-inventory.json').exists():
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{version}/elf'], logs / 'prepare_modules.log', cwd=tree)
    out = tree / f'rewritten/{version}'
    out.mkdir(parents=True, exist_ok=True)
    for name in ('sdk', 'subsdk0', 'main'):
        image = out / f'{name}.patched.bin'
        modded = merged is not None and name == 'main'
        source = tree / f'extracted/{version}/mod-exefs/main' if modded else exefs / name
        target = merged['rewritten_main_sha256'] if modded else expected['rewritten_sha256'][f'{name}.patched.bin']
        if image.exists() and target and sha256(image) == target:
            continue
        say(f'1/4 code: rewriting {name} for Apple silicon' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', source, out, name], logs / f'rewrite_{version}_{name}.log', cwd=tree,
            env=dict(env, DM_RULESET='native', DM_PRIVATE_GATES='0'))
        if modded:
            # The rewrite of the merged module is not the verified retail rewrite any more: the mod's own edits are
            # bound by the module's hash instead (the report says so, and the loader binds itself to this digest).
            merged['rewritten_main_sha256'] = sha256(image)
            merged_path.write_text(json.dumps(merged, indent=2) + '\n')
        else:
            check(f'rewritten {name}', sha256(image), target)
    # The loader binds itself to these images (scripts/prepare_runtime.py): they are the same bytes as the rewrite that
    # was verified site by site (python3 games/totk/scripts/verify_rewrites.py re-runs that verification, hours) unless
    # --mod merged a patch into main, which the report records.
    digests = {f'{name}.patched.bin': sha256(out / f'{name}.patched.bin') for name in ('main', 'sdk', 'subsdk0')}
    scope = f'hash match with verified {expected["display_version"]} rewrite'
    if merged:
        scope = (f'verified {expected["display_version"]} rewrite, with the main-module patches of {merged["folder"]} '
                 f'merged before it ({merged["patched_main_sha256"][:16]}...); the mod\u2019s bytes are bound by hash')
    report = dict(format=1, status='pass', rom_version=version, scope=scope,
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': want[n] for n in ('main', 'sdk', 'subsdk0')},
                                      **{f'rewritten/{version}/{n}': digests[n] for n in digests}},
                  **({'mod_exefs': {key: merged[key] for key in
                                    ('folder', 'folder_sha256', 'build_id', 'patches', 'edit_bytes', 'reserved_sites',
                                     'retail_main_sha256', 'patched_main_sha256', 'rewritten_main_sha256',
                                     'rewriter_sha256', 'notes')}} if merged else {}))
    report_path = tree / ('analysis/phase12-validation.json' if version == 'v0' else f'analysis/{version}-port/phase12-validation.json')
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')


HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'


def mod_heapdef(mod):
    """The mod's replacement of the heap-size table, when it has one: the 2K overlay is then built on top of it, so the
    mod's own heap sizes and the 2K texture heaps both hold. The player's manifest must leave that file to the app."""
    path = mod / 'romfs' / HEAPDEF if mod else None
    return path if path and path.is_file() else None


def mergeable_overlays(version):
    """The app's own overlay files a mod's copy is merged with instead of being left out (scripts/make_mod_manifest.py,
    merge_build_patch): the UI archive whose quick-menu edit the build applies to the mod's own copy (#73)."""
    try:
        from make_romfs_patches import common_archive
        return {common_archive(version)}
    except (ImportError, KeyError):
        return set()


def check_mod_overlays(tree, mod, version):
    """A mod folder may hold files the app replaces itself. They are not a build problem - the player's manifest leaves
    such paths out by itself (games/totk/scripts/make_mod_manifest.py), so the app's version is the one loaded - but say
    so, instead of leaving somebody wondering why the mod's version of a file does nothing. The heap table is the one the
    build folds in (see mod_heapdef), so it is not reported."""
    if not (mod and (mod / 'romfs').is_dir()):
        return []
    replaced = set()
    folders = [tree / f'extracted/{version}/romfs-patches']
    folders += sorted(p for p in (tree / f'extracted/{version}/romfs-patches-out').glob('*') if p.is_dir())
    for folder in folders:
        manifest = folder / 'manifest.tsv'
        if manifest.is_file():
            for line in manifest.read_text().splitlines():
                name = line.split('\t', 1)[0]
                if name and not name.startswith('#'):
                    replaced.add(name)
    mergeable = mergeable_overlays(version)
    clashes = sorted(name for name in replaced
                     if name != HEAPDEF and name not in mergeable and (mod / 'romfs' / name).is_file())
    folded = sorted(name for name in replaced if name in mergeable and (mod / 'romfs' / name).is_file())
    if folded:
        say(f"1/4 code: {mod.name}/romfs also holds {folded[0]}, which the app replaces itself: the build's edit is "
            f"merged into the mod's copy, so both apply (#73)")
    if clashes:
        say(f"1/4 code: {mod.name}/romfs also holds {clashes[0]}, which the app replaces itself; a player's manifest "
            f"leaves it out, so the app's version of it is the one loaded")
    return clashes


OVERLAY_RECORD = 'overlay.json'
# The bigger outputs whose heap overlays the app carries: the settings panel's 2K. The game sizes its swapchain and heaps
# for the largest one present and starts at 1080p. Not 4K yet: its heaps need one memory pool above the graphics layer's
# 1 GiB zero-copy limit (games/totk/runtime/game_patches_v143.c, 2026-10-25: the game stopped at the start).
OUTPUT_SIZES = ('2560x1440',)


def overlay_record(path):
    """The record a 2K output overlay leaves next to its manifest (scripts/make_output_res_overlay.py), or {}: what it
    was built from."""
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def overlay_stale(out_dir, heapdef, tree):
    """Why the 2K output overlay has to be built again, '' when the one there is current: it is missing, it has no
    record, it was made from another heap table (a --mod build chains the mod's sizes into that table, so without this a
    later build without --mod kept the mod's heap sizes), or the overlay script itself changed."""
    if not (out_dir/'manifest.tsv').is_file():
        return 'missing'
    record = overlay_record(out_dir/OVERLAY_RECORD)
    if not record:
        return 'no record of what it was built from'
    if record.get('heapdef_sha256') != (sha256(heapdef) if heapdef else None):
        return 'built from another heap table'
    if record.get('script_sha256') != sha256(tree/'scripts/make_output_res_overlay.py'):
        return 'built by another version of scripts/make_output_res_overlay.py'
    return ''


def install_mod_romfs(mod, tree, version, out, py, logs):
    """--mod --install-mod-romfs: replace the player's mod folder for this version with the mod's RomFS files, and write
    its manifest against the app just built (games/totk/scripts/mod_folder.py). The replacement is copied and given its
    manifest in a folder next to the player's first, so the player's files are only moved aside for a copy that is
    complete - and not at all when the mod would replace nothing the game loads. They are kept as a backup next to the
    folder, never deleted. The manifest leaves out the paths the
    app replaces itself, the heap table included: mod_heapdef (see above) folded its sizes into the 2K overlay."""
    log = logs / f'mod-manifest-{version}.log'
    try:
        installed = mod_folder.install(mod, version, out, py, tree, say=say)
    except mod_folder.InstallError as e:
        log.write_text(e.output + '\n')
        for line in e.output.splitlines()[-3:]:
            say(f'   {line}')
        raise BuildError(f'the app itself is built, but {mod_folder.mods_folder(version)} was not replaced: {e} '
                         f'(it holds what it held before; fix {mod.name}/romfs, then write the manifest again with '
                         f'scripts/make_mod_manifest.py)') from None
    if installed is None:
        return None
    log.write_text('\n'.join(installed.manifest) + '\n')
    say(f'4/4 app: {installed.folder} holds {installed.copied} files from {mod.name}/romfs now'
        + (f' (what it held before: {installed.backup}; delete that folder once you no longer need it)'
           if installed.backup else ''))
    for line in installed.manifest:                 # the notes and the summary; a mod's notes are a handful
        say(f'   {line}')
    return installed.folder


def say_mod_copy_steps(mod, tree, version, out):
    """--mod without --install-mod-romfs: the version's mod folder is the player's own, so the build leaves it alone and
    only says how to fill it."""
    if not (mod/'romfs').is_dir():
        say(f'4/4 app: {mod.name} has no romfs/: its code patches are in the app, and there is no data half to copy')
        return
    folder = mod_folder.mods_folder(version)
    say(f'4/4 app: {mod.name}/romfs was not installed: {folder} is your folder, and a build only replaces it when you '
        f'pass --install-mod-romfs (which keeps what it held as a backup next to it). To do it by hand:')
    say(f'   rsync -a --exclude=\'.DS_Store\' --exclude=\'manifest.tsv\' "{mod}/romfs/" "{folder}/"')
    say(f'   "{sys.executable}" "{tree}/scripts/make_mod_manifest.py" "{folder}" --app "{out}" --version {version}')


def step_data(tree, game, keys, logs, *, firmware=None, shared_fonts=None, mod=None):
    py = tree / '.venv/bin/python'
    romfs = tree / 'extracted/v0/romfs'
    index = romfs / 'index.json'
    ok = index.exists() and (romfs / 'romfs.bin').is_file() and json.loads(index.read_text()).get('romfs_sha256') == EXPECTED['romfs']['sha256'] and \
        (romfs / 'romfs.bin').stat().st_size == EXPECTED['romfs']['size']
    if not ok:
        say('2/4 data: extracting and verifying the RomFS (17 GB, a minute or two)')
        if romfs.exists():
            shutil.rmtree(romfs)
        run([py, 'scripts/extract_romfs.py', '--keys', keys], logs / 'extract_romfs.log', cwd=tree)
        r = json.loads(index.read_text())
        check('RomFS', r['romfs_sha256'], EXPECTED['romfs']['sha256'])
    fonts = tree / 'extracted/firmware/shared-fonts'
    if not fonts_valid(fonts):
        if shared_fonts:
            # Validate the actual bytes before copying, not just the source's manifest.
            report = font_report(shared_fonts)
            if shared_fonts.resolve() != fonts.resolve():
                if fonts.exists():
                    shutil.rmtree(fonts)
                fonts.mkdir(parents=True)
                for f in report['fonts']:
                    name = f"{f['type']}-{f['name']}.bfttf"
                    shutil.copy2(shared_fonts / name, fonts / name)
            (fonts / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        else:
            if not firmware and input_kind(game) == 'nsp':
                raise BuildError('NSP needs --firmware or --shared-fonts because no valid shared fonts are cached')
            say('2/4 data: extracting the system shared fonts')
            if fonts.exists():
                shutil.rmtree(fonts)
            source_args = ['--firmware', firmware] if firmware else ['--xci', game]
            run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source_args, '--output', fonts],
                logs / 'shared_fonts.log', cwd=tree)
        validate_fonts(fonts)
    if not (tree / 'extracted/v0/romfs-patches/manifest.tsv').exists():
        say('2/4 data: RomFS overlays (quick-menu capture timing, game-over retry)')
        run([py, 'scripts/make_romfs_patches.py'], logs / 'romfs_patches.log', cwd=tree)
    heapdef = mod_heapdef(mod)
    for size in OUTPUT_SIZES:   # the game takes the largest one present (runtime/game_patches.c output_res_auto)
        overlay = tree / f'extracted/v0/romfs-patches-out/{size}'
        why = overlay_stale(overlay, heapdef, tree)
        if why:
            say(f'2/4 data: RomFS overlay for the {size} output (the settings panel\'s resolution): {why}'
                + ('; built on top of the mod\'s heap sizes' if heapdef else ''))
            run([py, 'scripts/make_output_res_overlay.py', size, *(['--heapdef', heapdef] if heapdef else [])],
                logs / f'output_res_overlay-{size}.log', cwd=tree)


def build_hactool(tree, logs):
    source = tree.parent / 'src/hactool'
    git_fetch(*HACTOOL, source, logs/'fetch-hactool.log')
    # Add an explicit private title-key file, so base NSP and update tickets do not
    # need command-line keys or changes to the player's HOME/.switch files.
    main = source/'main.c'
    text = main.read_text()
    old = 'FILE *titlekeyfile = open_key_file("title");'
    new = 'FILE *titlekeyfile = getenv("MNG_TITLE_KEYS") ? fopen(getenv("MNG_TITLE_KEYS"), "rb") : open_key_file("title");'
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool title-key hook changed')
        main.write_text(text.replace(old, new))
    # Expose only the bounded Program NCA inside the original XCI/NSP as a
    # seekable FILE. This avoids a second 17 GB base copy during BKTR merging.
    text = main.read_text()
    if '#define _DARWIN_C_SOURCE 1' not in text:
        text = '#define _DARWIN_C_SOURCE 1\n' + text
        main.write_text(text)
    hook = 'static FILE *mng_open_base(const char *path)'
    if hook not in text:
        helper = r'''
typedef struct { FILE *file; off_t start, size, position; } mng_base_range;
static int mng_base_read(void *cookie, char *buffer, int count) {
    mng_base_range *range = cookie;
    off_t left = range->size - range->position;
    if (left < count) count = (int)left;
    if (!count) return 0;
    if (fseeko(range->file, range->start + range->position, SEEK_SET)) return -1;
    int got = (int)fread(buffer, 1, count, range->file);
    if (!got && ferror(range->file)) return -1;
    range->position += got;
    return got;
}
static fpos_t mng_base_seek(void *cookie, fpos_t offset, int whence) {
    mng_base_range *range = cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;
    if (base < 0 || offset < -base || offset > range->size - base) return (fpos_t)-1;
    return (fpos_t)(range->position = base + offset);
}
static int mng_base_close(void *cookie) {
    mng_base_range *range = cookie;
    int result = fclose(range->file); free(range); return result;
}
static FILE *mng_open_base(const char *path) {
    const char *start = getenv("MNG_BASE_RANGE_OFFSET"), *size = getenv("MNG_BASE_RANGE_SIZE");
    if (!start && !size) return fopen(path, "rb");
    if (!start || !size) return NULL;
    char *end1, *end2;
    unsigned long long a = strtoull(start, &end1, 10), n = strtoull(size, &end2, 10);
    if (!*start || !*size || *end1 || *end2 || a > INT64_MAX || !n || n > INT64_MAX - a) return NULL;
    mng_base_range *range = calloc(1, sizeof(*range));
    if (!range) return NULL;
    range->file = fopen(path, "rb"); range->start = (off_t)a; range->size = (off_t)n;
    if (!range->file) { free(range); return NULL; }
    if (fseeko(range->file, 0, SEEK_END) || ftello(range->file) < (off_t)(a + n)) { mng_base_close(range); return NULL; }
    FILE *view = funopen(range, mng_base_read, NULL, mng_base_seek, mng_base_close);
    if (!view) mng_base_close(range);
    return view;
}
'''
        marker = 'int main(int argc, char **argv)'
        if text.count(marker) != 1 or text.count('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")') != 2:
            raise BuildError('pinned hactool bounded base-file hook changed')
        text = text.replace(marker, helper + '\n' + marker)
        text = text.replace('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")',
                            'nca_ctx.tool_ctx->base_file = mng_open_base(optarg)')
        main.write_text(text)
    # Some newer cumulative update NCAs leave sector-aligned padding after the
    # BKTR subsection table. The relocation and subsection tables still have to
    # be contiguous and remain inside the section; IVFC verification below
    # validates the reconstructed RomFS data. Keep hactool's strict default for
    # the table boundary and permit only this unused trailing section space.
    nca = source/'nca.c'
    text = nca.read_text()
    old = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size != ctx->size')
    new = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size > ctx->size ||\n'
           '            (ctx->size - (sb->subsection_header.offset + sb->subsection_header.size)) % 0x200 != 0')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR layout check changed')
        nca.write_text(text.replace(old, new))
    text = nca.read_text()
    old = 'if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {'
    new = ('if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset &&\n'
           '            (sb->relocation_header.size > sb->subsection_header.offset ||\n'
           '             ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset - sb->relocation_header.size)) {')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR total-size check changed')
        nca.write_text(text.replace(old, new))
    config = source/'config.mk'
    if not config.exists():
        shutil.copy2(source/'config.mk.template', config)
    run(['make', '-j2', 'CC=clang'], logs/'hactool-build.log', cwd=source)
    return source/'hactool'


def step_update_data(tree, game, update, keys, logs, *, version='v112', mod=None):
    expected = VERSIONS[version]
    folder = tree/f'extracted/{version}/romfs'
    index = folder/'index.json'
    valid = False
    if index.is_file() and (folder/'romfs.bin').is_file():
        try:
            report = json.loads(index.read_text())
            valid = (report.get('status') == 'pass' and report['romfs_sha256'] == expected['romfs']['sha256']
                     and (folder/'romfs.bin').stat().st_size == expected['romfs']['size'])
        except (ValueError, KeyError):
            pass
    if not valid:
        hactool = build_hactool(tree, logs)
        if folder.exists():
            shutil.rmtree(folder)
        say(f'2/4 update: merging base + {expected["display_version"]} and verifying all IVFC levels')
        run([tree/'.venv/bin/python', 'scripts/extract_update_romfs.py', game, update,
             '--keys', keys, '--hactool', hactool, '--output', folder], logs/'update-romfs.log', cwd=tree)
        report = json.loads(index.read_text())
        check(f'{expected["display_version"]} RomFS', report['romfs_sha256'], expected['romfs']['sha256'])
        check(f'{expected["display_version"]} RomFS size', report['romfs_size'], expected['romfs']['size'])
    heapdef = mod_heapdef(mod)
    if heapdef:
        say(f'2/4 update: the overlays are built on the mod\'s heap sizes ({heapdef})')
    steps = [('make_romfs_patches.py', [])] + [('make_output_res_overlay.py', [size]) for size in OUTPUT_SIZES]
    if version == 'v143':   # a TOTK_RENDER_RES scene above 1600x900 grows the same heaps (runtime/game_patches_v143.c)
        steps += [('make_output_res_overlay.py', [canvas, '--scene', scene])
                  for canvas in ('1920x1080', *OUTPUT_SIZES) for scene in ('1920x1080', *OUTPUT_SIZES)]
    for i, (script, arguments) in enumerate(steps):
        if heapdef and script == 'make_output_res_overlay.py':   # every variant of the file keeps the mod's heap sizes
            arguments = [*arguments, '--heapdef', heapdef]
        run([tree/'.venv/bin/python', 'scripts/'+script, *arguments, '--version', version],
            logs/(version+'-'+script+(f'-{i}' if i > 1 else '')+'.log'), cwd=tree)


def build_spirv_cross(tree, logs):
    """The spirv-cross program with clang (the sources and flags of SPIRV-Cross's CMake Release build of it)."""
    src, out = tree / 'tooling/spirv-cross', tree / 'build/spirv-cross'
    objs = out / 'obj'
    objs.mkdir(parents=True, exist_ok=True)

    def compile_one(name):
        run(['xcrun', 'clang++', *SPIRV_CROSS_FLAGS, '-c', src / name, '-o', objs / (name + '.o')],
            logs / 'spirv-cross' / (name + '.log'), quiet=False)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        list(pool.map(compile_one, SPIRV_CROSS_SOURCES))
    run(['xcrun', 'clang++', '-arch', 'arm64', *[objs / (n + '.o') for n in SPIRV_CROSS_SOURCES], '-o', out / 'spirv-cross'],
        logs / 'spirv-cross' / 'link.log', quiet=False)


def check_translator(tree):
    """The shader translation service: a native program in this repository (no .NET needed to build or to play)."""
    translator = tree / 'prebuilt/macos-arm64/totk_xlate'
    want = (translator.parent / 'totk_xlate.sha256').read_text().strip()
    if not translator.is_file() or sha256(translator) != want:
        raise BuildError(f'{translator}: not the shader translator this repository ships (its SHA-256 differs from '
                         f'totk_xlate.sha256); check it out again, or rebuild it with games/totk/tools/totk_xlate/publish_native.sh')
    return translator


def step_shaders(tree, logs, *, version='v0'):
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        say('3/4 shaders: building SPIRV-Cross with clang')
        build_spirv_cross(tree, logs)
    translator = check_translator(tree)
    say('3/4 shaders: discovering and translating startup shaders from your game data')
    run([tree/'.venv/bin/python', 'scripts/prepare_shader_seed.py',
         '--romfs', tree/f'extracted/{version}/romfs', '--output', tree/f'build/shader-seed/{version}',
         '--translator', translator, '--spirv-cross', tree/'build/spirv-cross/spirv-cross',
         '--version', version, '--modules', tree/f'rewritten/{version}'], logs/'prepare-shader-seed.log', cwd=tree)


def developer_dirs():
    """Developer folders that may hold the Metal toolchain: DEVELOPER_DIR, the one xcode-select points at, Xcode*.app in
    /Applications, then any Xcode Spotlight knows (one kept outside /Applications, for example next to the game)."""
    found = [os.environ['DEVELOPER_DIR']] if os.environ.get('DEVELOPER_DIR') else []
    try:
        found.append(subprocess.check_output(['xcode-select', '-p'], text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        pass
    found += [str(p / 'Contents/Developer') for p in sorted(Path('/Applications').glob('Xcode*.app'))]
    found += [str(p / 'Contents/Developer') for p in sorted(Path.home().glob('Applications/Xcode*.app'))]
    # An Xcode kept on another volume, at its top or one folder down (Spotlight may not index it)
    found += [str(p / 'Contents/Developer') for p in sorted(Path('/Volumes').glob('*/Xcode*.app'))]
    found += [str(p / 'Contents/Developer') for p in sorted(Path('/Volumes').glob('*/*/Xcode*.app'))]
    try:
        spotlight = subprocess.check_output(['mdfind', "kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'"], text=True,
                                            stderr=subprocess.DEVNULL)
        found += [line + '/Contents/Developer' for line in spotlight.splitlines() if line.endswith('.app')]
    except (OSError, subprocess.CalledProcessError):
        pass
    return list(dict.fromkeys(d for d in found if d))


def metal_compiler():
    """(compiler, its version, the environment to run it with), or None. Apple's offline shader compiler comes with an
    Xcode's optional Metal toolchain, not the command line tools, so when the selected folder has none the other Xcodes
    on this Mac are tried."""
    for developer in developer_dirs():
        env = dict(os.environ, DEVELOPER_DIR=developer)
        found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True, env=env)
        metal = found.stdout.strip()
        if found.returncode or not metal:
            continue
        version = subprocess.run([metal, '--version'], capture_output=True, text=True, env=env)
        if not version.returncode and version.stdout:
            return metal, version.stdout.splitlines()[0], env
    return None


# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic.
METAL_FLAGS = ['-fmetal-math-mode=safe']


def variant_folder(game):
    """Where the game keeps the source of each patched shader variant it compiled while it was played (nvn_mtl.m
    variant_library: sampler border colours, missing varyings, the layer index, no depth output, MetalFX roles), named by
    its SHA-256: the player's shader cache, ~/Library/Application Support/MacNativeGames/<game>/shader-cache/rt-<chain>."""
    return Path.home()/'Library/Application Support/MacNativeGames'/APPS[game][2]/'shader-cache/rt-totk-native-v8-unverified/variants'


def variant_sources(game):
    """The recorded variant sources whose name is their SHA-256 (anything else is left out). Compiled ahead with the
    startup shaders, they are loaded by the game instead of compiled when a draw first needs them: 80-225 ms each on the
    submit thread with a cold Metal cache, and most of those draws cannot be deferred (2026-10-22)."""
    folder = variant_folder(game)
    found = sorted(folder.glob('*.metal')) if folder.is_dir() else []
    return [p for p in found if len(p.stem) == 64 and sha256(p) == p.stem]


def place_variant_libs(tree, res, version):
    """The compiled variants step_shader_libs listed, in the app's shader-libs beside the startup shaders'."""
    listed = tree/f'build/shader-libs/variants-{version}.json'
    for digest in json.loads(listed.read_text()) if listed.is_file() else []:
        lib = tree/f'build/shader-libs/{digest}.metallib'
        if len(digest) == 64 and all(c in '0123456789abcdef' for c in digest) and lib.is_file():
            clone(lib, res/'build/shader-libs'/lib.name)


def step_shader_libs(tree, logs, *, version='v0'):
    """Compile the startup shaders ahead of time. Metal's own cache of shaders compiled at launch does not follow the
    app to a folder whose path has another length (step_warm_metal); these files go inside the app. Each is named by the
    SHA-256 of its source, so the game loads only an exact match, and a rebuild reuses every shader that did not change."""
    sources = sorted((tree/f'build/shader-seed/{version}').glob('*.metal'))
    found = metal_compiler()
    if not found:
        say('3/4 shaders: no Metal toolchain (xcodebuild -downloadComponent MetalToolchain), so the startup shaders go '
            "into Metal's cache for the app's location at the end of the build instead; moving the app makes them cold again")
        return
    metal, compiler, metal_env = found
    libs = tree/'build/shader-libs'
    stamp = libs/'compiler.txt'
    identity = ' '.join([compiler, *METAL_FLAGS])
    if stamp.is_file() and stamp.read_text() != identity:
        shutil.rmtree(libs)
    libs.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)
    todo = {}
    known = seed_digests(tree/f'build/shader-seed/{version}')   # the digests the seed's preparation checked
    for src in sources:
        digest = seed_sha256(src.parent, src.name, known)
        if not (libs/f'{digest}.metallib').is_file():
            todo.setdefault(digest, src)
    variants = variant_sources('botw' if version.startswith('botw') else 'totk')
    for src in variants:
        if not (libs/f'{src.stem}.metallib').is_file():
            todo.setdefault(src.stem, src)
    say(f'3/4 shaders: compiling {len(todo)} of {len(sources)} startup shaders and {len(variants)} patched variants ahead of time')

    def compile_one(item):
        digest, src = item
        partial = libs/f'{digest}.partial'
        r = subprocess.run([metal, *METAL_FLAGS, '-o', partial, src], capture_output=True, text=True, env=metal_env)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src.name}\n{r.stdout}{r.stderr}'
        partial.replace(libs/f'{digest}.metallib')
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        failed = [f for f in pool.map(compile_one, todo.items()) if f]
    log = logs/'shader-libs.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text('\n'.join(failed))
    (libs/f'variants-{version}.json').write_text(json.dumps([p.stem for p in variants if (libs/f'{p.stem}.metallib').is_file()]))
    if failed:   # the game compiles these from source at launch, as before
        say(f'   {len(failed)} shaders did not compile ahead of time; they are compiled at launch ({log})')


def build_launcher(tree, version, logs, env):
    """build/app-launcher: per game and version (runtime/game_version.h: the data folder, the log name, the backup of saves)."""
    defines = [] if version == 'v0' else [f'-DGAME_{version.upper()}=1'] if version.startswith('botw') else [f'-DTOTK_GAME_{version.upper()}=1']
    run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Ithird_party/d2r_gfx/gfx/nvn', '-Iruntime', *defines, 'app/launcher.m', '-framework', 'AppKit',
         '-framework', 'GameController', '-framework', 'IOKit',
         '-o', 'build/app-launcher'], logs / 'launcher.log', cwd=tree, env=env)


def build_warm_tool(tree, logs, env):
    """build/warm-metal-cache: used only while packaging (step_warm_metal); not part of the app."""
    run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Wextra', 'tools/warm_metal_cache.m', '-framework', 'Metal',
         '-framework', 'Foundation', '-o', 'build/warm-metal-cache'], logs / 'warm-metal-cache-build.log', cwd=tree, env=env)


def step_binary(tree, game, keys, env, logs, *, version="v0", title='0100f2c0115b6000'):
    say('4/4 app: building the game binary and the launcher')
    run(['make', f'-j{os.cpu_count() or 4}', 'runtime' if version == 'v0' else f'runtime-{version}'], logs / 'runtime.log', cwd=tree, env=env)
    build_launcher(tree, version, logs, env)
    build_warm_tool(tree, logs, env)
    if not (tree / 'build/AppIcon.icns').exists():   # the game's own icon from its control data; optional
        try:
            run([tree / '.venv/bin/python', 'scripts/extract_app_icon.py', game, '--keys', keys, '--title', title, '--output', 'build/AppIcon.icns'],
                logs / 'app_icon.log', cwd=tree)
        except BuildError as e:
            say(f'   no app icon, the app gets the default one ({str(e).splitlines()[0]})')


_libc = None


def clonefile(src, dst):
    """macOS clonefile(2): an APFS clone of a file or a whole folder (no extra space) in one call, without starting a
    program for it (an app holds about 50,000 shader files per version: one cp each took minutes, 2026-10-19). False
    when it cannot: another volume or file system, or dst exists."""
    global _libc
    if _libc is None:
        import ctypes
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
    return _libc.clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0


def clone(src, dst):
    """APFS clone when on the same volume (no extra space), else a copy; an existing dst is replaced."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    if not clonefile(src, dst):
        shutil.copy2(src, dst)


def copy_tree(src, dst):
    """The files of src in dst: one clone of the whole folder when dst does not exist yet, else file by file."""
    if not dst.exists():
        dst.parent.mkdir(parents=True, exist_ok=True)
        if clonefile(src, dst):
            return
    for p in sorted(src.rglob('*')):
        if p.is_file():
            clone(p, dst / p.relative_to(src))


def remove_tree(path):
    """Remove a bundle. Finder can put .DS_Store back while it is being deleted, which makes the last rmdir fail."""
    for _ in range(5):
        try:
            shutil.rmtree(path)
            return
        except OSError as error:
            if error.errno != 66 or not path.exists():
                raise
            time.sleep(0.2)
    shutil.rmtree(path)


def data_bundle(tree, version, logs, *, bundle_id=BUNDLE_ID):
    """The RomFS image (16 GB) as a bundle of its own, signed once and kept in the work folder (build/data-bundles). The
    app carries a clone of it in Contents/Helpers, and the app's seal records only its signature: sealing the image
    inside the app hashed all 16 GB on every packaging (88 s on an M4 Mac mini, 2026-10-20), now only when the image
    changed (the stamp names the image: size, mtime and the SHA-256 the extraction verified, extracted/<version>/romfs/
    index.json). Not its inode, which an APFS clone of the work folder changes: a clone signed all 16 GB again (8 min on
    the external disk, 2026-10-22). The game opens the image through a link at its usual place,
    extracted/<version>/romfs/romfs.bin, and checks its SHA-256 itself as before (runtime/romfs.c)."""
    image = tree / f'extracted/{version}/romfs/romfs.bin'
    bundle = tree / f'build/data-bundles/GameData-{version}.bundle'
    stamp = bundle.with_name(bundle.name + '.json')
    st = image.stat()
    try:
        sha = json.loads((image.parent / 'index.json').read_text()).get('romfs_sha256')
    except (OSError, ValueError):
        sha = None
    want = dict(format=2, size=st.st_size, mtime_ns=st.st_mtime_ns, sha256=sha) if sha else \
        dict(format=1, size=st.st_size, mtime_ns=st.st_mtime_ns, inode=st.st_ino)
    try:
        have = json.loads(stamp.read_text())
        if (bundle / 'Contents/_CodeSignature/CodeResources').is_file():
            if have == want:
                return bundle
            if (want['format'] == 2 and have.get('format') == 1 and have.get('size') == st.st_size
                    and have.get('mtime_ns') == st.st_mtime_ns):   # an older stamp (or a clone's: other inode, same file)
                stamp.write_text(json.dumps(want))
                return bundle
    except (OSError, ValueError):
        pass
    say('4/4 app: signing the game data once (a minute or two; later builds reuse it)')
    tmp = bundle.with_name(bundle.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    clone(image, tmp / 'Contents/Resources/romfs.bin')
    import plistlib
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier=f'{bundle_id}.data.{version}', CFBundleName='Game data', CFBundlePackageType='BNDL',
        CFBundleShortVersionString=VERSIONS[version]['display_version'], CFBundleVersion='1')))
    run(['codesign', '--force', '--sign', '-', tmp], logs / f'sign-data-{version}.log')
    # Checked once here, where the 16 GB are hashed anyway; an app carrying this bundle checks only its own seal (which
    # records this bundle's signature), not --deep (55 s on each packaging, 2026-10-21).
    run(['codesign', '--verify', '--strict', tmp], logs / f'verify-data-{version}.log')
    stamp.unlink(missing_ok=True)
    if bundle.exists():
        shutil.rmtree(bundle)
    tmp.rename(bundle)
    write_stamp = stamp.with_name(stamp.name + '.tmp')
    write_stamp.write_text(json.dumps(want))
    write_stamp.replace(stamp)
    return bundle


GAME_DATA = ('runtime-tables.bin', 'romfs-config.txt', 'shared-fonts.txt', 'rewriter.txt')   # games/totk/runtime/game_data.h


def game_data_files(version):
    return GAME_DATA + (('aoc-config.txt',) if version.startswith('botw') else ())


def place_game_data(tree, res, version):
    """build/game-data/<version>: what the runtime reads from the player's files at start instead of having it compiled in
    (module tables, RomFS / add-on content / font sizes and hashes; games/totk/runtime/game_data.h), at the same place in
    the app. With it the seed's aliases, which place_runtime needs again when update_runtime.py exports a newer list."""
    data, dst = tree / f'build/game-data/{version}', res / f'build/game-data/{version}'
    missing = [name for name in game_data_files(version) if not (data / name).is_file()]
    if missing:
        raise BuildError(f'{data}: {", ".join(missing)} missing (games/totk/Makefile game-data-{version})')
    for name in game_data_files(version):
        clone(data / name, dst / name)
    aliases = tree / f'build/shader-seed/{version}.aliases.tsv'   # listed variants the seed carries under another name
    if aliases.is_file():
        clone(aliases, dst / 'shader-seed.aliases.tsv')


def place_runtime(tree, app, version, logs, *, game='totk', lists=True):
    """The parts of the app made from this repository alone, which update_runtime.py replaces in an installed app: the
    game binary, the launcher, the shader tools and the warm-up lists exported from the shared shader list. Everything
    else in the app comes from the player's game files (place_game_data, the rewritten modules, the RomFS, the seed).
    lists=False keeps the app's warm-up lists (update_runtime.py, when the seed's aliases are not known)."""
    res = app / 'Contents/Resources/totk'   # the launcher's game folder, for both games (app/launcher.m)
    clone(tree / ('build/totk-native' if version == 'v0' else f'build/totk-native-{version}'), res / 'build/totk-native')
    clone(tree / 'build/spirv-cross/spirv-cross', res / 'build/spirv-cross/spirv-cross')
    clone(tree / 'prebuilt/macos-arm64/totk_xlate', res / 'build/shader-service/totk_xlate')   # where totk_shader_rt.m looks for it
    for name in ('pso_manifest.tsv', 'pso_title.tsv') if lists else ():   # from the shared shader list (games/totk/Makefile)
        clone(tree / 'build' / name, res / 'build' / name)
    aliases = res / f'build/game-data/{version}/shader-seed.aliases.tsv'
    if lists and aliases.is_file():
        run([tree/'.venv/bin/python', 'scripts/shader_list.py', 'rename', aliases, res/'build/pso_manifest.tsv',
             res/'build/pso_title.tsv'], logs/'shader-list-rename.log', cwd=tree)
    clone(tree / 'build/app-launcher', app / f'Contents/MacOS/{APPS[game][2]}')


def place_data_bundle(bundle, app, res, version):
    """The signed game data in the app (Contents/Helpers), and the link the game opens it by."""
    copy_tree(bundle, app / 'Contents/Helpers' / bundle.name)
    link = res / f'extracted/{version}/romfs/romfs.bin'
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(os.path.relpath(app / 'Contents/Helpers' / bundle.name / 'Contents/Resources/romfs.bin', link.parent))


def library_game_data(game, version, base=None):
    """--mac-game-data library: where the launcher looks for the RomFS image of an app that carries none (app/launcher.m):
    <base>/<version>, base being the folder --mac-game-data-folder names (the app's Info.plist MNGGameDataFolder), else the
    game's data folder's GameData. One base can hold both games: their versions are named apart."""
    base = base or Path.home() / 'Library/Application Support/MacNativeGames' / APPS[game][2] / 'GameData'
    return base / version


def place_library_image(tree, game, version, base=None):
    """--mac-game-data library: the RomFS image in library_game_data instead of the app, put there once per image (an APFS
    clone when the work folder is on the same volume, else a 16 GB copy). romfs.sha256 is written last, so the launcher
    never takes a half-copied image; the image is not hashed here (the extraction checked it against the pin, and the
    runtime checks it once more at its first start). Returns whether it was put there now."""
    image, sha = tree / f'extracted/{version}/romfs/romfs.bin', VERSIONS[version]['romfs']['sha256']
    folder = library_game_data(game, version, base)
    target, mark = folder / 'romfs.bin', folder / 'romfs.sha256'
    try:
        if mark.read_text().strip() == sha and target.stat().st_size == image.stat().st_size:
            return False
    except OSError:
        pass
    mark.unlink(missing_ok=True)
    partial = folder / 'romfs.bin.partial'
    clone(image, partial)
    partial.replace(target)
    written = mark.with_name(mark.name + '.tmp')
    written.write_text(sha + '\n')
    written.replace(mark)
    return True


def app_seal_valid(app):
    """The app's own seal, which records each helper bundle's signature (not --deep: that hashes the 16 GB image)."""
    return subprocess.run(['codesign', '--verify', '--strict', app], capture_output=True).returncode == 0


class _AppMoves:
    """Keep rename inverses until publication finishes; do not discard the fresh data before then."""
    def __init__(self):
        self.done = []
        self.discard = []
        self.recovery_failed = False

    def move(self, source, target):
        source.rename(target)
        self.done.append((source, target))

    def restore(self, checkpoint=0):
        while len(self.done) > checkpoint:
            source, target = self.done[-1]
            try:
                if source.exists() or source.is_symlink():
                    raise OSError(f'recovery destination already exists: {source}')
                target.rename(source)
            except OSError as error:
                self.recovery_failed = True
                raise BuildError(f'App update rollback could not restore {target} to {source}: {error}. '
                                 'Files have been retained; recover them before rebuilding.') from error
            self.done.pop()

    def __enter__(self):
        return self

    def __exit__(self, kind, error, traceback):
        if kind is not None and not self.recovery_failed:
            self.restore()
        elif kind is None:
            for path in self.discard:
                try:
                    remove_tree(path)
                except OSError as cleanup_error:
                    say(f'Could not remove temporary game data {path}: {cleanup_error}')
        return False


def keep_game_data(new, old, *, verify=app_seal_valid, _moves=None):
    """When the app being replaced carries the same signed game data as the new one (the same signature, which names the
    image's SHA-256, the same Info.plist and image size), move its data bundle into the new app in place of the fresh
    clone. The image keeps its inode and times, so the runtime's RomFS verification record (runtime/romfs.c, keyed by
    them) still matches and the first start after a packaging does not hash 16 GB again (19.5 s, 2026-10-21). An image
    changed since keeps nothing either: its modification time moved, so the runtime hashes it and refuses it. Called when
    the old app no longer runs, just before it is replaced; if the new app's seal does not verify, the clone goes back."""
    if _moves is None:
        with _AppMoves() as moves:
            return keep_game_data(new, old, verify=verify, _moves=moves)
    image = 'Contents/Resources/romfs.bin'
    kept = False
    for bundle in sorted((new / 'Contents/Helpers').glob('GameData-*.bundle')):
        before = old / 'Contents/Helpers' / bundle.name
        if not all((before / rel).is_file() and (before / rel).read_bytes() == (bundle / rel).read_bytes()
                   for rel in ('Contents/_CodeSignature/CodeResources', 'Contents/Info.plist')):
            continue
        if not (before / image).is_file() or (before / image).stat().st_size != (bundle / image).stat().st_size:
            continue
        fresh = new.with_name(f'{new.name}.{bundle.name}.fresh')   # outside the app: anything extra in it fails the seal
        if fresh.exists():
            remove_tree(fresh)
        checkpoint = len(_moves.done)
        _moves.move(bundle, fresh)
        try:
            _moves.move(before, bundle)
        except OSError:
            _moves.restore(checkpoint)
            continue
        if not verify(new):
            _moves.restore(checkpoint)
            continue
        _moves.discard.append(fresh)
        kept = True
    return kept


def sign_app(app, logs):
    # Seal the bundle, including Info.plist and game resources. Copied helper
    # executables keep their existing signatures and byte identity. Checked once by install_app.
    run(['codesign', '--force', '--sign', '-', app], logs / 'sign-app.log')


def remove_in_background(path):
    """Delete a replaced app after the new one is in place, without waiting: its 90,000 shader files took 25 s."""
    subprocess.Popen(['rm', '-rf', str(path)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


def install_app(new, out, *, verify=None, remove=None):
    """Put the signed app `new` at `out`. Its seal is checked once, before the installed app is touched: by keep_game_data
    when the installed game data is kept, else here (a seal that does not verify stops the build and leaves the installed
    app as it is). The installed .app folder stays and takes the new contents; the replaced contents go to a folder whose
    name is not an app's, deleted in the background with any left by an earlier packaging. Returns whether the installed
    game data was kept."""
    verify, remove = verify or app_seal_valid, remove or remove_in_background
    with _AppMoves() as moves:
        kept = out.exists() and keep_game_data(new, out, verify=verify, _moves=moves)
        if not kept and not verify(new):
            raise BuildError(f'{new}: the new app\'s signature does not verify (codesign --verify --strict); the installed app is unchanged')
        leftovers = sorted(out.parent.glob(f'{glob.escape(out.name)}.replaced-*'))
        if out.exists():
            # The .app folder itself stays: macOS keeps its first-run check of an app (Gatekeeper / XProtect read the whole
            # bundle, 185-200 s for the 17 GB one) with the folder. A new folder with the same contents was scanned again,
            # new Contents in the same folder were not (2026-10-22). Everything in it moves out, the new Contents move in.
            old = out.with_name(f'{out.name}.replaced-{os.getpid()}')
            if old.exists():
                remove_tree(old)
            old.mkdir()
            for entry in list(out.iterdir()):
                moves.move(entry, old / entry.name)
            for entry in list(new.iterdir()):
                moves.move(entry, out / entry.name)
            new.rmdir()
            leftovers.append(old)
        else:
            moves.move(new, out)
    for path in leftovers:
        remove(path)
    return kept


def place_seed(tree, res, version, *, game='totk'):
    """The startup shader seed, checked (version, translation chain, size, every file's SHA-256), with the libraries
    step_shader_libs compiled for it and for the recorded variants. Also update_runtime.py --seed."""
    seed = tree/f'build/shader-seed/{version}'
    if not (seed/'seed-manifest.json').is_file():
        raise BuildError('startup shader seed was not prepared; refusing an app without first-launch warm-up data')
    try:
        report = json.loads((seed/'seed-manifest.json').read_text())
        known = seed_digests(seed)
        if (report['version'] != version or report['chain'] != 'totk-native-v8-unverified' or
                report['shaders'] < (8000 if game == 'totk' else 2000) or not report['files'] or any(
                    Path(name).name != name or seed_sha256(seed, name, known) != digest for name, digest in report['files'].items())):
            raise ValueError('version, translation chain or file hash differs')
    except (ValueError, KeyError, OSError, TypeError) as error:
        raise BuildError(f'invalid prepared startup shader seed: {error}') from None
    copy_tree(seed, res/'build/shader-seed')
    for src in sorted(seed.glob('*.metal')):   # each named in the manifest, its hash just checked
        lib = tree/f'build/shader-libs/{report["files"][src.name]}.metallib'
        if lib.is_file():
            clone(lib, res/'build/shader-libs'/lib.name)
    place_variant_libs(tree, res, version)


def wait_until_closed(out, prefix=''):
    """Preparing/signing a replacement is safe while playing; replacing the resources of an active game is not. Wait
    without closing the player."""
    old_binary = out/'Contents/Resources/totk/build/totk-native'
    notified = 0
    while old_binary.is_file():
        processes = subprocess.run(['ps', '-axo', 'comm='], capture_output=True, text=True, check=True).stdout.splitlines()
        if str(old_binary.resolve()) not in (p.strip() for p in processes):
            break
        if time.monotonic()-notified >= 60:
            say(f'{prefix}replacement is ready; waiting for the running game to close before updating it')
            notified = time.monotonic()
        time.sleep(2)


def source_commit():
    """The short hash of the commit this checkout is on (user 2026-10-25: the HUD shows it, to tell which commit an app
    was built from; uncommitted changes are not shown), or '' outside a git checkout."""
    result = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=7', 'HEAD'], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ''


def source_revision():
    """The full hash of the commit this checkout is on (Info.plist MNGSourceRevision: app/update_check.h compares it
    against GitHub main to offer updates), or '' outside a git checkout. Identifies the source baseline, not the
    Nintendo game version; local patches may also be present."""
    try:
        revision = subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ''  # Source archives without Git metadata simply do not check for updates.
    return revision if len(revision) == 40 and all(c in '0123456789abcdef' for c in revision) else ''


def write_main_build_id(nso, target):
    if not nso.is_file():
        return
    with nso.open('rb') as f:
        f.seek(0x40)
        build_id = f.read(20)
    if len(build_id) != 20:
        raise BuildError(f'{nso}: too short to hold a build ID')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(build_id.hex().upper() + '\n')


def step_app(tree, out, logs, *, version="v0", game='totk', game_data='app', game_data_folder=None):
    expected = VERSIONS[version]
    app_name, bundle_id, executable = APPS[game]
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        remove_tree(tmp)
    res = tmp / 'Contents/Resources/totk'   # the launcher's game folder, for both games (app/launcher.m)
    files = [f'rewritten/{version}/main.patched.bin', f'rewritten/{version}/sdk.patched.bin',
             f'rewritten/{version}/subsdk0.patched.bin', f'extracted/{version}/romfs/runtime-index.bin']   # romfs.bin: data_bundle
    for index in expected.get('aoc', {}):   # Only installed, verified expansion pass volumes.
        if not (tree / f'extracted/{version}/aoc/{index}/index.json').is_file():
            continue
        files += [f'extracted/{version}/aoc/{index}/romfs.bin', f'extracted/{version}/aoc/{index}/runtime-index.bin']
    for rel in files:
        clone(tree / rel, res / rel)
    write_main_build_id(tree / f'extracted/{version}/exefs/main', res / f'extracted/{version}/exefs/main.build-id')
    for rel in (f'extracted/{version}/romfs-patches', f'extracted/{version}/romfs-patches-out', 'extracted/firmware/shared-fonts'):
        if (tree / rel).is_dir():   # Breath of the Wild has no RomFS overlays
            copy_tree(tree / rel, res / rel)
    place_seed(tree, res, version, game=game)
    # after the checks above: signing it is slow
    place_game_data(tree, res, version)
    place_runtime(tree, tmp, version, logs, game=game)
    if game_data == 'library':
        folder = library_game_data(game, version, game_data_folder)
        say(f'4/4 app: the game data goes to {folder}, not into the app (put there once; later builds reuse it)')
        place_library_image(tree, game, version, game_data_folder)
    else:
        place_data_bundle(data_bundle(tree, version, logs, bundle_id=bundle_id), tmp, res, version)
    info = dict(CFBundleDevelopmentRegion='en', CFBundleExecutable=executable, CFBundleIdentifier=bundle_id, CFBundleName=app_name,
                CFBundleDisplayName=app_name, CFBundlePackageType='APPL', CFBundleShortVersionString=expected['display_version'],
                CFBundleVersion=str(expected['version'] + 1), LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSApplicationCategoryType='public.app-category.action-games', LSSupportsGameMode=True,
                GCSupportsControllerUserInteraction=True,
                CFBundleLocalizations=list(GAME_LANGUAGES) + (['pt-BR'] if version in ('v143', 'botw190') else []))
    icon = tree / 'build/AppIcon.icns'
    if icon.exists():
        clone(icon, tmp / 'Contents/Resources/AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    info['MNGContributionFolder'] = str(contrib_folder())   # app/launcher.m contrib_file: where players find it
    if game_data == 'library' and game_data_folder:
        info['MNGGameDataFolder'] = str(game_data_folder)   # app/launcher.m: where the image is, before the data folder's GameData
    commit = source_commit()
    if commit:
        info['MNGSourceCommit'] = commit   # app/launcher.m -> TOTK_SOURCE_COMMIT: the HUD box shows which commit the app is
    revision = source_revision()
    if revision:
        info['MNGSourceRevision'] = revision
    import plistlib
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    (tmp / 'Contents/PkgInfo').write_bytes(b'APPL????')
    sign_app(tmp, logs)  # A failed seal must not remove the previous working app.
    wait_until_closed(out, '4/4 app: ')
    if install_app(tmp, out):
        say('4/4 app: the game data is unchanged; kept the installed copy, so the first start need not verify it again')


def last_json(log):
    """The tool's report: its last line that is a JSON object."""
    for line in reversed(log.read_text(errors='replace').splitlines()):
        if line.startswith('{'):
            try: return json.loads(line)
            except ValueError: pass
    return None


def metal_cache_users():
    """Programs that have the Metal library cache of programs without an app bundle open, as 'name (pid)'. Whatever a
    program compiles there while another program that opened it first is running is lost when that other program quits
    (2026-10-18: 20 new libraries found while it ran, 2 after it quit; a packaging run with a short-lived probe running at
    its start kept 1 of 64)."""
    try:
        cache = subprocess.run(['getconf', 'DARWIN_USER_CACHE_DIR'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return []
    files = sorted(str(f) for f in Path(cache, 'com.apple.metal').glob('*/libraries.*'))
    if not files:
        return []
    try:
        listed = subprocess.run(['lsof', '-F', 'pc', '--', *files], capture_output=True, text=True).stdout
    except OSError:
        return []
    users, pid = set(), None
    for line in listed.splitlines():
        if line.startswith('p'):
            pid = line[1:]
        elif line.startswith('c') and pid:
            users.add(f'{line[1:]} ({pid})')
    return sorted(users)


def step_warm_metal(tree, out, logs):
    """Compile the app's startup shaders once into this Mac's Metal cache (2026-10-18). The game compiles each of them from
    source when it first meets it, and Metal keeps the result keyed by the source text, the compile options and the length
    of the path of the compiling program's folder (only its length: another folder with a path as long finds them too);
    on a Mac that has never compiled them (a new Mac, after the system purged its cache, or after the app moved) the game's
    background warm-up spends about 3 minutes (M4, 8 threads) compiling them, and the world stutters if the player goes
    in before it is done. The tool (games/totk/tools/warm_metal_cache.m) compiles the same files with the same options from
    the game binary's own folder, and is removed again afterwards (the app's signature seals that folder). It runs only
    while no other program has Metal's cache open (what it compiled would be lost when that program quits), and the
    result is checked in a new process. Nothing here can make the build fail: without it the game compiles the shaders
    itself, as before."""
    build = out / 'Contents/Resources/totk/build'
    seed = build / 'shader-seed'
    tool = build / 'warm-metal-cache'   # next to the game binary: Metal matches the length of the program's folder path
    tool.unlink(missing_ok=True)        # left by an interrupted build
    sources = sorted(seed.glob('*_b.metal'))
    if sources and all((build / 'shader-libs' / f'{sha256(src)}.metallib').is_file() for src in sources):
        say('4/4 app: the startup shaders are compiled into the app (step_shader_libs), so Metal\'s cache needs no warming')
        return
    users = metal_cache_users()
    if users or subprocess.run(['pgrep', '-q', 'totk-native']).returncode == 0:
        who = ', '.join(users) if users else 'a game'
        say(f"4/4 app: {who} has Metal's shader cache open, so the startup shaders are compiled on the first start instead "
            '(what this build compiled now would be lost when that program quits)')
        return
    # about 46 a second on an M4 Max (#410: 1.4.3's 33,266 in 720 s); 8 GB Macs swap and take much longer
    say(f"4/4 app: compiling the {len(sources):,} prepared shaders for this Mac's GPU once (about {max(1, round(len(sources) / 46 / 60))} "
        "minutes on an M4 Max, much longer on an 8 GB Mac; saved on the first start)")
    t0 = time.time()
    try:
        clone(tree / 'build/warm-metal-cache', tool)
        run([tool, seed], logs / 'warm-metal-cache.log')
        run([tool, seed, '--verify', '64'], logs / 'warm-metal-cache-verify.log', quiet=False)
    except (BuildError, OSError) as e:
        say(f'   not done, the first start compiles them instead: {str(e).splitlines()[0]}')
        return
    finally:
        tool.unlink(missing_ok=True)
    done, sample = last_json(logs / 'warm-metal-cache.log'), last_json(logs / 'warm-metal-cache-verify.log')
    if not done or not sample:
        say(f'   no report from the tool, the first start compiles them instead (see {logs / "warm-metal-cache.log"})')
        return
    if done.get('failed'):
        say(f"   {done['failed']} shaders do not compile on this Mac (the game reports them in its log too); see {logs / 'warm-metal-cache.log'}")
    if sample['sampled'] and sample['hits'] * 10 >= sample['sampled'] * 9:
        say(f"   {done['compiled']} shaders compiled in {time.time() - t0:.0f} s; a new program finds them in Metal's cache "
            f"({sample['hits']} of {sample['sampled']} sampled, median {sample['median_ms']:.1f} ms). Moving the app makes them "
            f"cold again.")
    else:
        users = metal_cache_users()
        why = (f"{', '.join(users)} has Metal's cache open now" if users else
               'another program probably used Metal\'s cache meanwhile; build again with no other program running to retry')
        say(f"   compiled, but Metal did not keep them ({sample['hits']} of {sample['sampled']} sampled found, median "
            f"{sample['median_ms']:.0f} ms; {why}); the first start compiles them instead")


# ---- Breath of the Wild (games/botw) ----
def identify_game(tree, game, keys, logs):
    """'totk' or 'botw' from the title IDs of the NCAs in the game file (scripts/identify_game.py); the name is not used."""
    log = logs / 'identify-game.log'
    run([tree / '.venv/bin/python', 'scripts/identify_game.py', game, '--keys', keys], log, cwd=tree, quiet=False)
    report = last_json(log)
    if not report:
        raise BuildError(f'could not read the game file (see {log})')
    found = sorted({TITLES[t] for t in report['titles'] if t in TITLES})
    if not found:
        for title_id in ticket_title_ids(game):
            application, kind = title_kind(title_id)
            if kind == 'dlc' and application in TITLES:
                raise BuildError(f'{game.name} is {APPS[TITLES[application]][0]} add-on content (DLC), not the game' +
                                 (': pass the base NSP as the game file with --update <update.nsp>, and this file with --dlc'
                                  if TITLES[application] == 'botw' else ', which this build does not support'))
    if len(found) != 1:
        raise BuildError(f'the game file holds {report["titles"]}: expected Tears of the Kingdom (0100F2C0115B6000) or '
                         f'Breath of the Wild (01007EF00011E000)')
    return found[0]


# Ticket body offsets by signature type, as common_tickets in games/totk/scripts/extract_exefs.py (little-endian type:
# RSA-4096, RSA-2048, ECDSA with SHA-1 at 0x10000-0x10002, the same three with SHA-256 at 0x10003-0x10005). Only the rights ID is read here, never the title key.
TICKET_BODIES = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}


def ticket_title_ids(path):
    """The title IDs (16 hex digits, uppercase) in the rights IDs of the .tik files of a PFS0 package; [] for an XCI,
    a package without tickets and a file that is not readable as PFS0."""
    ids = []
    try:
        with path.open('rb') as f:
            head = f.read(16)
            if len(head) < 16 or head[:4] != b'PFS0':
                return []
            count, strings_size = struct.unpack_from('<II', head, 4)
            table = f.read(count * 24)
            strings = f.read(strings_size)
            if len(table) < count * 24 or len(strings) < strings_size:
                return []
            data_start = 16 + count * 24 + strings_size
            for i in range(count):
                offset, size, name_at, _ = struct.unpack_from('<QQII', table, i * 24)
                name = strings[name_at:strings.find(b'\0', name_at)] if name_at < len(strings) else b''
                if not name.lower().endswith(b'.tik') or size < 4:
                    continue
                f.seek(data_start + offset)
                sig = f.read(4)
                body = TICKET_BODIES.get(struct.unpack('<I', sig)[0]) if len(sig) == 4 else None
                if body is None or size < body + 0x168:
                    continue
                f.seek(data_start + offset + body + 0x160)
                rights = f.read(8)
                if len(rights) == 8:
                    ids.append(rights.hex().upper())
    except (OSError, struct.error):
        return []
    return ids


def title_kind(title_id):
    """(application ID, 'base' | 'update' | 'dlc') of a title ID: base ends in 000, update in 800, DLC is the rest
    and belongs to the application 0x1000 below its 4096-title block."""
    value = int(title_id, 16)
    low = value & 0xFFF
    if low == 0x000:
        return f'{value:016X}', 'base'
    if low == 0x800:
        return f'{value & ~0xFFF:016X}', 'update'
    return f'{(value & ~0xFFF) - 0x1000:016X}', 'dlc'


def check_package(path, game, role):
    """Stop early when a ticketed package is of the other game or of another kind than its role ('base' or 'update').
    An XCI, a rights-free package and a title of no known game pass: the NCA checks of the extraction scripts follow."""
    flags = {'base': 'the game file', 'update': '--update', 'dlc': '--dlc'}   # where each kind belongs on the command line
    for title_id in ticket_title_ids(path):
        application, kind = title_kind(title_id)
        owner = TITLES.get(application)
        if owner is None:
            continue
        if owner != game:
            raise BuildError(f'{path.name} is {APPS[owner][0]} ({kind}), but this build is for {APPS[game][0]}: use the files of one game')
        if kind != role:
            raise BuildError(f'{path.name} is {APPS[game][0]} {kind} content, not the {role}: pass it as {flags[kind]}')


def check_dlc(paths, expected):
    """Stop early when a --dlc package is not an add-on pack this version pins, or when two files are the same pack."""
    pins = {pack['title_id']: i for i, pack in expected['aoc'].items()}
    owner = {}
    for path in paths:
        check_package(path, 'botw', 'dlc')
        for title_id in ticket_title_ids(path):
            if title_id not in pins:
                raise BuildError(f'{path.name} is add-on content {title_id}, but the packs of this version are {", ".join(pins)}')
            if owner.setdefault(title_id, path) != path:
                raise BuildError(f'{owner[title_id].name} and {path.name} are the same DLC pack ({title_id}): pass one of them')


def dlc_pack_valid(folder, pin):
    try:
        report = json.loads((folder / 'index.json').read_text())
        return (report.get('status') == 'pass' and report['romfs_sha256'] == pin['sha256'] and (folder / 'runtime-index.bin').is_file()
                and (folder / 'romfs.bin').stat().st_size == pin['size'])
    except (OSError, ValueError, KeyError):
        return False


def step_dlc_botw(tree, botw, dlc, keys, logs, pins, aoc):
    """Add-on content of a base game with its separate 1.6.0 or 1.9.0 update: import each --dlc NSP (games/botw/scripts/extract_dlc.py) into aoc/<index> and check it against its
    pin. A valid pack from an earlier run is reused; a pack that is not given this time is removed, so the app holds the given
    packs only (the runtime takes the packs it finds: AOC_PRESENT_MASK)."""
    titles = {pack['title_id']: i for i, pack in pins.items()}
    staging = aoc.with_name('aoc-import')
    keep = set()
    for nsp in dlc:
        index = next((titles[t] for t in ticket_title_ids(nsp) if t in titles), None)
        if index is not None and dlc_pack_valid(aoc / index, pins[index]):
            keep.add(index)
            continue
        say(f'2/4 data: importing the add-on content {nsp.name}')
        shutil.rmtree(staging, ignore_errors=True)
        try:
            run([tree / '.venv/bin/python', botw / 'scripts/extract_dlc.py', nsp, '--keys', keys, '--output', staging], logs / f'botw-dlc-{nsp.stem}.log', cwd=tree)
            (pack,) = list(staging.iterdir())
            report = json.loads((pack / 'index.json').read_text())
            index = str(report['index'])
            check(f'add-on content {index}', report['romfs_sha256'], pins[index]['sha256'])
            if report['romfs_size'] != pins[index]['size']:
                raise BuildError(f'add-on content {index}: size {report["romfs_size"]} is not the expected {pins[index]["size"]}')
            shutil.rmtree(aoc / index, ignore_errors=True)
            aoc.mkdir(parents=True, exist_ok=True)
            pack.rename(aoc / index)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        keep.add(index)
    stale = [p for p in aoc.iterdir() if p.name not in keep] if aoc.is_dir() else []
    if stale:
        say(f'2/4 data: removing the add-on content not given with --dlc this time ({", ".join(sorted(p.name for p in stale))})')
    for p in stale:
        shutil.rmtree(p) if p.is_dir() else p.unlink()
    if stale:   # the Makefile only sees packs that appear, so the old mask goes with the pack
        (tree / 'build/game-data' / aoc.parent.name / 'aoc-config.txt').unlink(missing_ok=True)


def step_code_botw(tree, botw, game, keys, env, logs, *, version='botw160'):
    """The ExeFS of the card image or of the separate 1.6.0 or 1.9.0 update given as game (games/botw/scripts/extract_exefs.py
    reads the update's Program NCA, the one with a BKTR section), the three modules rewritten with the same rewriter as Tears of the Kingdom (it learnt
    this game's jump tables), each file checked against expected-v<version>.json."""
    expected = VERSIONS[version]
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    want = expected['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'
    if not source_matches(manifest, game) or not all((exefs / n).is_file() and sha256(exefs / n) == h for n, h in want.items()):
        say(f'1/4 code: extracting the {expected["display_version"]} ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        run([py, botw / 'scripts/extract_exefs.py', game, '--keys', keys, '--output', exefs], logs / f'extract_exefs-{version}.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not Breath of the Wild {expected["display_version"]}')
        check(f'ExeFS {n}', sha256(exefs / n), h)
    if not (tree / f'extracted/{version}/elf/main-inventory.json').exists():
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{version}/elf'], logs / f'prepare_modules-{version}.log', cwd=tree)
    out = tree / f'rewritten/{version}'
    out.mkdir(parents=True, exist_ok=True)
    for name in ('sdk', 'subsdk0', 'main'):
        image, h = out / f'{name}.patched.bin', expected['rewritten_sha256'][f'{name}.patched.bin']
        if image.exists() and sha256(image) == h:
            continue
        say(f'1/4 code: rewriting {name} for Apple silicon' + (' (the big one: about a minute)' if name == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', exefs / name, out, name], logs / f'rewrite_{version}_{name}.log', cwd=tree,
            env=dict(env, DM_RULESET='native', DM_PRIVATE_GATES='0'))
        check(f'rewritten {name}', sha256(image), h)
    # scripts/prepare_runtime.py binds the loader to these images (verify_rewrites.py --version <version> re-runs the site-by-site check)
    report = dict(format=1, status='pass', rom_version=version, scope=f'hash match with verified {expected["display_version"]} rewrite',
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': want[n] for n in ('main', 'sdk', 'subsdk0')},
                                      **{f'rewritten/{version}/{n}.patched.bin': expected['rewritten_sha256'][f'{n}.patched.bin'] for n in ('main', 'sdk', 'subsdk0')}})
    report_path = tree / f'analysis/{version}-port/phase12-validation.json'
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')


def step_data_botw(tree, botw, game, keys, logs, *, version='botw160', update=None, dlc=(), firmware=None, shared_fonts=None):
    """The base + update RomFS merged from the card image, or from a base game and its separate 1.6.0 or 1.9.0 update
    (games/botw/scripts/extract_romfs.py, hactool BKTR); the add-on content: the card image's volumes (extract_aoc_romfs.py),
    or with a separate 1.6.0 or 1.9.0 update the packs of the --dlc NSPs (extract_dlc.py; none is needed); and the shared fonts: neither the card's system update
    partition nor an NSP holds them, so they come from --shared-fonts, from --firmware or from a previous build in
    the work folder."""
    expected = VERSIONS[version]
    py = tree / '.venv/bin/python'
    folder = tree / f'extracted/{version}/romfs'
    index = folder / 'index.json'
    valid = False
    if index.is_file() and (folder / 'romfs.bin').is_file():
        try:
            report = json.loads(index.read_text())
            valid = (report.get('status') == 'pass' and report['romfs_sha256'] == expected['romfs']['sha256']
                     and (folder / 'romfs.bin').stat().st_size == expected['romfs']['size'])
        except (ValueError, KeyError):
            pass
    hactool = None
    if not valid:
        hactool = build_hactool(tree, logs)
        if folder.exists():
            shutil.rmtree(folder)
        say(f'2/4 data: merging the base RomFS with the {expected["display_version"]} update and verifying all IVFC levels (15 GB, about 5 minutes)')
        run([py, botw / 'scripts/extract_romfs.py', game, *(['--update', update] if update else []), '--keys', keys, '--hactool', hactool, '--output', folder], logs / 'botw-romfs.log', cwd=tree)
        report = json.loads(index.read_text())
        check(f'{expected["display_version"]} RomFS', report['romfs_sha256'], expected['romfs']['sha256'])
        check(f'{expected["display_version"]} RomFS size', report['romfs_size'], expected['romfs']['size'])
    aoc = tree / f'extracted/{version}/aoc'
    if update:   # a separate update: add-on content comes from the --dlc NSPs, and only from them (cached card packs go)
        step_dlc_botw(tree, botw, dlc, keys, logs, expected['aoc'], aoc)
    else:
        aoc_valid = True
        for i, want in expected['aoc'].items():
            try:
                report = json.loads((aoc / i / 'index.json').read_text())
                aoc_valid = aoc_valid and report.get('status') == 'pass' and report['romfs_sha256'] == want['sha256'] and \
                    (aoc / i / 'romfs.bin').stat().st_size == want['size']
            except (OSError, ValueError, KeyError):
                aoc_valid = False
        if not aoc_valid:
            hactool = hactool or build_hactool(tree, logs)
            if aoc.exists():
                shutil.rmtree(aoc)
            say('2/4 data: extracting the add-on content (the expansion pass, 1.4 GB)')
            run([py, botw / 'scripts/extract_aoc_romfs.py', game, '--keys', keys, '--hactool', hactool, '--output', aoc,
                 '--expected', botw / 'assets/expected-v1.6.0.json'], logs / 'botw-aoc.log', cwd=tree)
            for i, want in expected['aoc'].items():
                report = json.loads((aoc / i / 'index.json').read_text())
                check(f'add-on content {i}', report['romfs_sha256'], want['sha256'])
    fonts = tree / 'extracted/firmware/shared-fonts'
    if not fonts_valid(fonts):
        if shared_fonts:
            report = font_report(shared_fonts)
            if shared_fonts.resolve() != fonts.resolve():
                if fonts.exists():
                    shutil.rmtree(fonts)
                fonts.mkdir(parents=True)
                for f in report['fonts']:
                    name = f"{f['type']}-{f['name']}.bfttf"
                    shutil.copy2(shared_fonts / name, fonts / name)
            (fonts / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        elif firmware:
            say('2/4 data: extracting the system shared fonts from the firmware')
            if fonts.exists():
                shutil.rmtree(fonts)
            run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, '--firmware', firmware, '--output', fonts],
                logs / 'shared_fonts.log', cwd=tree)
        else:
            raise BuildError('Breath of the Wild needs --firmware <unpacked firmware NCA directory> or --shared-fonts <folder with '
                             'the six verified .bfttf files> (a Tears of the Kingdom build extracts them into its work folder, '
                             'extracted/firmware/shared-fonts); this card has no system update partition to take them from')
        validate_fonts(fonts)


def step_shaders_botw(tree, botw, logs, *, version='botw160'):
    """SPIRV-Cross and the translator as for Tears of the Kingdom; the startup seed from this game's archives and its
    shared shader list (games/botw/scripts/prepare_shader_seed.py, games/botw/assets/shader_list.tsv)."""
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        say('3/4 shaders: building SPIRV-Cross with clang')
        build_spirv_cross(tree, logs)
    translator = tree / 'prebuilt/macos-arm64/totk_xlate'
    want = (translator.parent / 'totk_xlate.sha256').read_text().strip()
    if not translator.is_file() or sha256(translator) != want:
        raise BuildError(f'{translator}: not the shader translator this repository ships (its SHA-256 differs from '
                         f'totk_xlate.sha256); check it out again, or rebuild it with games/totk/tools/totk_xlate/publish_native.sh')
    seed = tree / f'build/shader-seed/{version}'
    say('3/4 shaders: preparing the startup shaders from the game data and the shared shader list (a few minutes)')
    run([tree / '.venv/bin/python', botw / 'scripts/prepare_shader_seed.py', '--romfs', tree / f'extracted/{version}/romfs',
         '--output', seed, '--translator', translator, '--spirv-cross', tree / 'build/spirv-cross/spirv-cross',
         '--version', version, '--modules', tree / f'rewritten/{version}', '--shader-list', BOTW_LIST],
        logs / 'prepare-shader-seed-botw.log', cwd=tree)


def mac_game_data(args, device_app):
    """--mac-game-data and --mac-game-data-folder: 'app' or 'library', and the folder the image goes in (None: the default,
    library_game_data). Naming the folder is enough for 'library'."""
    folder = args.mac_game_data_folder.expanduser().resolve() if args.mac_game_data_folder else None
    mode = 'library' if folder else args.mac_game_data
    if mode != 'app' and device_app:
        raise BuildError('--mac-game-data is for the Mac app; an iPhone / iPad build keeps its game data outside the app with '
                         '--ios-game-data documents')
    if folder and not (folder.is_dir() or folder.parent.is_dir()):
        raise BuildError(f'--mac-game-data-folder {folder}: neither it nor the folder it would be made in is there '
                         f'(a disk that is not connected?)')
    return mode, folder


def parse_args(argv):
    ap = argparse.ArgumentParser(description='Build a native Mac app or unsigned iPhone/iPad IPA from TOTK base 1.0.0 (XCI/NSP), optionally with the separate 1.1.2, 1.2.1 or 1.4.3 update; or a Mac app from Breath of the Wild: a combined 1.6.0 card image, or a base NSP with its separate 1.6.0 or 1.9.0 update NSP and optional add-on content NSPs (--dlc).',
                                 epilog='example: python3 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys')
    ap.add_argument('game', type=Path, help='the game file (.xci or .nsp), base game 1.0.0 (Breath of the Wild: the 1.6.0 card image, or the base NSP with --update)')
    ap.add_argument('--update', type=Path, help='separate supported cumulative update NSP/NSZ; outputs the same single app (Breath of the Wild: the 1.9.0 update NSP, not for a card image)')
    ap.add_argument('--dlc', type=Path, action='append', metavar='DLC.nsp', help='Breath of the Wild with a separate 1.6.0 or 1.9.0 --update: an add-on content (DLC) NSP; repeat for the second pack (01007EF00011F001, 01007EF00011F002); each is optional, and a pack left out is not in the app; such a build takes its packs from --dlc only, also when the game file is a card image')
    ap.add_argument('--platform', choices=('macos','ios','visionos'), default='macos', help='build a macOS app (default), unsigned iPhone/iPad IPA, or native Vision Pro IPA')
    ap.add_argument('--ios-game-data', choices=('app', 'documents'), default='app',
                    help='iOS: carry the 17 GB RomFS image in the IPA (default), or leave it out for the app\'s Documents/GameData '
                         '(ios/tools/install_device.py --game-data copies it there once; later builds install in minutes)')
    ap.add_argument('--mac-game-data', choices=('app', 'library'), default='app',
                    help='Mac: carry the 16 GB RomFS image in the app (default), or keep it in ~/Library/Application Support/'
                         'MacNativeGames/<TOTK|BotW>/GameData/<version>, put there once (the app is then a few hundred MB, '
                         'and copies to another Mac without it)')
    ap.add_argument('--mac-game-data-folder', type=Path, metavar='FOLDER',
                    help='Mac: keep the RomFS image in FOLDER/<version> instead (implies --mac-game-data library), for example '
                         'on another disk; the app remembers the folder, and the launcher falls back to the default one '
                         '(on another Mac, say) when FOLDER has no complete image')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('-o', '--out', type=Path, help=f'the app or IPA to write (default depends on --platform; next to the game file)')
    ap.add_argument('--work', type=Path, help='work folder (default: TOTK_build next to the game file)')
    ap.add_argument('--mod', type=Path, metavar='FOLDER',
                    help='a mod folder prepared before the build: its exefs/ IPS / IPS32 / pchtxt patches for the main '
                         'module are merged into the code before it is rewritten (its romfs/ goes in with '
                         '--install-mod-romfs; docs/mods.md)')
    ap.add_argument('--install-mod-romfs', action='store_true',
                    help='with --mod: at the end of the build, replace <data>/mods/<version> with the mod\'s romfs/ '
                         'and its manifest.tsv, so the app plays with the mod. That folder is the player\'s: what it '
                         'held is moved next to it as <version>.backup-<date>-<time>, not deleted. Without this flag '
                         'the build leaves the folder alone and prints the two commands that fill it by hand')
    ap.add_argument('--shader-cache', type=Path, help=argparse.SUPPRESS)   # removed: the seed comes from the game data and the shared list
    ap.add_argument('--no-shader-reminder', action='store_true', help='iOS: do not install the Mac agent that asks, while an iPhone / iPad with the game is connected, to send the shaders the shared list lacks')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA directory providing the six expected shared fonts (needed for NSP unless --shared-fonts or valid cached fonts are available; Breath of the Wild 1.9.0 too)')
    fonts.add_argument('--shared-fonts', type=Path, help='previously extracted shared-font directory containing the six verified .bfttf files')
    return ap.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    device_app = args.platform in ('ios', 'visionos')
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    if not game.is_file():
        raise BuildError(f'no such file: {game}')
    if not keys.is_file():
        raise BuildError(f'no such file: {keys}')
    out = (args.out.expanduser().resolve() if args.out else game.parent / (f'{APP_NAME}.ipa' if device_app else f'{APP_NAME}.app'))
    work = (args.work.expanduser().resolve() if args.work else game.parent / 'TOTK_build')
    if out.suffix.lower() not in (('.ipa', '.app') if device_app else ('.app',)):   # iOS .app: unsigned folder for install_device.py
        raise BuildError(f'the {args.platform} output must end in {".ipa or .app" if device_app else ".app"}: {out}')
    kind = input_kind(game)
    if kind not in ('xci', 'nsp'):
        raise BuildError('the base game must be an uncompressed XCI or NSP; NSZ is supported for update packages')
    update = args.update.expanduser().resolve() if args.update else None
    if update and (not update.is_file() or input_kind(update) not in ('nsp', 'nsz')):
        raise BuildError('--update must be a separate cumulative NSP or NSZ')
    mod = args.mod.expanduser().resolve() if args.mod else None
    if mod and not ((mod / 'exefs').is_dir() or (mod / 'romfs').is_dir()):
        raise BuildError(f'--mod expects a mod folder prepared before the build (its files under exefs/ and/or romfs/): '
                         f'{mod} has neither. See docs/mods.md.')
    if args.install_mod_romfs and not mod:
        raise BuildError('--install-mod-romfs fills the player\'s mod folder from a mod\'s romfs/, so it needs --mod '
                         '<mod folder>; nothing was written')
    if args.install_mod_romfs and not (mod / 'romfs').is_dir():
        raise BuildError(f'--install-mod-romfs installs {mod.name}/romfs into the player\'s mod folder, and this mod has '
                         f'none; leave the flag out to merge its code patches only')
    args.mac_game_data, game_data_folder = mac_game_data(args, device_app)
    if args.install_mod_romfs and device_app:
        raise BuildError('an iPhone / iPad / Vision Pro build loads no player mod folder, so --install-mod-romfs has nothing to fill: '
                         'build the Mac app (leave out --platform ios) to install a mod\'s RomFS')
    code_mod = mod if mod and (mod / 'exefs').is_dir() else None   # a data-only mod has no code to merge
    dlcs = list(dict.fromkeys(d.expanduser().resolve() for d in args.dlc or []))
    for dlc in dlcs:
        if not dlc.is_file() or input_kind(dlc) != 'nsp':
            raise BuildError(f'--dlc must be an add-on content NSP: {dlc}')
    version = 'v0'
    firmware = args.firmware.expanduser().resolve() if args.firmware else None
    shared_fonts = args.shared_fonts.expanduser().resolve() if args.shared_fonts else None
    if firmware and not firmware.is_dir():
        raise BuildError(f'no firmware directory: {firmware}')
    if shared_fonts:
        font_report(shared_fonts)
    no_fonts = BuildError('NSP files do not include system shared fonts. Supply --firmware <unpacked firmware NCA directory> '
                          'or --shared-fonts <previously extracted font directory>; valid fonts in --work are also reused.')
    fonts_given = firmware or shared_fonts or fonts_valid(work / 'totk/extracted/firmware/shared-fonts')
    if kind == 'nsp' and not (fonts_given or (not args.work and fonts_valid(game.parent / 'BotW_build/totk/extracted/firmware/shared-fonts'))):   # Breath of the Wild's default work folder
        raise no_fonts
    py = prerequisites(args)
    def existing_volume_path(path):
        path=path.resolve()
        while not path.exists(): path=path.parent
        return path
    logs = work / 'logs'
    env = dict(os.environ, PATH=os.pathsep.join(TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))
    t0 = time.time()
    fresh_work = not work.exists()
    # The game decides the platforms, the work folder and the disk space needed, so it is identified before those checks.
    tree = prepare_work(work, py, logs)
    which = identify_game(tree, game, keys, logs)
    if kind == 'nsp' and which != 'botw' and not fonts_given:   # only Breath of the Wild's fonts passed the check above
        raise no_fonts
    if dlcs and which != 'botw':
        raise BuildError(f'{APPS[which][0]} has no DLC support: leave out --dlc')
    if which == 'botw':
        if args.platform == 'visionos':
            raise BuildError('Breath of the Wild has no native Vision Pro build yet: use --platform macos or ios')
        if fresh_work and not args.work:   # BotW_build next to the game file, not TOTK_build
            shutil.rmtree(work)
        if mod:
            raise BuildError('--mod: merging a mod\'s main-module patches is only wired for Tears of the Kingdom so far '
                             '(the patch sites of Breath of the Wild 1.6.0 are not listed in scripts/patch_sites.py)')
        if not args.work:
            work = game.parent / 'BotW_build'
            logs = work / 'logs'
            tree = prepare_work(work, py, logs)
        if not args.out:
            out = game.parent / f'{APPS["botw"][0]}.{"ipa" if args.platform == "ios" else "app"}'
        check_package(game, 'botw', 'base')
        if update:
            check_package(update, 'botw', 'update')
        elif kind != 'xci':
            raise BuildError('Breath of the Wild from a base NSP needs the 1.9.0 update (or the 1.6.0 update): pass it with '
                             '--update <update.nsp> (a card image already holds its 1.6.0 update)')
        check_dlc(dlcs, BOTW190)   # the files first; the packs are the same for both versions
        if update:   # the base game (NSP or XCI) with the separate 1.6.0 or 1.9.0 update
            update = prepare_update_container(tree, update, keys, logs)
            version = identify_botw_update(tree, update, keys, logs)
        else:   # a combined card image: base, 1.6.0 update and add-on content
            version = 'botw160'
        if dlcs and not update:
            raise BuildError('a Breath of the Wild card image holds its own add-on content, so leave out --dlc (or pass the base NSP '
                             'as the game file with --update <update.nsp>)')
        expected = VERSIONS[version]
        if shutil.disk_usage(existing_volume_path(work.parent if work.parent.exists() else game.parent)).free / 2**30 < 22 and \
                not (work / f'totk/extracted/{version}/romfs/romfs.bin').exists():
            raise BuildError('needs about 22 GB free for the work folder (the app itself is an APFS clone of it when on the same volume)')
        if args.shader_cache:
            say('--shader-cache is no longer needed: the startup shaders come from the game data and the shared shader list (games/botw/assets/shader_list.tsv)')
        botw = work / 'botw'
        say(f'Breath of the Wild {expected["display_version"]}; work folder {work}')
        step_code_botw(tree, botw, update or game, keys, env, logs, version=version)
        step_data_botw(tree, botw, game, keys, logs, version=version, update=update, dlc=dlcs, firmware=firmware, shared_fonts=shared_fonts)
        step_shaders_botw(tree, botw, logs, version=version)
        if not device_app:   # the Mac app's metallibs; build_ipa.py compiles the device's own (6 of a 21-minute iPhone build, 2026-10-28)
            step_shader_libs(tree, logs, version=version)
        # The work folder's copy of games/botw (prepare_work), relative: make splits a path with spaces into several targets
        step_binary(tree, game, keys, {**env, 'SHADER_LIST': '../botw/assets/shader_list.tsv'}, logs, version=version, title='01007ef00011e000')
        say(f'4/4 app: assembling {out}')
        if device_app:   # the same IPA tooling as Tears of the Kingdom (ios/tools/build_ipa.py --version botw160 / botw190)
            run([py, 'ios/tools/build_ipa.py', '--tree', tree, '--out', out, '--version', version,
                 '--logs', logs, '--app-name', APPS['botw'][2], '--bundle-id', APPS['botw'][1],
                 *(['--image-outside'] if args.ios_game_data == 'documents' else [])], logs / 'build-ipa.log', cwd=tree, env=env)
            say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
            say('Install it with games/totk/ios/tools/install_device.py' + (f' --game-data {tree}/extracted/{version}/romfs/romfs.bin'
                if args.ios_game_data == 'documents' else '') + '; saves and the shader cache stay in the app\'s Documents.')
            return
        step_app(tree, out, logs, version=version, game='botw', game_data=args.mac_game_data, game_data_folder=game_data_folder)
        run([LSREGISTER, '-f', out], logs / 'register-app.log')
        say('4/4 app: macOS checks the app once (seconds when this app folder was checked before, a few minutes for a new one)')
        t1 = time.time()
        r = subprocess.run([out / f'Contents/MacOS/{APPS["botw"][2]}', '--check'], capture_output=True, text=True, timeout=1800)
        if r.returncode: raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
        say(f'   checked in {time.time() - t1:.0f} s')
        step_warm_metal(tree, out, logs)
        say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
        say('Open it from Finder. The first start verifies the game data and any installed add-on content and keeps a record; '
            'saves, the shader cache and the log go to ~/Library/Application Support/MacNativeGames/BotW and ~/Library/Logs/MacNativeGames/botw.log.')
        return
    work_volume=existing_volume_path(work.parent if work.parent.exists() else game.parent)
    output_volume=existing_volume_path(out.parent if out.parent.exists() else game.parent)
    free=shutil.disk_usage(work_volume).free/2**30
    output_free=shutil.disk_usage(output_volume).free/2**30
    same_volume=os.stat(work_volume).st_dev==os.stat(output_volume).st_dev
    base_cached=(work/'totk/extracted/v0/romfs/romfs.bin').exists()
    say(f'work folder {work} ({free:.0f} GB free on its volume)')
    if device_app:
        if same_volume:
            required=(60 if update else 45) if not base_cached else (40 if update else 18)
        else:
            required=(40 if update else 25) if not base_cached else (25 if update else 18)
        if args.ios_game_data=='documents' and base_cached:
            required=10   # the IPA carries no 17 GB image
        if free<required:
            raise BuildError(f'iOS build needs about {required} GB free on the work/output volume for the IPA archive')
        if not same_volume and output_free<18:
            raise BuildError('the output volume needs about 18 GB free for the iOS IPA')
    else:
        if update and free<40 and not base_cached:
            raise BuildError('needs about 40 GB free for a fresh base + update build (19 GB extra with base data cached)')
        if free<25 and not base_cached:
            raise BuildError('needs about 25 GB free for the work folder (the app itself is an APFS clone of it when on the same volume)')
    if mod and args.install_mod_romfs:      # said before the long steps, so a mistake can still be stopped with Ctrl-C
        say(f'--install-mod-romfs: the player\'s mod folder for the version this build makes will be REPLACED by '
            f'{mod.name}/romfs (and its manifest.tsv) when the build ends; what it holds now is moved next to it as '
            f'<folder>.backup-<date>-<time>, not deleted. Without the flag the build leaves the folder alone.')
    if update:
        version, update = identify_update(tree, update, keys, logs)
    step_code(tree, game, keys, env, logs, mod=None if update else code_mod)
    if update:
        step_code(tree, update, keys, env, logs, version=version,
                  mod=code_mod)   # a mod is for the updated main, not for 1.0.0
    step_data(tree, game, keys, logs, firmware=firmware, shared_fonts=shared_fonts, mod=None if update else mod)
    if update:
        step_update_data(tree, game, update, keys, logs, version=version, mod=mod)
    if mod:
        check_mod_overlays(tree, mod, version)
    step_shaders(tree, logs, version=version)
    if not device_app:   # the Mac app's metallibs; build_ipa.py compiles the device's own
        step_shader_libs(tree, logs, version=version)
    step_binary(tree, game, keys, env, logs, version=version)
    say(f'4/4 app: assembling {out}')
    if device_app:
        run([py, 'ios/tools/build_ipa.py', '--tree', tree, '--out', out, '--version', version,
             '--logs', logs, '--app-name', IOS_APP_NAME, '--bundle-id', BUNDLE_ID, '--platform', args.platform,
             *(['--image-outside'] if args.ios_game_data == 'documents' else [])], logs/'build-ipa.log', cwd=tree, env=env)
        if not args.no_shader_reminder:
            # The iOS app never asks during play; while the device is connected, this Mac asks to send the shaders the
            # shared list lacks (ios/tools/shader_contrib_watch.py). Not essential: a failure only says so.
            try:
                run([py, 'ios/tools/shader_contrib_watch.py', 'install', '--store', contrib_folder()],   # in this clone, where players look
                    logs/'shader-contrib-watch.log', cwd=tree, env=env)
                say('   installed the Mac reminder for iPhone / iPad shader contributions (remove: python3 '
                    'games/totk/ios/tools/shader_contrib_watch.py uninstall)')
            except BuildError as e:
                say(f'   the Mac reminder for iPhone / iPad shader contributions was not installed: {e}')
    else:
        step_app(tree, out, logs, version=version, game_data=args.mac_game_data, game_data_folder=game_data_folder)
    # Refresh an existing app's version, executable and signing identity before
    # the next Finder launch; the desktop alias keeps pointing to this bundle.
    if args.platform=='macos': run([LSREGISTER, '-f', out], logs / 'register-app.log')
    # macOS scans a new app the first time it runs (minutes for 16 GB, with nothing on screen): run it once here, in its
    # check-only mode, so the player's first start is immediate.
    if args.platform=='macos':
        say('4/4 app: macOS checks the app once (seconds when this app folder was checked before, a few minutes for a new one)')
        t1 = time.time()
        r = subprocess.run([out / 'Contents/MacOS/TOTK', '--check'], capture_output=True, text=True, timeout=1800)
        if r.returncode: raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
        say(f'   checked in {time.time() - t1:.0f} s')
        step_warm_metal(tree, out, logs)
        if mod and args.install_mod_romfs:
            install_mod_romfs(mod, tree, version, out, py, logs)   # --install-mod-romfs: <data>/mods/<version> + its manifest
        elif mod:
            say_mod_copy_steps(mod, tree, version, out)            # the folder is the player's: only say how to fill it
    elif mod:
        say('4/4 app: an iPhone / iPad / Vision Pro build loads no player mod folder, so --mod merged its code patches only')
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
    if args.platform=='macos':
        say('Open it from Finder. Moving it afterwards (also to /Applications) keeps the macOS check when it stays on the same '
            'volume, but Metal then compiles the startup shaders again in the background on the next start (a few minutes): '
            'pass -o with the final location instead. The first start verifies the game data and loads the prepared startup '
            'shaders. Additional scene/binding variants are translated and cached as they appear.')
    else: say('Sign and install the unsigned app with games/totk/ios/tools/install_device.py. Requires iOS/iPadOS 26 or visionOS 26 and a supported game controller; verify gameplay on your device.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
