#!/usr/bin/env python3
"""Build a verified, self-contained LM3 1.4.0 app from local player-owned inputs."""
import argparse
import ctypes
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

from game_versions import VERSIONS, verify_exefs


ROOT = Path(__file__).resolve().parents[1]
MODULES = ('main', 'subsdk0', 'subsdk1', 'sdk')
GAME_PREFIX = Path('Contents/Resources/game')
MANIFEST_PATH = Path('Contents/Resources/package-manifest.json')
SEED_NAME = re.compile(r'(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json)|pso_manifest\.tsv)\Z')
SHA256 = re.compile(r'[0-9a-f]{64}\Z')


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def regular_file(path):
    path = Path(path)
    if not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError(f'Expected a regular file: {path}')
    return path


def checked_file(path, expected=None, size=None):
    path = regular_file(path)
    actual = digest(path)
    if expected is not None and actual != expected:
        raise ValueError(f'SHA-256 mismatch: {path.name}')
    if size is not None and path.stat().st_size != size:
        raise ValueError(f'Size mismatch: {path.name}')
    return actual


def run(arguments):
    result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{arguments[0]} failed: {(result.stderr or result.stdout).strip()}')
    return result.stdout


def version_key(value):
    if not re.fullmatch(r'\d+(?:\.\d+){1,2}', value):
        raise ValueError(f'Invalid macOS deployment version: {value}')
    parts = tuple(int(part) for part in value.split('.'))
    return parts + (0,) * (3 - len(parts))


def inspect_executable(path):
    regular_file(path)
    architectures = run(['xcrun', 'lipo', '-archs', path]).split()
    if 'arm64' not in architectures:
        raise ValueError(f'Apple silicon code is missing: {path.name}')
    build = run(['xcrun', 'vtool', '-arch', 'arm64', '-show-build', path])
    minimum = re.search(r'^\s*minos\s+(\S+)\s*$', build, re.MULTILINE)
    if not minimum:
        raise ValueError(f'Missing arm64 deployment target: {path.name}')
    version_key(minimum[1])
    dependencies = []
    for line in run(['otool', '-arch', 'arm64', '-L', path]).splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        dependency = line.split(' (compatibility version ', 1)[0]
        if not dependency.startswith(('/System/Library/Frameworks/', '/usr/lib/')):
            raise ValueError(f'External runtime dependency in {path.name}: {dependency}')
        dependencies.append(dependency)
    return {'minimum_macos': minimum[1], 'architectures': architectures, 'dependencies': dependencies}


def copy_file(source, destination):
    source = regular_file(source)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin':
        library = ctypes.CDLL(None, use_errno=True)
        clone = library.clonefile
        clone.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
        clone.restype = ctypes.c_int
        if clone(os.fsencode(source), os.fsencode(destination), 0) == 0:
            return
        if destination.exists():
            destination.unlink()
    shutil.copy2(source, destination)


