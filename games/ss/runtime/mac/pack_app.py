#!/usr/bin/env python3
# Pack a self-contained, double-clickable Mac app of a game (the NSP script's --platform mac-app): the launcher (dm/mac/launcher.c, linked here with its settings window,
# dm/mac/Launcher.swift, and the iOS launcher's model and rows) as CFBundleExecutable, the Mac host beside it,
# and in Contents/Resources what the IPA carries (dm/ios/tools/build_ipa.sh): romfs.img (the RomFS image), msl/<b-chain | vk | gl>/ (the Mac shader libraries, the folder named as in the IPA),
# aoc/<index>/ (DLC), pso/ (an NVN game's pipeline archive made offline for this Mac's GPU, dm/gfx/tools/pso_offline.py: the launcher hands it to the host as DM_PSO_BUNDLE),
# and dm/ (the rewritten modules and their rewriter tables, nro.tsv, the profile's shader_map.tsv / pso_manifest.tsv / c1_full.tsv / cb_extent.tsv / shader_pairs.tsv / pso_pairs.tsv, font/, env.txt,
# launcher.json: the game's launcher settings, dm/games/settings.py, which the settings window and Mac front ends show),
# plus AppIcon.icns from the game's icon (sips, iconutil) and the Info.plist keys the launcher reads (DMTitleID, DMRomfsMarker, DMLanguage).
# The large folders are APFS clones (cp -c) when the app is on the same APFS volume as the build, so they take no space; plain copies otherwise.
# Signing: the inner host first, then the bundle (codesign --force --sign <identity>; '-' = ad-hoc), without the hardened runtime: the host maps the game's code executable, which the hardened
# runtime refuses without entitlements, and so the app cannot be notarized.
# Usage: pack_app.py <out.app> --host <dmhost> --patched <dir> --romfs <image or dir> --msl <dir> --tables <profile dir> --font <dir> --title-id <id> --name <name> [options]
#   Reuses <out.app> while <out.app>.key matches; otherwise writes <out.app>.partial and swaps it in at the end. Prints "step: ..." and "app: <path>[, reused (inputs unchanged)]".
import argparse, ctypes, hashlib, json, os, plistlib, re, shutil, stat, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import romfs as rfs
MODULE_FILES = ('*.patched.bin', '*.fixups.tsv', '*.gates.tsv', '*.cuts.tsv', '*.obs.tsv', '*.changed.tsv', 'nro.tsv')   # what the host reads from the module folder (build_ipa.sh copies the same tables but changed.tsv: the iPhone's cheats never write code)
MSL_FILES = ('*.metallib', '*.json', '*.tsv')   # what the host reads from msl/ (not the Mac build's .metal sources or _b_build.json)
# read beside DM_NVN_SHMAP; any of them may be absent (a Vulkan or GL game has no c1_full.tsv). The shader pairs are the pipeline prebuild's (nvn_mtl.m pso_prebuild): without them the
# app makes no pulling bases, a hybrid draw has no stand-in and a new pipeline compiles on the submit thread (build_ipa.sh copies them into the IPA the same way)
TABLES = ('shader_map.tsv', 'pso_manifest.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')
PSO_FILES = ('*.mtl4a', 'prebuilt.tsv', 'rest_pairs.tsv', 'archive.tsv')   # what the app takes of pso_offline.py's output folder
EXE = 'nxnative'   # the launcher's file name (Contents/MacOS/nxnative)
ICON_SIZES = (16, 32, 128, 256, 512)


def step(msg): print(f'step: {msg}', flush=True)


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def first_file(d):
    for root, _, files in os.walk(d):
        if files: return Path(root) / files[0]
    return None


def can_clone(src, dst_dir):   # whether src (a file such as a RomFS image, or a folder's first file) can be cloned into dst_dir (the same APFS volume)
    f = src if src.is_file() else first_file(src)   # os.walk of a file yields nothing
    if f is None: return False
    t = dst_dir / '.clone_test'
    try:
        if t.exists(): t.unlink()
        return ctypes.CDLL(None, use_errno=True).clonefile(str(f).encode(), str(t).encode(), 0) == 0
    finally:
        if t.exists(): t.unlink()


def tree_bytes(d):
    if d.is_file(): return d.stat().st_size
    return sum((Path(r) / f).stat().st_size for r, _, fs in os.walk(d) for f in fs)


