#!/usr/bin/env python3
"""The iPhone / iPad app of a port built with the Zelda app and tools (user 2026-11-20: "统一 统一 统一！！！ 这是规范 要求"):
one build for every port whose runtime comes from the same line as Tears of the Kingdom's (Animal Crossing, Super Mario
Odyssey). Its sources are games/totk/ios/Sources, compiled with the port's GAME_<NAME>, beside the port's own runtime and
graphics layer and its games/<game>/ios/Sources answers (ios_front.m); the embed, dependency and Xcode tools are this
folder's, and so is the installer (install_device.py). A port's games/<game>/ios/tools/build_ipa.py only describes it
(Port, below) and calls main(). tests/test_ios_ports_unified.py keeps every port on this file.

iOS cannot execute code written at run time, so the rewritten modules are compiled into the app (gen_embed.py; the
port's loader maps them back), with the game patches applied on the Mac (its host's export-ios). The device cannot
translate shaders either: the app carries the prepared seed and what the Mac app translated while playing (its shader
cache); a variant neither has is skipped on the device. --image-outside leaves romfs.bin out of the app:
install_device.py --game-data copies it once into Documents/GameData, so an update carries only the program."""
import argparse, json, os, re, shutil, subprocess, sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

TOOLS = Path(__file__).resolve().parent
TOTK = TOOLS.parents[1]          # games/totk
REPO = TOTK.parents[1]
SHELL = TOTK / 'ios/Sources'     # the shared iPhone / iPad app
sys.path.insert(0, str(TOOLS))
from xcode_dir import developer_dir  # noqa: E402
from compile_seed import seed_flags  # noqa: E402   Zelda's device libraries (compile_seed.py)
from device_pipelines import device_gpus, make as make_device_pipelines, seed_check  # noqa: E402   Zelda's device pipelines

APP = 'TOTK'   # the product: TOTK.app, which install_device.py signs and installs
NOT_SOURCES = ('**/*.md', '**/*.py', '**/*.txt', '**/*.inc')   # notes and #included fragments: not app resources
ICONS = (('AppIcon60x60@2x.png', 120), ('AppIcon60x60@3x.png', 180), ('AppIcon76x76@2x~ipad.png', 152),
         ('AppIcon83.5x83.5@2x~ipad.png', 167))   # plain PNGs named in Info.plist: an asset catalog needs actool's simulator
ICON_PLIST = '''
        CFBundleIcons:
          CFBundlePrimaryIcon:
            CFBundleIconFiles: [AppIcon60x60]
        CFBundleIcons~ipad:
          CFBundlePrimaryIcon:
            CFBundleIconFiles: [AppIcon60x60, AppIcon76x76, AppIcon83.5x83.5]'''
# The Mac's translations taken into the seed: sources and indexes only, by the import's name rule (`safe`); its
# .metallib files are macOS libraries, the device's are compiled from the sources (compile_seed.py)
SEED_NAME = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json))$')
FRAMEWORKS = ('UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore', 'ImageIO', 'CoreGraphics', 'CoreText',
              'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox', 'GameController', 'CoreHaptics', 'CoreMotion',
              'AVFAudio', 'VideoToolbox', 'CoreMedia', 'CoreVideo', 'Security')


@dataclass
class Layout:
    """Where a port's Mac build left what the app needs (absolute paths)."""
    host: Path                 # the Mac host binary (export-ios)
    build: Path                # its generated headers: runtime_tables.h, ... (and the icon, AppIcon.icns)
    rewritten: Path            # <name>.patched.bin, as the host maps them
    fixups: Callable[[str], Path]   # a module's <name>.fixups.tsv
    romfs: Path                # romfs.bin, runtime-index.bin, index.json
    fonts: Path                # the six system fonts (*.bfttf) and their manifest
    seed: Path                 # the prepared shader seed (seed-manifest.json)
    version: str               # CFBundleShortVersionString
    icon: Path = None          # the game's icon (.icns, .png or .jpg); default <build>/AppIcon.icns