def validated_seed(seed, root=ROOT):
    seed = Path(seed)
    if seed.is_symlink() or not seed.is_dir():
        raise ValueError('Prepared shader seed must be a directory, not a symlink')
    manifest = json.loads(regular_file(seed / 'seed-manifest.json').read_text())
    chain_header = (root / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
    chain = re.search(r'^\s*#define\s+DMSH_CHAIN\s+"([^"]+)"', chain_header, re.MULTILINE)
    if not chain or manifest.get('schema') != 1 or manifest.get('chain') != chain[1]:
        raise ValueError('Shader seed schema/translation chain differs from this runtime')
    shaders, files = manifest.get('shaders'), manifest.get('files')
    if not isinstance(shaders, int) or isinstance(shaders, bool) or shaders <= 0 or not isinstance(files, dict) or not files:
        raise ValueError('Shader seed has no validated translations')
    if 'pso_manifest.tsv' not in files:
        raise ValueError('Prepared shader seed is missing its pipeline warmup manifest')
    if {path.name for path in seed.iterdir()} != set(files) | {'seed-manifest.json'}:
        raise ValueError('Prepared shader seed contains unlisted files')
    for name, expected in files.items():
        if not isinstance(name, str) or not SEED_NAME.fullmatch(name) or not isinstance(expected, str) or not SHA256.fullmatch(expected):
            raise ValueError('Unsafe shader seed filename or SHA-256')
        checked_file(seed / name, expected)
    return manifest


def payload_sources(root, version, seed, launcher):
    """Validate source identities, then return only the runtime's explicit file set."""
    root, seed, launcher = Path(root), Path(seed), Path(launcher)
    if version != 'v140':
        raise ValueError('The standalone LM3 package currently requires v140')
    spec = verify_exefs(root / f'extracted/{version}/exefs', version)
    game_config = (root / 'build/game_version_config.h').read_text()
    game_dir = re.search(r'^#define TOTK_GAME_DIR "([^"]+)"$', game_config, re.MULTILINE)
    display = re.search(r'^#define TOTK_GAME_VERSION "([^"]+)"$', game_config, re.MULTILINE)
    if not game_dir or not display or game_dir[1] != version or display[1] != spec['display']:
        raise ValueError('Runtime build configuration does not match v140')
    tables = (root / 'build/runtime_tables.h').read_text()
    rows = re.findall(r'\.name="([^"]+)",\.digest="([0-9a-f]{64})",\.file_size=(\d+)ULL', tables)
    if {name for name, _, _ in rows} != set(MODULES) or len(rows) != len(MODULES):
        raise ValueError('Runtime module table is incomplete')
    sources = {Path('Contents/MacOS/LM3'): regular_file(launcher)}
    for relative in ('build/lm3-native', 'build/shader-service/totk_xlate', 'build/spirv-cross/spirv-cross'):
        sources[GAME_PREFIX / relative] = regular_file(root / relative)
    runtime_data = (root / 'build/lm3-native').read_bytes()
    for name, expected, size in rows:
        relative = Path(f'rewritten/{version}/{name}.patched.bin')
        checked_file(root / relative, expected, int(size))
        if expected.encode() not in runtime_data:
            raise ValueError('Native binary does not contain the current module identities')
        sources[GAME_PREFIX / relative] = root / relative
    sources[GAME_PREFIX / f'extracted/{version}/exefs/main'] = root / f'extracted/{version}/exefs/main'
    romfs = root / f'extracted/{version}/romfs'
    metadata = json.loads((romfs / 'index.json').read_text())
    if metadata.get('status') != 'pass' or metadata.get('romfs_sha256') != spec['romfs_sha256']:
        raise ValueError('RomFS extraction manifest differs from the pinned 1.4.0 input')
    checked_file(romfs / 'romfs.bin', spec['romfs_sha256'], metadata.get('romfs_size'))
    config = (root / 'build/romfs_config.h').read_text()
    index = re.search(r'#define ROMFS_INDEX_SHA256 "([0-9a-f]{64})"', config)
    image = re.search(r'#define ROMFS_IMAGE_SHA256 "([0-9a-f]{64})"', config)
    if not index or not image or image[1] != spec['romfs_sha256']:
        raise ValueError('Runtime RomFS configuration differs from the pinned image')
    checked_file(romfs / 'runtime-index.bin', index[1])
    for identity in (index[1], image[1]):
        if identity.encode() not in runtime_data:
            raise ValueError('Native binary does not contain the current RomFS identities')
    for name in ('romfs.bin', 'runtime-index.bin'):
        sources[GAME_PREFIX / f'extracted/{version}/romfs/{name}'] = romfs / name
    fonts = root / 'extracted/firmware/shared-fonts'
    font_manifest = json.loads((fonts / 'manifest.json').read_text())
    if font_manifest.get('status') != 'pass' or [row['type'] for row in font_manifest.get('fonts', [])] != list(range(6)):
        raise ValueError('Shared-font manifest is incomplete')
    for row in font_manifest['fonts']:
        name = f'{row["type"]}-{row["name"]}.bfttf'
        if '/' in name or '\\' in name or not SHA256.fullmatch(row['sha256']):
            raise ValueError('Invalid shared-font manifest row')
        checked_file(fonts / name, row['sha256'], row['bfttf_bytes'])
        if row['sha256'].encode() not in runtime_data:
            raise ValueError('Native binary does not contain the current shared-font identities')
        sources[GAME_PREFIX / 'extracted/firmware/shared-fonts' / name] = fonts / name
    prepared = validated_seed(seed, root)
    if prepared['chain'].encode() not in runtime_data:
        raise ValueError('Native binary does not contain the prepared shader translation chain')
    for name in (*prepared['files'], 'seed-manifest.json'):
        sources[GAME_PREFIX / 'build/shader-seed' / name] = seed / name
    sources[GAME_PREFIX / 'build/pso_manifest.tsv'] = seed / 'pso_manifest.tsv'
    sources[Path('Contents/Resources/AppIcon.icns')] = regular_file(root / 'build/AppIcon.icns')
    return sources, prepared


def resource_manifest(app):
    """Hash data/resources; codesign verifies the outer executable's resource seal.

    Hashing that executable here would be circular: outer signing puts this
    manifest's resource hash into its embedded signature, changing its bytes.
    """
    result = {}
    for path in sorted(Path(app).rglob('*')):
        relative = path.relative_to(app)
        if relative in (MANIFEST_PATH, Path('Contents/MacOS/LM3')) or '_CodeSignature' in relative.parts:
            continue
        if path.is_symlink():
            raise ValueError(f'Symlink is not allowed in the package: {relative}')
        if path.is_file():
            result[relative.as_posix()] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
    return result


def verify_resources(app):
    app = Path(app)
    manifest = json.loads((app / MANIFEST_PATH).read_text())
    if manifest.get('schema') != 1 or manifest.get('game_version') != 'v140' or manifest.get('files') != resource_manifest(app):
        raise ValueError('Bundle resource manifest verification failed')
    return manifest


def rename_exchange(first, second):
    """Exchange sibling directories atomically; an unsupported filesystem keeps the old app."""
    if sys.platform != 'darwin':
        raise RuntimeError('Atomic replacement of an existing app requires macOS')
    library = ctypes.CDLL(None, use_errno=True)
    exchange = library.renamex_np
    exchange.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
    exchange.restype = ctypes.c_int
    if exchange(os.fsencode(first), os.fsencode(second), 2):  # RENAME_SWAP
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))