def copy(srcs, dst, clone):   # cp -R of each source to dst (a new path for one folder, an existing folder for files); -c clones (batched under ARG_MAX)
    cmd = ['/bin/cp', '-R'] + (['-c'] if clone else [])
    pointer = ctypes.sizeof(ctypes.c_void_p)
    def arg_bytes(p): return len(os.fsencode(p)) + 1 + pointer
    # exec's limit includes encoded strings, NULs and argv/envp pointers. Reserve 2 KiB for alignment and platform overhead (as POSIX xargs does).
    env_bytes = sum(arg_bytes(f'{k}={v}') for k, v in os.environ.items()) + pointer
    budget = os.sysconf('SC_ARG_MAX') - env_bytes - sum(arg_bytes(p) for p in cmd + [dst]) - pointer - 2048
    batch, used = [], 0
    for src in srcs:
        size = arg_bytes(src)
        if size > budget: raise SystemExit(f'copy arguments exceed ARG_MAX for {src}')
        if batch and used + size > budget:
            run(cmd + batch + [dst]); batch, used = [], 0
        batch.append(src); used += size
    if batch: run(cmd + batch + [dst])


def min_os(host):   # the host's deployment target (LC_BUILD_VERSION minos): the launcher is built for the same and Info.plist says it
    m = re.search(r'cmd LC_BUILD_VERSION.*?minos (\S+)', run(['otool', '-l', host]), re.S)
    return m.group(1) if m else '14.0'


def swift_sources():   # the settings window, with the iOS launcher's model and rows
    return [HERE / 'Launcher.swift', HERE.parent / 'ios/Sources/LauncherSettings.swift', HERE.parent / 'ios/Sources/LauncherRow.swift']


def build_launcher(out, minos, clang, swiftc, sdk):   # launcher.c's main and the host's cloud-save code (its send after the host exits), linked by swiftc with the settings window (no Swift main: -parse-as-library)
    with tempfile.TemporaryDirectory(dir=out.parents[3]) as t:
        objs = [Path(t) / 'launcher.o', Path(t) / 'save_sync.o']
        for obj, src in zip(objs, (HERE / 'launcher.c', HERE.parent / 'host/save_sync.c')):
            run([clang, '-isysroot', sdk, '-arch', 'arm64', f'-mmacosx-version-min={minos}', '-O2', '-Wall', '-I', HERE.parent / 'host', '-c', '-o', obj, src])
        run([swiftc, '-sdk', sdk, '-target', f'arm64-apple-macos{minos}', '-O', '-swift-version', '5', '-parse-as-library', '-module-cache-path', Path(t) / 'module-cache',
             '-o', out, *swift_sources(), *objs, '-framework', 'CoreFoundation', '-framework', 'AppKit', '-framework', 'SwiftUI'])


def make_icns(jpg, out):   # the game's icon (256x256 JPEG) at every size an .icns holds
    with tempfile.TemporaryDirectory(dir=out.parent) as t:
        s = Path(t) / 'AppIcon.iconset'; s.mkdir()
        for n in ICON_SIZES:
            for k, suffix in ((1, ''), (2, '@2x')):
                run(['sips', '-s', 'format', 'png', '-z', n * k, n * k, jpg, '--out', s / f'icon_{n}x{n}{suffix}.png'])
        run(['iconutil', '-c', 'icns', s, '-o', out])


def settings_rules(listed):   # DMSettings: what the launcher accepts as each setting's saved choice (docs/frontends.md, Saved choices)
    rules = []
    for s in listed:
        if s['kind'] == 'range': rules.append({'env': s['env'], 'min': float(s['min']), 'max': float(s['max'])})
        elif s['kind'] == 'choice': rules.append({'env': s['env'], 'values': [o[0] for o in s['options']]})
        elif s['kind'] == 'toggle': rules.append({'env': s['env'], 'values': [s.get('on', '1'), s.get('off', '0')]})
    return rules


def info_plist(a, minos, icon):
    p = {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': a.bundle_id or f'org.nxnative.{a.title_id.lower()}',
         'CFBundleVersion': a.version or '1.0', 'CFBundleShortVersionString': a.version or '1.0', 'CFBundlePackageType': 'APPL', 'CFBundleExecutable': EXE,
         'CFBundleInfoDictionaryVersion': '6.0', 'LSMinimumSystemVersion': minos, 'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True,
         'LSApplicationCategoryType': a.category or 'public.app-category.games', 'DMTitleID': a.title_id.upper(), 'DMRomfsMarker': a.marker or '', 'DMLanguage': a.lang or 'en-US'}
    if icon: p['CFBundleIconFile'] = 'AppIcon'
    if a.settings:
        listed = json.loads(Path(a.settings).read_text(encoding='utf-8'))['settings']
        p['DMSettings'] = settings_rules(listed)
        lang = next((s['default'] for s in listed if s['env'] == 'DM_LANG'), None)
        if lang: p['DMLanguage'] = lang   # the Game language row's default: --lang, or the game's first language when it lacks that one (settings.py), with or without the window
    return p