@dataclass
class Port:
    game: str                  # games/<game>
    define: str                # GAME_<NAME>, the shell's branch for this port
    name: str                  # the Home Screen name
    bundle_id: str
    layout: Callable[[Path], Layout]
    mac_cache: Callable[[Layout], Path]            # the Mac app's shader cache (read only)
    cflags: Callable[[Layout], str] = lambda L: ''  # more OTHER_CFLAGS (e.g. a version header to -include)
    libs: tuple = ('libopus.a', 'libvpx.a')
    deps_args: tuple = ()      # build_deps.py's options (--vorbis)
    exports: Callable = None   # (host, rewritten, gen, env, run, logs, work) -> gen_embed.py's extra arguments
    game_data: Callable = None  # (res, work, layout, extra) -> copy the port's own files into GameData
    pane: Callable = None      # (root specs) -> the port's changes to the shared Settings pane
    pane_app: str = None       # build_ipa.py pane_for's app (default: game)
    mac_only: tuple = ('frontend.m', 'loader.c', 'game_info.plist')   # runtime files the iPhone app leaves out (loader.c is added as main; the Mac host's bundle id)
    extra: dict = field(default_factory=dict)
    # A port's own steps where the Zelda line's default does not fit (Pokémon Legends: Arceus: prepare, check); None keeps the
    # Zelda line's steps below as they are (Animal Crossing, Super Mario Odyssey, Mario Kart 8 Deluxe).
    prepare: Callable = None   # (L, gen, env, run, logs, work) -> stage what the steps below read (before the checks)
    check: Callable = None     # (L) -> raise RuntimeError for what the Mac build has not made (default: check_mac_build)
    modules: Callable = None   # (L) -> the module names (default: the generated runtime_tables.h's)
    embed: Callable = None     # (L, gen, names, env, run, logs, work) -> link flags: the modules compiled into the app (default: export-ios + gen_embed.py)
    sources: Callable = None   # (L, gen) -> [(path, {'excludes': [...], 'compilerFlags': '...'})]: the runtime's sources (default: runtime/, d2r_gfx, loader.c as main)
    includes: Callable = None  # (L, gen) -> the runtime's header folders (default: d2r_gfx nvn and nvn/include)
    shaders: Callable = None   # (L, out, work, logs, env) -> fingerprint: the seed folder out, as install_device.py pushes it (default: seed_with_mac_cache + device_pipelines)
    # The port's own fixes to the merged seed's MSL sources before they are compiled for the device (seed_with_mac_cache fix):
    # (L, folder, run, log, env) -> None, rewriting <variant>_b.metal files in place (replacing, never writing through the
    # clones). Pokémon Legends: Arceus: its host's msl-rewrite, the source fixes its Mac runtime makes before compiling,
    # which a library compiled on the Mac cannot get on the device. None: the sources as translated (the other ports).
    sources_fix: Callable = None
    entitlements: tuple = ()   # entitlements beyond the increased memory limit, listed in Info.plist MNGEntitlements for install_device.py

    @property
    def scheme(self): return f'mng-{self.game}'
    @property
    def dir(self): return REPO / 'games' / self.game


