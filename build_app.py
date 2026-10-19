#!/usr/bin/env python3
"""MacNativeGames: turn your own XCI or NSP into a native Mac app (macOS only).

    python3 build_app.py <game.xci|game.nsp> -k <prod.keys> [-o "<output>.app"] [--work <folder>]

Supported: TOTK base 1.0.0 (0100F2C0115B6000), XCI or NSP; optional --update with a supported cumulative update.
NSPs need common tickets (unless rights-free) and a separate shared-font source. No iPhone/iOS builds are supported.
Nothing from the game is in this repository; everything is made from your files on your Mac. Key values are never printed.

Steps (each one checked against games/totk/assets/expected-v<version>.json, so a damaged dump or another version stops the
build at the first wrong file):
  1. code: ExeFS out of the XCI or NSP, the three code modules rewritten for Apple silicon (games/totk/third_party/d2r)
  2. data: RomFS (17 GB), the system shared fonts (from XCI firmware or an explicit font source), the RomFS overlays
  3. shaders: SPIRV-Cross (fetched at a pinned commit, compiled with clang) and the shader translator, which ships
     ready-made as a native program (games/totk/prebuilt/macos-arm64: vendor/ryujinx + patches + Service.cs, made with
     games/totk/tools/totk_xlate/publish_native.sh); startup shaders are extracted from your selected RomFS and
     translated into a bundled seed, ready for GPU warm-up even with an empty cache on the first launch
  4. the game binary, the launcher, and the app bundle (the 17 GB RomFS is an APFS clone when on the same volume)

Every intermediate file is in the work folder (default: "<game file folder>/TOTK_build"); a second run reuses
what is already done and correct. The finished app keeps no writable state: saves, the shader cache and logs go to
~/Library/Application Support/MacNativeGames/TOTK and ~/Library/Logs/MacNativeGames.

Needs: macOS 26 or later on Apple silicon, the Xcode command line tools, Homebrew with
    brew install opus libvpx python@3.14
Optional: Xcode's Metal toolchain (xcodebuild -downloadComponent MetalToolchain) compiles the startup shaders into the
app, so moving or rebuilding it does not repeat that work at launch.
"""
import argparse, concurrent.futures, hashlib, json, os, platform, shutil, subprocess, sys, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parent
GAME = REPO / 'games/totk'
EXPECTED = json.loads((GAME / 'assets/expected-v1.0.0.json').read_text())
UPDATED = json.loads((GAME / 'assets/expected-v1.1.2.json').read_text())
UPDATED143 = json.loads((GAME / 'assets/expected-v1.4.3.json').read_text())
VERSIONS = {'v0': EXPECTED, 'v112': UPDATED, 'v143': UPDATED143}
HACTOOL = ('https://github.com/SciresM/hactool.git', '1d64a83450e025622f3468c28fc4164dad2c5ef6')
APP_NAME = 'Tears of the Kingdom'
BUNDLE_ID = 'org.macnativegames.totk'
LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
# Pinned upstream sources fetched at build time (repository, commit)
SPIRV_CROSS = ('https://github.com/KhronosGroup/SPIRV-Cross.git', 'aa217aeb6c9f0ace7a0ab233b28807edf45eb165')
# The spirv-cross program: the sources and flags of SPIRV-Cross's own CMake Release build of it, so no cmake is needed
SPIRV_CROSS_SOURCES = ('main.cpp', 'spirv_cfg.cpp', 'spirv_cpp.cpp', 'spirv_cross.cpp', 'spirv_cross_parsed_ir.cpp',
                       'spirv_cross_util.cpp', 'spirv_glsl.cpp', 'spirv_hlsl.cpp', 'spirv_msl.cpp', 'spirv_parser.cpp',
                       'spirv_reflect.cpp')
SPIRV_CROSS_FLAGS = ('-O3', '-DNDEBUG', '-std=c++11', '-arch', 'arm64', '-Wno-deprecated-declarations')
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin']


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