def source_commit():   # DMCommit: the checkout that packed the app (scripts/bug_report.py reads it), short hash, +dirty with uncommitted changes to tracked files; None outside a checkout
    repo = HERE.parent.parent
    try:
        head = subprocess.run(['git', '-C', repo, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', repo, 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError): return None
    return head + ('+dirty' if dirty else '') if head else None


def sign(app, identity, codesign):   # the inner host first, then the bundle (its resources and the launcher); no --options runtime (see the top)
    host = app / 'Contents/MacOS/dmhost'
    for target in (host, app):
        try: run([codesign, '--force', '--sign', identity, target])
        except SystemExit as e:
            if 'detritus' not in str(e): raise
            run(['xattr', '-cr', app]); run([codesign, '--force', '--sign', identity, target])   # Finder information or resource forks on copied files: codesign refuses them


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def inventory(root):
    """Stat trees avoid reading multi-GB game data, and detect changes to executable modes in the app."""
    def walk_error(e): raise e
    paths = [root]
    for parent, dirs, files in (os.walk(root, onerror=walk_error) if root.is_dir() else []):
        paths.extend(Path(parent) / n for n in dirs + files)
    rows = []
    for p in sorted(paths):
        s = p.lstat()
        rows.append([str(p.relative_to(root)), s.st_mode, s.st_size, s.st_mtime_ns, s.st_ctime_ns,
                     os.readlink(p) if stat.S_ISLNK(s.st_mode) else ''])
    return rows


def selected_inputs(a):
    """Resolve once: these same lists supply both the reuse key and the copies."""
    for name in ('patched', 'romfs', 'msl', 'tables', 'font', 'aoc', 'pso'):
        p = getattr(a, name)
        if p and not (rfs.Path(p).is_dir() if name == 'romfs' else Path(p).is_dir()): raise SystemExit(f'no {name} directory at {p}')
    if a.marker and not (rfs.Path(a.romfs) / a.marker).exists(): raise SystemExit(f'no RomFS at {a.romfs} (missing {a.marker})')
    mods = sorted({p for pat in MODULE_FILES for p in Path(a.patched).glob(pat)})
    if not any(p.name.endswith('.patched.bin') for p in mods): raise SystemExit(f'no rewritten module in {a.patched}')
    for p in mods:
        if p.name.endswith('.patched.bin') and not p.with_name(p.name.removesuffix('.patched.bin') + '.fixups.tsv').is_file():
            raise SystemExit(f'no rewriter fixups for {p}')
    msls = sorted(p for pat in MSL_FILES for p in Path(a.msl).glob(pat) if p.is_file() and p.name != '_b_build.json')
    if not any(p.suffix == '.metallib' for p in msls): raise SystemExit(f'no shader libraries at {a.msl}')
    pso = sorted({p for pat in PSO_FILES for p in Path(a.pso).glob(pat)}) if a.pso else []
    if a.pso and (not any(p.suffix == '.mtl4a' for p in pso) or not (Path(a.pso) / 'prebuilt.tsv').is_file()):
        raise SystemExit(f'no pipeline archive with its prebuilt.tsv in {a.pso}')
    tables = [Path(a.tables) / n for n in TABLES if (Path(a.tables) / n).is_file()]
    pairs = Path(a.pso_pairs) if a.pso_pairs and not any(p.name == 'pso_pairs.tsv' for p in tables) else None
    fonts = sorted(Path(a.font).glob('*.ttf'))
    files = [Path(a.host)] + mods + msls + pso + tables + fonts + ([pairs] if pairs else [])
    files += [Path(p) for p in (a.env_file, a.settings, a.icon) if p]
    for p in files:
        if not p.is_file(): raise SystemExit(f'no input file at {p}')
    return mods, msls, pso, tables, pairs, fonts, files


def replace_app(tmp, out):
    if out.exists():
        # RENAME_SWAP exchanges complete directories atomically; a failed swap leaves the old app at out.
        swap = ctypes.CDLL(None, use_errno=True).renamex_np
        swap.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        swap.restype = ctypes.c_int
        if swap(os.fsencode(tmp), os.fsencode(out), 2):
            err = ctypes.get_errno(); raise OSError(err, os.strerror(err), str(out))
    else:
        tmp.rename(out)


def pack(a):
    out = Path(a.out).resolve()
    if out.suffix != '.app': raise SystemExit(f'the output must end in .app: {out}')
    key_file = out.with_name(out.name + '.key')
    try:
        previous = json.loads(key_file.read_text())
    except (OSError, ValueError): previous = None
    try:
        a = argparse.Namespace(**vars(a))
        for name in ('out', 'host', 'patched', 'romfs', 'msl', 'tables', 'font', 'aoc', 'pso', 'pso_pairs', 'env_file', 'settings', 'icon'):
            if getattr(a, name): setattr(a, name, str(Path(getattr(a, name)).resolve()))
        mods, msls, pso, tables, pairs, fonts, files = selected_inputs(a)
        clang = Path(run(['xcrun', '--find', 'clang']).strip()).resolve()
        swiftc = Path(run(['xcrun', '--find', 'swiftc']).strip())   # not resolved: the driver is a link to swift-frontend, which acts by its name
        sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()).resolve()
        if not clang.is_file() or not swiftc.is_file() or not sdk.is_dir(): raise SystemExit(f'no compiler or SDK at {clang}, {swiftc}, {sdk}')
        codesign = shutil.which('codesign')
        if not codesign: raise SystemExit('codesign is not available')
        codesign = Path(codesign).resolve()
        minos = min_os(a.host)
        sources = [Path(__file__).resolve(), HERE / 'launcher.c', HERE.parent / 'host/romfs_image.h', HERE.parent / 'host/save_sync.c', HERE.parent / 'host/save_sync.h', HERE.parent / 'tools/romfs.py'] + swift_sources()
        inputs = {'options': vars(a), 'files': [(str(p), p.stat().st_mode, digest(p), os.readlink(p) if p.is_symlink() else None) for p in files + sources],
                  'trees': [(p, inventory(Path(p))) for p in (a.romfs, a.aoc) if p],
                  'tools': [[str(c), c.stat().st_mode, c.stat().st_size, c.stat().st_mtime_ns, c.stat().st_ctime_ns, run([c, '--version']).strip()] for c in (clang, swiftc)] + [
                            str(sdk), run(['xcrun', '--sdk', 'macosx', '--show-sdk-version']).strip(),
                            str(codesign), codesign.stat().st_mode, digest(codesign), minos]}
        key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
        if out.is_dir() and isinstance(previous, dict) and previous.get('inputs') == key and previous.get('app') == inventory(out):
            print(f'app: {out}, reused (inputs unchanged)', flush=True)
            return
    except BaseException:
        key_file.unlink(missing_ok=True)
        raise
    key_file.unlink(missing_ok=True)   # no failed attempt may leave a valid reuse key
    tmp = out.with_name(out.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True)
    c = tmp / 'Contents'; mac, res = c / 'MacOS', c / 'Resources'; dm = res / 'dm'
    for d in (mac, dm / 'font'): d.mkdir(parents=True)
    romfs, msl, aoc = Path(a.romfs), Path(a.msl), Path(a.aoc) if a.aoc else None
    # each source is probed on its own, as they may lie on different volumes; cp -c copies what it cannot clone, so a probe decides only the free-space check and the step line
    trees, lists = {'romfs': romfs, 'aoc': aoc}, {'mods': mods, 'msl': msls, 'pso': pso}
    clone = {k: bool(d) and can_clone(d, res) for k, d in trees.items()} | {k: bool(ps) and can_clone(ps[0], res) for k, ps in lists.items()}
    need = sum(tree_bytes(d) for k, d in trees.items() if d and not clone[k]) + sum(p.stat().st_size for k, ps in lists.items() if not clone[k] for p in ps)
    if need:
        free = shutil.disk_usage(res).free
        if need + (1 << 30) > free: raise SystemExit(f'the app needs {need / 2**30:.1f} GB on the volume of {out.parent} (no APFS clones from the build there) and {free / 2**30:.1f} GB are free')
    step(f'launcher and host ({"clones" if clone["romfs"] else "copies"} of the game files)')
    build_launcher(mac / EXE, minos, clang, swiftc, sdk)
    shutil.copy2(a.host, mac / 'dmhost')
    copy(mods, dm, clone['mods'])
    if tables: copy(tables, dm, False)
    if pairs: shutil.copyfile(pairs, dm / 'pso_pairs.tsv')   # the split's copy, for a profile without the table
    if fonts: copy(fonts, dm / 'font', False)
    if a.env_file: shutil.copyfile(a.env_file, dm / 'env.txt')
    if a.settings: shutil.copyfile(a.settings, dm / 'launcher.json')
    step('shader libraries'); msl_dst = res / 'msl' / (a.msl_name or msl.name); msl_dst.mkdir(parents=True); copy(msls, msl_dst, clone['msl'])
    if aoc: step('add-on content'); copy([aoc], res / 'aoc', clone['aoc'])
    if pso: step('pipeline archive'); (res / 'pso').mkdir(); copy(pso, res / 'pso', clone['pso'])
    step('game files'); copy([romfs], res / ('romfs.img' if romfs.is_file() else 'romfs'), clone['romfs'])
    icon = bool(a.icon)
    if icon: step('icon'); make_icns(a.icon, res / 'AppIcon.icns')
    info = info_plist(a, minos, icon)
    info['DMModuleFiles'] = [p.name for p in mods]
    commit = source_commit()
    if commit: info['DMCommit'] = commit
    if msl_dst.name.startswith('b-'): info['DMChain'] = msl_dst.name[2:]   # the library chain the app carries (a Vulkan or GL game's vk / gl has none)
    (c / 'Info.plist').write_bytes(plistlib.dumps(info))
    (c / 'PkgInfo').write_text('APPL????')
    step(f'signing ({"ad-hoc" if a.sign == "-" else a.sign})'); sign(tmp, a.sign, codesign)
    had_old = out.exists()
    replace_app(tmp, out)
    key_tmp = key_file.with_name(key_file.name + '.partial')
    try:
        key_tmp.write_text(json.dumps({'inputs': key, 'app': inventory(out)}, sort_keys=True) + '\n')
        key_tmp.replace(key_file)
    except BaseException:
        if had_old: replace_app(tmp, out)
        else: out.rename(tmp)
        raise
    finally:
        key_tmp.unlink(missing_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)   # the old app after a successful swap
    print(f'app: {out}', flush=True)


def parse(argv):
    ap = argparse.ArgumentParser(prog='pack_app.py', description='Pack a self-contained Mac app of a game')
    ap.add_argument('out', help='the app (<name>.app)')
    ap.add_argument('--host', required=True, help='the Mac host built for the profile')
    ap.add_argument('--patched', required=True, help='the rewritten modules (patch_nso.py output)')
    ap.add_argument('--romfs', required=True, help='the RomFS of the packed version')
    ap.add_argument('--msl', required=True, help='the Mac shader libraries: <shader>/cache/b-<chain>, or a Vulkan or GL game\'s <shader>/vk or gl')
    ap.add_argument('--msl-name', help='its folder name in the app (default: its own)')
    ap.add_argument('--tables', required=True, help='the game profile folder (shader_map.tsv and the tables beside it)')
    ap.add_argument('--font', required=True, help='the folder of the shared-font stand-ins (nintendo_ext.ttf)')
    ap.add_argument('--aoc', help='the DLC RomFS images or folders by index')
    ap.add_argument('--pso', help='the pipeline archive made offline for this Mac (pso_offline.py --platform macos output: <arch>.mtl4a, prebuilt.tsv)')
    ap.add_argument('--pso-pairs', help='pso_pairs.tsv for a profile that has none (the shader split\'s copy)')
    ap.add_argument('--env-file', help='environment switches baked in (Resources/dm/env.txt)')
    ap.add_argument('--settings', help='the game\'s launcher settings (dm/games/settings.py output; Resources/dm/launcher.json, which the Mac front end reads)')
    ap.add_argument('--title-id', required=True); ap.add_argument('--name', required=True)
    ap.add_argument('--version'); ap.add_argument('--bundle-id'); ap.add_argument('--category'); ap.add_argument('--lang'); ap.add_argument('--marker', help='the profile\'s romfs_marker')
    ap.add_argument('--icon', help='the game icon (icon.jpg from the control data)')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    return ap.parse_args(argv)


if __name__ == '__main__':
    pack(parse(sys.argv[1:]))