def run(args, cwd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as f:
        p = subprocess.run([str(x) for x in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(errors='replace').splitlines()[-35:]
        raise RuntimeError(f'{args[0]} failed; see {log}\n' + '\n'.join(tail))


def clone(src, dst):
    """An APFS clone (cp -c): a game image of many GB takes no extra space and no time on the same volume."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode:
        shutil.copy2(src, dst)


def module_names(tables):
    """The modules in the loader's order, as the generated tables list them."""
    names = re.findall(r'\{\.name="([A-Za-z0-9_]+)",\.digest=', tables.read_text(errors='replace'))
    if not names:
        raise RuntimeError(f'no modules in {tables}')
    return names


def yaml_sources(entries):
    """project.yml's lines for a port's own runtime sources (Port.sources): each a path with its excludes and compiler flags."""
    out = ''
    for path, opts in entries:
        out += f'\n      - path: {path}'
        if opts.get('excludes'): out += '\n        excludes:' + ''.join(f'\n          - "{n}"' for n in opts['excludes'])
        if opts.get('compilerFlags'): out += f'\n        compilerFlags: "{opts["compilerFlags"]}"'
    return out


def project(port, L, gen, flags_file):
    rt, gfx = port.dir / 'runtime', port.dir / 'third_party/d2r_gfx/gfx'
    excludes = ''.join(f'\n          - "{n}"' for n in port.mac_only + NOT_SOURCES)
    not_sources = ''.join(f'\n          - "{n}"' for n in NOT_SOURCES)
    entitlements_plist = ''.join(['        MNGEntitlements:\n', *(f'          - {e}\n' for e in port.entitlements)]) if port.entitlements else ''
    icon_plist = ICON_PLIST if (gen / 'icons' / ICONS[0][0]).is_file() else ''
    incs = port.includes(L, gen) if port.includes else [gfx / 'nvn', gfx / 'nvn/include']
    if port.sources:   # another runtime line's sources (Port.sources), then the embedded modules
        runtime = yaml_sources(port.sources(L, gen)) + f'\n      - path: {gen / "embed.S"}'
    else:
        runtime = f'''
      - path: {rt}
        excludes:{excludes}
      - path: {gfx / 'nvn'}
        excludes:{not_sources}
      - path: {gfx / 'tex'}
        excludes:{not_sources}
          - nvn_bctc.metal   # the runtime compiles its copy from source (nvn_mtl.m k_bctc_msl)
      - path: {gen / 'embed.S'}
      - path: {rt / 'loader.c'}
        compilerFlags: "-Dmain=totk_host_main"'''
    headers = ''.join(f'\n        - {d}' for d in incs)
    include_flags = ' '.join(f'-I{d}' for d in [rt, *incs])
    return f'''name: PortIOS
options:
  bundleIdPrefix: org.macnativegames
  deploymentTarget:
    iOS: "26.0"
configFiles:
  Release: {flags_file}
  Debug: {flags_file}
targets:
  PortIOS:
    type: application
    platform: iOS
    sources:
      - path: {SHELL}   # the iPhone / iPad app every Zelda port uses (start screen, picture, touch, settings)
        excludes:
          - TOTK.entitlements
          - Info.plist
          - "*.lproj"
      - path: {port.dir / 'ios/Sources'}   # the port's answers to what the shell asks of a runtime (ios_front.m)
        excludes:
          - "*.entitlements"
          - Info.plist
      - path: {gen / 'Settings.bundle'}{runtime}
    info:
      path: {gen / 'Info.plist'}
      properties:
        CFBundleDisplayName: {port.name}
        CFBundleName: {port.name}
        CFBundleIdentifier: $(PRODUCT_BUNDLE_IDENTIFIER)
        CFBundleShortVersionString: "{L.version}"
        CFBundleVersion: "1"
        UILaunchScreen: {{}}{icon_plist}
        UIFileSharingEnabled: true
{entitlements_plist}        LSSupportsOpeningDocumentsInPlace: true
        CFBundleURLTypes:   # the iPhone MNG Launcher sees the game installed and starts it
          - CFBundleURLName: org.macnativegames.launch
            CFBundleURLSchemes:
              - {port.scheme}
        UIRequiresFullScreen: true
        UISupportedInterfaceOrientations:
          - UIInterfaceOrientationLandscapeLeft
          - UIInterfaceOrientationLandscapeRight
        UIStatusBarHidden: true
        UIViewControllerBasedStatusBarAppearance: false
        UIApplicationSupportsIndirectInputEvents: true
        GCSupportsControllerUserInteraction: true
        NSMotionUsageDescription: Tilt the device to move the camera while you play with the on-screen controls.
        GCSupportedGameControllers:
          - ProfileName: ExtendedGamepad
        CADisableMinimumFrameDurationOnPhone: true
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default
                UISceneDelegateClassName: TOTKSceneDelegate
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: {port.bundle_id}
      PRODUCT_NAME: {APP}
      CODE_SIGN_ENTITLEMENTS: {SHELL / 'TOTK.entitlements'}
      TARGETED_DEVICE_FAMILY: "1,2"
      IPHONEOS_DEPLOYMENT_TARGET: "26.0"
      CODE_SIGN_STYLE: Manual
      CODE_SIGNING_ALLOWED: NO
      ENABLE_BITCODE: NO
      GCC_C_LANGUAGE_STANDARD: gnu11
      CLANG_ENABLE_OBJC_ARC: YES
      GCC_WARN_INHIBIT_ALL_WARNINGS: YES
      HEADER_SEARCH_PATHS:
        - $(inherited){headers}
        - {gen}
        - {gen / 'deps/include'}
        - {gen / 'deps/include/opus'}
        - {L.build}
      OTHER_CFLAGS: $(inherited) -D{port.define}=1 {port.cflags(L)} -I{SHELL} -mcpu=apple-a15 {include_flags} -I{gen} -I{L.build} -idirafter {TOTK / 'runtime'} -idirafter {TOTK / 'third_party/d2r_gfx/gfx/nvn'}
'''


def settings_pane(port, bundle):
    """The shared Settings pane (games/totk/ios/Settings.bundle, build_ipa.py pane_for), with the port's changes, in the
    device's language (settings_strings.py), as every Zelda port's."""
    import importlib.util, plistlib
    spec = importlib.util.spec_from_file_location('totk_build_ipa', TOOLS / 'build_ipa.py')
    totk = importlib.util.module_from_spec(spec); spec.loader.exec_module(totk)
    shutil.rmtree(bundle, ignore_errors=True); shutil.copytree(TOTK / 'ios/Settings.bundle', bundle)
    root = totk.pane_for(plistlib.loads((bundle / 'Root.plist').read_bytes()), port.pane_app or port.game)
    specs = root['PreferenceSpecifiers']
    # A port's MetalFX: off, or the picture to the screen (no smaller scene for the scene stage; port_features.h)
    mfx = next((x for x in specs if x.get('Key') == 'TOTK_METALFX'), None)
    if mfx: mfx.update(Values=['off', 'screen'], Titles=['關 · Off', '到螢幕 · To the screen'], DefaultValue='off')
    if port.pane: port.pane(specs)
    (bundle / 'Root.plist').write_bytes(plistlib.dumps(root))
    totk.settings_strings.localize(bundle)


def seed_with_mac_cache(seed, cache, out, run=None, log=None, env=None, fix=None, fix_id=''):
    """The prepared seed with the Mac app's translations merged in, at out, and its fingerprint (fingerprint.txt): a
    translation the seed lacks is added and named in seed-manifest.json's files (its SHA-256), a .variants index both
    have gets the Mac's rows the seed lacks. Then, as Zelda's build_ipa.py does, compile_seed.py compiles every
    shader into the device's .metallib (the game loads it instead of compiling the source on the phone: the iPad's
    first-start warm-up of 61,896 items, 2026-11-21); run None (tests) leaves them out. fix(out), when given (Port.sources_fix),
    changes the merged sources first and their manifest hashes follow; fix_id names what it does (the inputs' fingerprint).
    Made again only when the inputs changed (the fingerprint of the inputs, with the compiler options)."""
    import hashlib
    manifest = json.loads((seed / 'seed-manifest.json').read_text())
    # only shader files, by the name rule the seed import checks (totk_shader_seed_ports.h `safe`): the Mac cache also
    # keeps its own import receipt (seed-installed.json), which the import refuses as an unsafe name (iPad, 2026-11-21)
    mac = sorted(f for f in cache.iterdir() if f.is_file() and SEED_NAME.match(f.name)) if cache.is_dir() else []
    inputs = hashlib.sha256((seed / 'seed-manifest.json').read_bytes())
    for f in mac: st = f.stat(); inputs.update(f'{f.name}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
    if run: inputs.update(f'metallibs\0{seed_flags("iphoneos")}'.encode())
    if fix: inputs.update(f'fix\0{fix_id}'.encode())
    want = inputs.hexdigest()
    stamp = out / 'fingerprint.txt'
    if stamp.is_file() and (out / 'inputs.txt').is_file() and (out / 'inputs.txt').read_text().strip() == want:
        return stamp.read_text().strip()
    shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
    for f in sorted(seed.iterdir()):
        if f.is_file(): clone(f, out / f.name)
    files = manifest['files']
    rows = lambda path: json.loads(path.read_text())
    for f in mac:
        target = out / f.name
        if f.name.endswith('.variants') and target.is_file():
            have = rows(target); known = {r.get('variant') for r in have if isinstance(r, dict)}
            add = [r for r in rows(f) if isinstance(r, dict) and r.get('variant') not in known]
            if not add: continue
            target.unlink(); target.write_text(json.dumps(have + add, sort_keys=True))
        elif target.exists():
            continue
        else:
            clone(f, target)
        files[f.name] = hashlib.sha256(target.read_bytes()).hexdigest()
    if fix:   # the port's source fixes, then the manifest names the sources as they now are (compile_seed.py keys libraries by them)
        fix(out)
        for name in [n for n in files if n.endswith('_b.metal')]:
            if (out / name).is_file(): files[name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    (out / 'seed-manifest.json').unlink()
    (out / 'seed-manifest.json').write_text(json.dumps(manifest, indent=1, sort_keys=True))
    if run:   # libraries kept by source hash beside out (<out>.ios-metallibs): a rebuild compiles only new shaders
        run([sys.executable, TOOLS / 'compile_seed.py', out, 'iphoneos'], out.parent, log, env)
    fingerprint = hashlib.sha256((out / 'seed-manifest.json').read_bytes()).hexdigest()
    stamp.write_text(fingerprint + '\n'); (out / 'inputs.txt').write_text(want + '\n')
    return fingerprint


def port_lists(sources, out):
    """The pipeline list the Mac makes the device's pipelines from (device_pipelines.make): the R (render) and C (compute)
    rows of the Mac build's list (<build>/pso_manifest.tsv, Odyssey's) and of what the Mac app collected while playing (its
    shader cache's pso_collected.tsv), each once, in that order. The ports carry no curated list like Zelda's shader_list.tsv
    (its pso_list.tsv / pso_title.tsv); rows whose shaders the seed lacks are counted and left to the device."""
    rows, seen = [], set()
    for src in sources:
        if not src or not Path(src).is_file(): continue
        for line in Path(src).read_text(errors='replace').splitlines():
            if len(line) > 2 and line[1] == '\t' and line[0] in 'RC' and line not in seen:
                seen.add(line); rows.append(line)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('# the device pipelines\' list (build_port_ipa.py port_lists)\n' + ''.join(r + '\n' for r in rows))
    return len(rows)


def seed_fingerprint(seed):
    """What install_device.py compares to push the seed once per content (fingerprint.txt): the manifest, and every
    pipelines-* file by name (the name carries the GPU) and content, as Zelda's build_ipa.py seed_fingerprint: a build for
    another device's GPU is pushed again."""
    import hashlib
    fp = hashlib.sha256((seed / 'seed-manifest.json').read_bytes())
    for f in sorted(seed.glob('pipelines-*')): fp.update(f.name.encode() + b'\0' + hashlib.sha256(f.read_bytes()).digest())
    return fp.hexdigest()


def device_pipelines(port, L, seed, cache, work, logs, env, requested):
    """The device's pipelines in the seed (pipelines-<arch>-*), made as Zelda's build_ipa.py makes them (device_pipelines.make:
    the Mac host's pipeline-archive mode from the seed's device libraries, metal-tt for each GPU on the cable, kept in
    <work>/ios-pipelines between builds): the first start takes the pipelines from them, and the warm-up loads no shader
    library for itself (nvn_mtl.m warm_start, g_par_valid; the iPad's start warmed 61,896 items before, 2026-11-21).
    Then the seed's fingerprint with them; every cabled GPU's are checked (seed_check). Returns the fingerprint."""
    gpus = device_gpus(requested, env)
    pipe_work = work / 'ios-pipelines'
    listed = port_lists([L.build / 'pso_manifest.tsv', cache / 'pso_collected.tsv'], pipe_work / 'pso_list.tsv')
    print(f'pipelines: {listed} rows from the Mac build\'s list and what the Mac app collected')
    lists = [pipe_work / 'pso_list.tsv', seed / 'pso_manifest.tsv']
    make_device_pipelines(gpus, L.host, L.rewritten, lists, [seed / 'seed-manifest.json', pipe_work / 'pso_list.tsv', L.host],
                          seed, pipe_work, 'iphoneos', run, work, logs, env, clone, variants=False)
    for g in gpus:   # what goes to the device carries each cabled GPU's pipelines (install_device.py checks it again)
        state, line = seed_check(seed, g)
        if state != 'ok': raise RuntimeError(f'{seed}: {line}')
    fingerprint = seed_fingerprint(seed)
    (seed / 'fingerprint.txt').write_text(fingerprint + '\n')
    return fingerprint


def check_mac_build(port, L):
    """What the Zelda line's Mac build leaves (the host, its tables, the image and its index, the seed, the fonts)."""
    for need in (L.host, L.build / 'runtime_tables.h', L.romfs / 'romfs.bin', L.romfs / 'runtime-index.bin', L.seed / 'seed-manifest.json'):
        if not need.exists():
            raise RuntimeError(f'missing {need}: build the Mac app first (games/{port.game}/build.py)')
    if not list(L.fonts.glob('*.bfttf')):
        raise RuntimeError(f'no system fonts in {L.fonts}: build the Mac app first')


def seed_from_files(files, out):
    """A seed folder for install_device.py from a runtime's own shader files (Port.shaders: a runtime that
    makes its own iOS libraries and tables): files {name: path}, cloned into out with seed-manifest.json (each file's SHA-256) and
    fingerprint.txt, which install_device.py compares to push it once per content. Made again only when the inputs changed
    (their names, sizes and times). Returns the fingerprint."""
    import hashlib
    inputs = hashlib.sha256()
    for name, path in sorted(files.items()):
        st = path.stat(); inputs.update(f'{name}\0{path}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
    want, stamp = inputs.hexdigest(), out / 'fingerprint.txt'
    if stamp.is_file() and (out / 'inputs.txt').is_file() and (out / 'inputs.txt').read_text().strip() == want:
        return stamp.read_text().strip()
    shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
    manifest = {}
    import ctypes
    clonefile = getattr(ctypes.CDLL(None, use_errno=True), 'clonefile', None)   # one call per file: tens of thousands of cp processes took minutes
    for name, path in sorted(files.items()):
        if '/' in name or name in ('seed-manifest.json', 'fingerprint.txt', 'inputs.txt'):
            raise RuntimeError(f'not a seed file name: {name}')
        if not clonefile or clonefile(os.fsencode(path), os.fsencode(out / name), 0):
            shutil.copy2(path, out / name)
        h = hashlib.sha256()
        with open(out / name, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
        manifest[name] = h.hexdigest()
    (out / 'seed-manifest.json').write_text(json.dumps({'files': manifest}, indent=1, sort_keys=True))
    fingerprint = seed_fingerprint(out)
    stamp.write_text(fingerprint + '\n'); (out / 'inputs.txt').write_text(want + '\n')
    return fingerprint


def drop_group(specs, first, keys):
    """Removes a pane group (its heading before the row keyed first) and its rows."""
    at = next(i for i, x in enumerate(specs) if x.get('Key') == first)
    del specs[at - 1]
    specs[:] = [x for x in specs if x.get('Key') not in keys]


def main(port, argv=None):
    p = argparse.ArgumentParser(description=f'The {port.name} iPhone / iPad app (games/totk/ios/tools/build_port_ipa.py)')
    p.add_argument('--work', type=Path, required=True, help=f'the Mac build\'s work folder (games/{port.game}/build.py --work)')
    p.add_argument('--out', type=Path, required=True, help='.ipa, or .app for the unsigned folder (install_device.py takes both)')
    p.add_argument('--image-outside', action='store_true', help='leave romfs.bin out: the device keeps it in Documents/GameData '
                   '(install_device.py --game-data <romfs.bin>)')
    p.add_argument('--mac-shader-cache', type=Path, help='the shaders the Mac app translated while playing (default: the Mac app\'s)')
    p.add_argument('--device-gpu', action='append', help='a GPU architecture to compile the device\'s pipelines for (e.g. applegpu_g15p, '
                   'the iPad A16); default auto: the iPhones / iPads on the cable; none: the device builds them itself (as Zelda\'s build_ipa.py)')
    a = p.parse_args(argv)
    work, out = a.work.expanduser().resolve(), a.out.expanduser().resolve()
    if out.suffix.lower() not in ('.ipa', '.app'):
        raise RuntimeError('the output must end in .ipa or .app')
    L = port.layout(work)
    ios = work / 'ios'; gen = ios / 'Gen'; logs = work / 'logs/ios'
    gen.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    subprocess.run(['xcodebuild', '-version'], env=env, check=True, stdout=subprocess.DEVNULL)
    sys.path.insert(0, str(REPO))   # the repository's tools/deps: pinned xcodegen, cmake and ninja (no Homebrew)
    from tools.deps import path as deps_path
    env['PATH'] = os.pathsep.join(deps_path('xcodegen', 'cmake', 'ninja') + [env.get('PATH', '')])
    if port.prepare: port.prepare(L, gen, env, run, logs, work)
    port.check(L) if port.check else check_mac_build(port, L)
    names = port.modules(L) if port.modules else module_names(L.build / 'runtime_tables.h')

    extra = []
    if port.embed:   # another runtime line's embedding (Port.embed)
        flags = port.embed(L, gen, names, env, run, logs, work)
    else:
        # gen_embed.py reads <name>.patched.bin and <name>.fixups.tsv side by side
        staged = gen / 'rewritten'; shutil.rmtree(staged, ignore_errors=True); staged.mkdir()
        for n in names:
            (staged / f'{n}.patched.bin').symlink_to(L.rewritten / f'{n}.patched.bin')
            (staged / f'{n}.fixups.tsv').symlink_to(L.fixups(n))
        # The modules as the game patches leave them (export-ios), compiled into the executable's segments
        modules = gen / 'modules'; shutil.rmtree(modules, ignore_errors=True); modules.mkdir()
        extra = port.exports(L.host, L.rewritten, gen, env, run, logs, work) if port.exports else []
        if not port.exports:
            run([L.host, L.rewritten, 'export-ios'], work, logs / 'export-ios.log', dict(env, TOTK_EXPORT_IOS=str(modules)))
        flags = subprocess.check_output([sys.executable, TOOLS / 'gen_embed.py', staged, gen / 'embed.S', '--text-dir', modules,
                                         *extra, *names], text=True, env=env).strip()

    # The audio and video libraries for the device: the Mac archives (tools/deps) cannot be linked into an iOS app
    deps = work / 'ios-deps'
    if port.libs and not all((deps / 'lib' / lib).is_file() for lib in port.libs):
        run([sys.executable, TOOLS / 'build_deps.py', deps, 'iphoneos', *port.deps_args], work, logs / 'dependencies.log', env)
    if port.libs:   # a runtime that builds its own libraries takes none
        shutil.copytree(deps / 'include', gen / 'deps/include', dirs_exist_ok=True)
    for lib in port.libs:
        clone(deps / 'lib' / lib, gen / 'deps/lib' / lib)

    icon, icons = L.icon or L.build / 'AppIcon.icns', gen / 'icons'   # the game's own, from its control data
    shutil.rmtree(icons, ignore_errors=True); icons.mkdir()
    if icon.is_file():
        for n, px in ICONS:
            run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), icon, '--out', icons / n], work, logs / 'icon.log', env)
    shutil.copy2(SHELL / 'Info.plist', gen / 'Info.plist')   # xcodegen writes the properties into it
    settings_pane(port, gen / 'Settings.bundle')
    xcconfig = gen / 'port.xcconfig'
    xcconfig.write_text('OTHER_LDFLAGS = $(inherited) ' + ' '.join('-framework ' + x for x in FRAMEWORKS) + ' ' +
                        ' '.join(str(gen / 'deps/lib' / lib) for lib in port.libs) + f' -lc++ -lz {flags}\n')
    (ios / 'project.yml').write_text(project(port, L, gen, xcconfig))
    run(['xcodegen', 'generate', '--spec', 'project.yml'], ios, logs / 'xcodegen.log', env)
    # The target with the iphoneos SDK and no -destination: a destination asks for Xcode's iOS platform component
    derived = ios / 'derived'
    run(['xcodebuild', '-project', 'PortIOS.xcodeproj', '-target', 'PortIOS', '-configuration', 'Release', '-sdk', 'iphoneos',
         'ARCHS=arm64', f'SYMROOT={derived / "Build/Products"}', f'OBJROOT={derived / "obj"}',
         'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY='], ios, logs / 'xcodebuild.log', env)
    app = derived / f'Build/Products/Release-iphoneos/{APP}.app'
    if not app.is_dir():
        raise RuntimeError(f'Xcode did not produce {app}')
    # the increased-memory-limit entitlement in the ad-hoc signature, so re-signing keeps it
    run(['codesign', '--force', '--sign', '-', '--entitlements', SHELL / 'TOTK.entitlements', app / APP], ios, logs / 'entitlements.log', env)

    payload = ios / 'payload'
    dest = out if out.suffix == '.app' else payload / f'Payload/{APP}.app'
    shutil.rmtree(dest if out.suffix == '.app' else payload / 'Payload', ignore_errors=True)
    shutil.copytree(app, dest, symlinks=True)
    for f in sorted(icons.iterdir()):
        shutil.copy2(f, dest / f.name)
    res = dest / 'GameData'
    for n in (('runtime-index.bin', 'index.json') if a.image_outside else ('romfs.bin', 'runtime-index.bin', 'index.json')):
        if (L.romfs / n).is_file():
            clone(L.romfs / n, res / 'romfs' / n)
    for f in sorted(L.fonts.iterdir()):
        if f.is_file():
            clone(f, res / 'shared-fonts' / f.name)
    if port.game_data: port.game_data(res, work, L, extra)
    cache = a.mac_shader_cache.expanduser() if a.mac_shader_cache else port.mac_cache(L)
    fix, fix_id = None, ''
    if port.sources_fix:   # the port's source fixes (Port.sources_fix), named by its host (what makes them)
        fix = lambda folder: port.sources_fix(L, folder, run, logs / 'msl-fix.log', env)
        st = Path(L.host).stat(); fix_id = f'{port.game}\0{st.st_size}\0{st.st_mtime_ns}'
    if a.image_outside:
        # The shaders beside the app as Zelda's build_ipa.py --image-outside leaves them (<out>-shader-seed with its
        # fingerprint.txt): install_device.py pushes them once (image parts and a pack) and skips them while the device
        # has the same content; the port's seed import reads them there (shared totk_shader_seed_ports.h). The Mac's
        # translations go into the same seed, named in its manifest, so its integrity check covers them too.
        seed_out = out.with_name(out.stem + '-shader-seed')
        if port.shaders:   # another runtime line's shaders (Port.shaders), in the same folder install_device.py pushes
            fingerprint = port.shaders(L, seed_out, work, logs, env)
            print(f'shaders: beside the app: {seed_out} ({fingerprint[:12]})')
        else:
            fingerprint = seed_with_mac_cache(L.seed, cache, seed_out, run, logs / 'iphoneos-shaders.log', env, fix, fix_id)
            fingerprint = device_pipelines(port, L, seed_out, cache, work, logs / 'pipelines', env, a.device_gpu)
            print(f'shaders: the seed, the Mac\'s translations and their device libraries beside the app: {seed_out} ({fingerprint[:12]})')
    else:   # the same seed, inside the app
        seed_out = work / 'ios-shader-seed'
        if port.shaders:
            port.shaders(L, seed_out, work, logs, env)
        else:
            seed_with_mac_cache(L.seed, cache, seed_out, run, logs / 'iphoneos-shaders.log', env, fix, fix_id)
            device_pipelines(port, L, seed_out, cache, work, logs / 'pipelines', env, a.device_gpu)
        for f in sorted(seed_out.iterdir()):
            if f.is_file() and f.name not in ('fingerprint.txt', 'inputs.txt'):
                clone(f, res / 'shader-seed' / f.name)
        what = 'the seed' if port.shaders else "the seed, the Mac's translations and their device libraries"
        print(f'shaders: {what} in the app ({cache})')
    install = f'python3 games/totk/ios/tools/install_device.py "{out}" --device <name>' + (
        f' --game-data {L.romfs / "romfs.bin"}' if a.image_outside else '')
    if out.suffix == '.app':
        print(f'app ready: {out}; unsigned. Install: {install}')
        return
    out.parent.mkdir(parents=True, exist_ok=True); out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, logs / 'ipa-zip.log', env)
    print(f'IPA ready: {out} ({out.stat().st_size / 2**30:.1f} GiB); unsigned. Install: {install}')


def cli(port):
    try:
        main(port)
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