def check(what, got, want):
    if got != want:
        raise BuildError(f'{what}: SHA-256 {got[:16]}... is not the expected {want[:16]}... '
                         f'(is this Tears of the Kingdom 1.0.0, and the dump complete?)')


# ---- prerequisites ----
def prerequisites(args):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise BuildError('needs a Mac with Apple silicon')
    major = int(platform.mac_ver()[0].split('.')[0] or 0)
    if major < 26:
        raise BuildError(f'needs macOS 26 or later (Metal 4); this is macOS {platform.mac_ver()[0]}')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise BuildError('the Xcode command line tools are missing: xcode-select --install')
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
    run(['rsync', '-a', *excludes, f'{GAME}/', f'{tree}/'], logs / 'sources.log')
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
    for version in ('v112', 'v143'):
        exefs = tree / f'extracted/{version}/exefs'
        if source_matches(exefs.parent / 'extraction-manifest.json', source) and all(
                (exefs / name).is_file() and sha256(exefs / name) == digest
                for name, digest in VERSIONS[version]['exefs_sha256'].items()):
            return version, source
    staging = tree / 'extracted/update-detection'
    if staging.exists(): shutil.rmtree(staging)
    run([tree / '.venv/bin/python', 'scripts/extract_exefs.py', source, '--keys', keys,
         '--update', '--output', staging / 'exefs'], logs / 'identify-update.log', cwd=tree)
    actual = {path.name: sha256(path) for path in (staging / 'exefs').iterdir()}
    version = next((version for version in ('v112', 'v143')
                    if actual == VERSIONS[version]['exefs_sha256']), None)
    if version is None:
        raise BuildError('unsupported update module hashes; expected 1.1.2 or 1.4.3 (the file name is not used to choose a version)')
    target = tree / f'extracted/{version}'
    target.mkdir(parents=True, exist_ok=True)
    if (target / 'exefs').exists(): shutil.rmtree(target / 'exefs')
    (staging / 'exefs').rename(target / 'exefs')
    (staging / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
    staging.rmdir()
    say(f'update identified by verified ExeFS hashes: {VERSIONS[version]["display_version"]}')
    return version, source


def font_report(folder):
    """Only known font bytes can be bound into this version's runtime."""
    rows = []
    for kind, (name, expected) in enumerate(EXPECTED['shared_fonts_sha256'].items()):
        path = folder / f'{kind}-{name}.bfttf'
        if not path.is_file():
            raise BuildError(f'missing shared font: {path}')
        check(f'shared font {name}', sha256(path), expected)
        size = path.stat().st_size
        rows.append(dict(type=kind, name=name, bfttf_bytes=size, font_bytes=size - 8, sha256=expected))
    return dict(status='pass', integrity='Each shared font matches the verified SHA-256 for this build', fonts=rows)


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


def step_code(tree, game, keys, env, logs, *, version="v0"):
    expected = VERSIONS[version]
    update_args = [] if version == "v0" else ["--update"]
    py = tree / '.venv/bin/python'
    exefs = tree / f'extracted/{version}/exefs'
    want = expected['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'
    if not source_matches(manifest, game) or not all((exefs / n).is_file() and sha256(exefs / n) == h for n, h in want.items()):
        say('1/4 code: extracting the ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        source = prepare_update_container(tree, game, keys, logs) if update_args else game
        run([py, 'scripts/extract_exefs.py', source, '--keys', keys, '--output', exefs, *update_args], logs / f'extract_exefs-{version}.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing; not the supported version')
        check(f'ExeFS {n}', sha256(exefs / n), h)
    if not (tree / f'extracted/{version}/elf/main-inventory.json').exists():
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{version}/elf'], logs / 'prepare_modules.log', cwd=tree)
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
    # The loader binds itself to these images (scripts/prepare_runtime.py): they are the same bytes as the rewrite that
    # was verified site by site (python3 games/totk/scripts/verify_rewrites.py re-runs that verification, hours).
    report = dict(format=1, status='pass', rom_version=version, scope=f'hash match with verified {expected["display_version"]} rewrite',
                  module_file_sha256={**{f'extracted/{version}/exefs/{n}': want[n] for n in ('main', 'sdk', 'subsdk0')},
                                      **{f'rewritten/{version}/{n}.patched.bin': expected['rewritten_sha256'][f'{n}.patched.bin'] for n in ('main', 'sdk', 'subsdk0')}})
    report_path = tree / ('analysis/phase12-validation.json' if version == 'v0' else f'analysis/{version}-port/phase12-validation.json')
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')


def step_data(tree, game, keys, logs, *, firmware=None, shared_fonts=None):
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
    if not (tree / 'extracted/v0/romfs-patches-out/2560x1440/manifest.tsv').exists():
        say('2/4 data: RomFS overlay for the 2560x1440 output (key 3 in the game)')
        run([py, 'scripts/make_output_res_overlay.py', '2560x1440'], logs / 'output_res_overlay.log', cwd=tree)


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


def step_update_data(tree, game, update, keys, logs, *, version='v112'):
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
    for script, arguments in [('make_romfs_patches.py', []), ('make_output_res_overlay.py', ['2560x1440'])]:
        run([tree/'.venv/bin/python', 'scripts/'+script, *arguments, '--version', version],
            logs/(version+'-'+script+'.log'), cwd=tree)


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


def step_shaders(tree, logs, *, version='v0'):
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        say('3/4 shaders: building SPIRV-Cross with clang')
        build_spirv_cross(tree, logs)
    # the shader translation service: a native program in this repository (no .NET needed to build or to play)
    translator = tree / 'prebuilt/macos-arm64/totk_xlate'
    want = (translator.parent / 'totk_xlate.sha256').read_text().strip()
    if not translator.is_file() or sha256(translator) != want:
        raise BuildError(f'{translator}: not the shader translator this repository ships (its SHA-256 differs from '
                         f'totk_xlate.sha256); check it out again, or rebuild it with games/totk/tools/totk_xlate/publish_native.sh')
    say('3/4 shaders: discovering and translating startup shaders from your game data')
    run([tree/'.venv/bin/python', 'scripts/prepare_shader_seed.py',
         '--romfs', tree/f'extracted/{version}/romfs', '--output', tree/f'build/shader-seed/{version}',
         '--translator', translator, '--spirv-cross', tree/'build/spirv-cross/spirv-cross',
         '--version', version], logs/'prepare-shader-seed.log', cwd=tree)


def metal_compiler():
    """Apple's offline shader compiler, or None: it comes with Xcode's optional Metal toolchain, not the command line tools."""
    found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True)
    metal = found.stdout.strip()
    if found.returncode or not metal:
        return None
    version = subprocess.run([metal, '--version'], capture_output=True, text=True)
    return (metal, version.stdout.splitlines()[0]) if not version.returncode and version.stdout else None


# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic.
METAL_FLAGS = ['-fmetal-math-mode=safe']


def step_shader_libs(tree, logs, *, version='v0'):
    """Compile the startup shaders ahead of time. Metal's own cache of shaders compiled at launch starts over when the
    app is moved or rebuilt; these files go inside the app. Each is named by the SHA-256 of its source, so the game
    loads only an exact match, and a rebuild reuses every shader that did not change."""
    sources = sorted((tree/f'build/shader-seed/{version}').glob('*.metal'))
    found = metal_compiler()
    if not found:
        say('3/4 shaders: no Metal toolchain (xcodebuild -downloadComponent MetalToolchain), so the startup shaders are '
            'compiled on the first launch, and again after the app is moved or rebuilt')
        return
    metal, compiler = found
    libs = tree/'build/shader-libs'
    stamp = libs/'compiler.txt'
    identity = ' '.join([compiler, *METAL_FLAGS])
    if stamp.is_file() and stamp.read_text() != identity:
        shutil.rmtree(libs)
    libs.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)
    todo = {}
    for src in sources:
        digest = sha256(src)
        if not (libs/f'{digest}.metallib').is_file():
            todo.setdefault(digest, src)
    say(f'3/4 shaders: compiling {len(todo)} of {len(sources)} startup shaders ahead of time')

    def compile_one(item):
        digest, src = item
        partial = libs/f'{digest}.partial'
        r = subprocess.run([metal, *METAL_FLAGS, '-o', partial, src], capture_output=True, text=True)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src.name}\n{r.stdout}{r.stderr}'
        partial.replace(libs/f'{digest}.metallib')
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        failed = [f for f in pool.map(compile_one, todo.items()) if f]
    log = logs/'shader-libs.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text('\n'.join(failed))
    if failed:   # the game compiles these from source at launch, as before
        say(f'   {len(failed)} shaders did not compile ahead of time; they are compiled at launch ({log})')