def install_bundle(stage, app, exchange=rename_exchange):
    stage, app = Path(stage), Path(app)
    if app.is_symlink() or (app.exists() and not app.is_dir()):
        raise ValueError('Refusing to replace a symlink or a non-directory app')
    if not app.exists():
        os.replace(stage, app)
        return None
    backup = app.with_name(f'.{app.stem}.previous-{datetime.now():%Y%m%d-%H%M%S}-{uuid.uuid4().hex[:8]}.app')
    exchange(stage, app)
    # After the atomic exchange, the previous app is still intact at stage.
    try:
        os.replace(stage, backup)
    except OSError:
        return stage
    return backup


def package(app, version, seed, launcher=None, root=ROOT):
    root = Path(root).resolve()
    app = Path(app).expanduser().absolute()
    app = app.parent.resolve() / app.name
    if app.suffix != '.app' or app.is_symlink() or (app.exists() and not app.is_dir()):
        raise ValueError('Output must be an .app directory, not a symlink')
    seed = Path(seed).expanduser().absolute()
    for source in (root / 'build', root / 'rewritten', root / 'extracted', seed):
        if app.is_relative_to(source.resolve()) or source.resolve().is_relative_to(app):
            raise ValueError('Output overlaps a package input directory')
    launcher = Path(launcher) if launcher else root / 'build/lm3-app-launcher'
    sources, prepared = payload_sources(root, version, seed, launcher)
    binaries = {relative: inspect_executable(source) for relative, source in sources.items()
                if relative == Path('Contents/MacOS/LM3') or relative in {
                    GAME_PREFIX / 'build/lm3-native', GAME_PREFIX / 'build/shader-service/totk_xlate',
                    GAME_PREFIX / 'build/spirv-cross/spirv-cross'}}
    minimum = max((row['minimum_macos'] for row in binaries.values()), key=version_key)
    app.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f'.{app.stem}.stage-', suffix='.app', dir=app.parent))
    stage_identity = (stage.stat().st_dev, stage.stat().st_ino)
    installed = False
    try:
        for relative, source in sources.items():
            copy_file(source, stage / relative)
            if digest(source) != digest(stage / relative):
                raise ValueError(f'Copied resource differs from its source: {relative}')
        (stage / GAME_PREFIX / 'build/shader-map.tsv').write_text('# TOTK control-code-sha256-v1\n')
        info = dict(CFBundleExecutable='LM3', CFBundleIdentifier='org.macnativegames.lm3',
                    CFBundleName="Luigi's Mansion 3", CFBundleDisplayName="Luigi's Mansion 3",
                    CFBundlePackageType='APPL', CFBundleShortVersionString=VERSIONS[version]['display'],
                    CFBundleVersion='1', CFBundleIconFile='AppIcon', LSMinimumSystemVersion=minimum,
                    LSArchitecturePriority=['arm64'], LSApplicationCategoryType='public.app-category.action-games',
                    NSHighResolutionCapable=True)
        (stage / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        for relative in binaries:
            executable = stage / relative
            executable.chmod(executable.stat().st_mode | 0o111)
            run(['codesign', '--force', '--sign', '-', '--timestamp=none', executable])
            run(['codesign', '--verify', '--strict', executable])
        manifest = {'schema': 1, 'game_version': version, 'display_version': VERSIONS[version]['display'],
                    'minimum_macos': minimum, 'executables': {str(path): row for path, row in binaries.items()},
                    'outer_executable_integrity': 'codesign resource seal and CodeDirectory',
                    'shader_chain': prepared['chain'], 'shaders': prepared['shaders'],
                    'files': resource_manifest(stage)}
        (stage / MANIFEST_PATH).write_text(json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + '\n')
        verify_resources(stage)
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', stage])
        run(['codesign', '--verify', '--deep', '--strict', stage])
        # Check again after signing: only excluded signature records may be new.
        verify_resources(stage)
        previous = install_bundle(stage, app)
        installed = True
        return {'status': 'pass', 'app': str(app), 'previous_app': str(previous) if previous else None,
                'minimum_macos': minimum, 'shaders': prepared['shaders'], 'files': len(manifest['files']),
                'resource_bytes': sum(row['bytes'] for row in manifest['files'].values()),
                'manifest': str(app / MANIFEST_PATH)}
    finally:
        if not installed and stage.exists() and (stage.stat().st_dev, stage.stat().st_ino) == stage_identity:
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--version', choices=['v140'], default='v140')
    parser.add_argument('--seed', type=Path, required=True, help='Verified prepare_shader_seed.py output')
    parser.add_argument('--launcher', type=Path, default=ROOT / 'build/lm3-app-launcher')
    args = parser.parse_args()
    print(json.dumps(package(args.app, args.version, args.seed, args.launcher), ensure_ascii=False))


if __name__ == '__main__':
    main()