def step_binary(tree, game, keys, env, logs, *, version="v0"):
    say('4/4 app: building the game binary and the launcher')
    run(['make', f'-j{os.cpu_count() or 4}', 'runtime' if version == 'v0' else f'runtime-{version}'], logs / 'runtime.log', cwd=tree, env=env)
    run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Ithird_party/d2r_gfx/gfx/nvn', '-Iruntime', *([] if version == 'v0' else [f'-DTOTK_GAME_{version.upper()}=1']), 'app/launcher.m', '-framework', 'AppKit',
         '-o', 'build/app-launcher'], logs / 'launcher.log', cwd=tree, env=env)
    if not (tree / 'build/AppIcon.icns').exists():   # the game's own icon from its control data; optional
        try:
            run([tree / '.venv/bin/python', 'scripts/extract_app_icon.py', game, '--keys', keys, '--output', 'build/AppIcon.icns'],
                logs / 'app_icon.log', cwd=tree)
        except BuildError as e:
            say(f'   no app icon, the app gets the default one ({str(e).splitlines()[0]})')


def clone(src, dst):
    """APFS clone when on the same volume (no extra space), else a copy."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['cp', '-c', src, dst], capture_output=True).returncode:
        shutil.copy2(src, dst)


def copy_tree(src, dst):
    for p in sorted(src.rglob('*')):
        if p.is_file():
            clone(p, dst / p.relative_to(src))


def sign_app(app, logs):
    # Seal the bundle, including Info.plist and game resources. Copied helper
    # executables keep their existing signatures and byte identity.
    run(['codesign', '--force', '--sign', '-', app], logs / 'sign-app.log')
    run(['codesign', '--verify', '--deep', '--strict', app], logs / 'verify-app-signature.log')


def step_app(tree, out, logs, *, version="v0"):
    expected = VERSIONS[version]
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    res = tmp / 'Contents/Resources/totk'
    clone(tree / ('build/totk-native' if version == 'v0' else f'build/totk-native-{version}'), res/'build/totk-native')
    for rel in ('build/pso_manifest.tsv', 'build/spirv-cross/spirv-cross',
                f'rewritten/{version}/main.patched.bin', f'rewritten/{version}/sdk.patched.bin',
                f'rewritten/{version}/subsdk0.patched.bin', f'extracted/{version}/romfs/romfs.bin', f'extracted/{version}/romfs/runtime-index.bin'):
        clone(tree / rel, res / rel)
    clone(tree / 'prebuilt/macos-arm64/totk_xlate', res / 'build/shader-service/totk_xlate')   # where totk_shader_rt.m looks for it
    for rel in (f'extracted/{version}/romfs-patches', f'extracted/{version}/romfs-patches-out', 'extracted/firmware/shared-fonts'):
        copy_tree(tree / rel, res / rel)
    seed = tree/f'build/shader-seed/{version}'
    if not (seed/'seed-manifest.json').is_file():
        raise BuildError('startup shader seed was not prepared; refusing an app without first-launch warm-up data')
    try:
        report = json.loads((seed/'seed-manifest.json').read_text())
        if (report['version'] != version or report['chain'] != 'totk-native-v4-unverified' or
                report['shaders'] < 8000 or not report['files'] or any(
                    Path(name).name != name or sha256(seed/name) != digest for name, digest in report['files'].items())):
            raise ValueError('version, translation chain or file hash differs')
    except (ValueError, KeyError, OSError, TypeError) as error:
        raise BuildError(f'invalid prepared startup shader seed: {error}') from None
    copy_tree(seed, res/'build/shader-seed')
    for src in sorted(seed.glob('*.metal')):
        lib = tree/f'build/shader-libs/{sha256(src)}.metallib'
        if lib.is_file():
            clone(lib, res/'build/shader-libs'/lib.name)
    clone(tree / 'build/app-launcher', tmp / 'Contents/MacOS/TOTK')
    info = dict(CFBundleDevelopmentRegion='en', CFBundleExecutable='TOTK', CFBundleIdentifier=BUNDLE_ID, CFBundleName=APP_NAME,
                CFBundleDisplayName=APP_NAME, CFBundlePackageType='APPL', CFBundleShortVersionString=expected['display_version'],
                CFBundleVersion=str(expected['version'] + 1), LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True,
                LSApplicationCategoryType='public.app-category.action-games', LSSupportsGameMode=True,
                GCSupportsControllerUserInteraction=True)
    icon = tree / 'build/AppIcon.icns'
    if icon.exists():
        clone(icon, tmp / 'Contents/Resources/AppIcon.icns')
        info['CFBundleIconFile'] = 'AppIcon'
    import plistlib
    (tmp / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    (tmp / 'Contents/PkgInfo').write_bytes(b'APPL????')
    sign_app(tmp, logs)  # A failed seal must not remove the previous working app.
    old_binary = out/'Contents/Resources/totk/build/totk-native'
    notified = 0
    while old_binary.is_file():
        # Preparing/signing a replacement is safe while playing; replacing the
        # resources of an active game is not. Wait without closing the player.
        processes = subprocess.run(['ps', '-axo', 'comm='], capture_output=True, text=True, check=True).stdout.splitlines()
        if str(old_binary.resolve()) not in (p.strip() for p in processes):
            break
        if time.monotonic()-notified >= 60:
            say('4/4 app: replacement is ready; waiting for the running game to close before updating it')
            notified = time.monotonic()
        time.sleep(2)
    if out.exists():
        shutil.rmtree(out)
    tmp.rename(out)


def parse_args(argv):
    ap = argparse.ArgumentParser(description='Build one native macOS app from TOTK base 1.0.0 (XCI/NSP), optionally with the separate 1.1.2 or 1.4.3 update.',
                                 epilog='example: python3 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys')
    ap.add_argument('game', type=Path, help='the game file (.xci or .nsp), base game 1.0.0')
    ap.add_argument('--update', type=Path, help='separate supported cumulative update NSP/NSZ; outputs the same single app')
    ap.add_argument('-k', '--keys', type=Path, required=True, help='your prod.keys')
    ap.add_argument('-o', '--out', type=Path, help=f'the app to write (default: "{APP_NAME}.app" next to the game file)')
    ap.add_argument('--work', type=Path, help='work folder (default: TOTK_build next to the game file)')
    fonts = ap.add_mutually_exclusive_group()
    fonts.add_argument('--firmware', type=Path, help='unpacked firmware NCA directory providing the six expected shared fonts (needed for NSP unless --shared-fonts or valid cached fonts are available)')
    fonts.add_argument('--shared-fonts', type=Path, help='previously extracted shared-font directory containing the six verified .bfttf files')
    return ap.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    game, keys = args.game.expanduser().resolve(), args.keys.expanduser().resolve()
    if not game.is_file():
        raise BuildError(f'no such file: {game}')
    if not keys.is_file():
        raise BuildError(f'no such file: {keys}')
    out = (args.out.expanduser().resolve() if args.out else game.parent / f'{APP_NAME}.app')
    work = (args.work.expanduser().resolve() if args.work else game.parent / 'TOTK_build')
    if out.suffix != '.app':
        raise BuildError(f'the output must end in .app: {out}')
    kind = input_kind(game)
    if kind not in ('xci', 'nsp'):
        raise BuildError('the base game must be an uncompressed XCI or NSP; NSZ is supported for update packages')
    update = args.update.expanduser().resolve() if args.update else None
    if update and (not update.is_file() or input_kind(update) not in ('nsp', 'nsz')):
        raise BuildError('--update must be a separate cumulative NSP or NSZ')
    version = 'v0'
    firmware = args.firmware.expanduser().resolve() if args.firmware else None
    shared_fonts = args.shared_fonts.expanduser().resolve() if args.shared_fonts else None
    if firmware and not firmware.is_dir():
        raise BuildError(f'no firmware directory: {firmware}')
    if shared_fonts:
        font_report(shared_fonts)
    if kind == 'nsp' and not (firmware or shared_fonts or fonts_valid(work / 'totk/extracted/firmware/shared-fonts')):
        raise BuildError('NSP files do not include system shared fonts. Supply --firmware <unpacked firmware NCA directory> '
                         'or --shared-fonts <previously extracted font directory>; valid fonts in --work are also reused.')
    py = prerequisites(args)
    free = shutil.disk_usage(work.parent if work.parent.exists() else game.parent).free / 2**30
    say(f'work folder {work} ({free:.0f} GB free on its volume)')
    if update and free < 40 and not (work / 'totk/extracted/v0/romfs/romfs.bin').exists():
        raise BuildError('needs about 40 GB free for a fresh base + update build (19 GB extra with base data cached)')
    if free < 25 and not (work / 'totk/extracted/v0/romfs/romfs.bin').exists():
        raise BuildError('needs about 25 GB free for the work folder (the app itself is an APFS clone of it when on the same volume)')
    logs = work / 'logs'
    env = dict(os.environ, PATH=os.pathsep.join(TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))
    t0 = time.time()
    tree = prepare_work(work, py, logs)
    if update:
        version, update = identify_update(tree, update, keys, logs)
    step_code(tree, game, keys, env, logs)
    if update:
        step_code(tree, update, keys, env, logs, version=version)
    step_data(tree, game, keys, logs, firmware=firmware, shared_fonts=shared_fonts)
    if update:
        step_update_data(tree, game, update, keys, logs, version=version)
    step_shaders(tree, logs, version=version)
    step_shader_libs(tree, logs, version=version)
    step_binary(tree, game, keys, env, logs, version=version)
    say(f'4/4 app: assembling {out}')
    step_app(tree, out, logs, version=version)
    # Refresh an existing app's version, executable and signing identity before
    # the next Finder launch; the desktop alias keeps pointing to this bundle.
    run([LSREGISTER, '-f', out], logs / 'register-app.log')
    # macOS scans a new app the first time it runs (minutes for 16 GB, with nothing on screen): run it once here, in its
    # check-only mode, so the player's first start is immediate.
    say('4/4 app: macOS checks the new app once (a few minutes for the 16 GB bundle)')
    t1 = time.time()
    r = subprocess.run([out / 'Contents/MacOS/TOTK', '--check'], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
    say(f'   checked in {time.time() - t1:.0f} s')
    say(f'done in {(time.time() - t0) / 60:.0f} min: {out}')
    say('Open it from Finder (or move it to /Applications first: moving within one volume keeps the check). The first '
        'start verifies the game data, discovers the prepared startup shaders and warms them on your GPU. '
        'Additional scene/binding variants are translated and cached as they appear.')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
