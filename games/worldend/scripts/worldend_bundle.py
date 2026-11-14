"""Signed immutable data, verified digest reuse and in-place App publication."""
import hashlib
import fcntl
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from build_app import clone, install_app, record_update_source


class Digests:
    """Reuse only a hash of the same file identity and unchanged metadata."""
    def __init__(self, record):
        self.record = Path(record)
        try:
            self.known = json.loads(self.record.read_text())
        except (OSError, ValueError):
            self.known = {}

    @staticmethod
    def identity(path):
        s = path.stat()
        return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]

    def sha256(self, path):
        path = Path(path)
        identity = self.identity(path)
        old = self.known.get(str(path))
        if old and old[0] == identity:
            return old[1]
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if self.identity(path) != identity:
            raise ValueError(f'file changed while verifying: {path}')
        self.known[str(path)] = [identity, digest]
        return digest

    def files(self, folder):
        result = {}
        for p in sorted(Path(folder).rglob('*')):
            if p.is_symlink():
                raise ValueError(f'symlink in data bundle input: {p}')
            if p.is_file() and p.name != '.DS_Store':
                result[p.relative_to(folder).as_posix()] = self.sha256(p)
        return result

    def save(self):
        self.record.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.record.with_suffix('.tmp')
        temporary.write_text(json.dumps(self.known, sort_keys=True)+'\n')
        temporary.replace(self.record)


def check_output(output):
    if output.suffix != '.app' or output.is_symlink():
        raise ValueError('--output must be a real .app directory')
    if output.exists():
        try:
            info = plistlib.loads((output/'Contents/Info.plist').read_bytes())
        except (OSError, ValueError):
            raise ValueError(f'output exists and is not an Worldend app: {output}') from None
        if info.get('CFBundleIdentifier') != 'org.macnativegames.worldend':
            raise ValueError(f'output exists and belongs to another app: {output}')


def signed_bundle(partial, old, name, sources, digests, env):
    """Keep a matching signed bundle byte-for-byte; changed inputs get a new seal."""
    files = {}
    for key, folder in sources.items():
        files.update({f'{key}/{rel}': value for rel, value in digests.files(folder).items()})
    ident = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    names = {'ShaderData': 'ShaderData-worldend.bundle', 'GameData': 'GameData-worldend.bundle'}
    bundle = partial/'Contents/Helpers'/names[name]
    before = old/'Contents/Helpers'/bundle.name
    manifest = 'Contents/Resources/files.json'
    try:
        reusable = json.loads((before/manifest).read_text()) == files
        reusable = reusable and all(digests.sha256(before/'Contents/Resources'/rel) == sha for rel, sha in files.items())
        reusable = reusable and verified_signature(before, digests, env)
    except (OSError, ValueError):
        reusable = False
    bundle.parent.mkdir(parents=True, exist_ok=True)
    if reusable:
        print(f'{name}: reusing verified data and its signature', flush=True)
        subprocess.run(['cp', '-cR', str(before), str(bundle)], check=True)
        return bundle
    print(f'{name}: rebuilding because verified inputs/signature differ or no previous bundle exists', flush=True)
    resources = bundle/'Contents/Resources'
    resources.mkdir(parents=True)
    for key, folder in sources.items():
        subprocess.run(['cp', '-cR', str(folder), str(resources/key)], check=True)
    if digests.files(resources) != files:
        raise ValueError('data changed while the new bundle was being copied')
    (bundle/manifest).write_text(json.dumps(files, sort_keys=True)+'\n')
    (bundle/'Contents/Info.plist').write_bytes(plistlib.dumps({
        'CFBundleIdentifier': f'org.macnativegames.worldend.{name.lower()}',
        'CFBundlePackageType': 'BNDL', 'CFBundleVersion': '1', 'MNGDataSHA256': ident}))
    subprocess.run(['codesign', '--force', '--sign', '-', str(bundle)], env=env, check=True)
    if not verified_signature(bundle, digests, env):
        raise ValueError(f'data bundle signature failed: {bundle}')
    return bundle


def verified_signature(folder, digests, env):
    # All sealed bytes (including Info.plist, the seal and unexpected extra files)
    # participate. Per-file metadata lets unchanged files reuse their known hash.
    identity = hashlib.sha256(json.dumps(digests.files(folder), sort_keys=True).encode()).hexdigest()
    seals = digests.known.setdefault('_verified_seals', {})
    if identity in seals:
        return True
    if subprocess.run(['codesign', '--verify', '--strict', str(folder)], env=env, capture_output=True).returncode:
        return False
    seals[identity] = True
    return True


def link_data(resources, name, bundle, key=None):
    link = resources/name
    link.symlink_to(os.path.relpath(bundle/'Contents/Resources'/(key or name), link.parent))


def publish(partial, output):
    check_output(output)
    # The launcher holds this across exec; a normal game must finish saving before
    # its resources move. The test profile override uses the launcher's same rule.
    profile = Path(os.environ.get('DM_WORLDEND_PROFILE') or Path.home()/'Library/Application Support/MacNativeGames/Worldend Syndrome')
    profile.mkdir(parents=True, exist_ok=True)
    with (profile/'session.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('publication: waiting for Worldend Syndrome to close', flush=True)
            fcntl.flock(lock, fcntl.LOCK_EX)
        install_app(partial, output)



def adopt_inputs(app, build, env):
    """Restore a lost extraction cache from the installed, verified original data."""
    from build_worldend import verify_originals
    destination = build/'extract/v0'
    if destination.exists(): return False
    bundle = app/'Contents/Helpers/GameData-worldend.bundle'
    original = bundle/'Contents/Resources/original'
    if not original.is_dir(): return False
    digests = Digests(build/'verified-digests.json')
    files = json.loads((bundle/'Contents/Resources/files.json').read_text())
    if any(Path(rel).is_absolute() or '..' in Path(rel).parts for rel in files):
        raise ValueError('invalid game-data manifest path')
    if not verified_signature(bundle, digests, env) or any(digests.sha256(bundle/'Contents/Resources'/rel) != sha for rel, sha in files.items()):
        raise ValueError('installed game data changed; rebuild from the original inputs')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name('v0.adopt')
    if temporary.exists(): raise ValueError(f'interrupted data adoption: {temporary}')
    subprocess.run(['cp', '-cR', str(original), str(temporary)], check=True)
    # verify_originals takes the ordinary extraction layout. Validate before publication.
    pins = json.loads((Path(__file__).resolve().parents[1]/'assets/expected-1.0.0.json').read_text())
    for name, pin in pins['modules'].items():
        path = temporary/'ExeFS'/name
        if path.stat().st_size != pin['size'] or digests.sha256(path) != pin['sha256']:
            raise ValueError(f'installed original ExeFS hash mismatch: {name}')
    temporary.rename(destination)
    verify_originals(build)
    digests.save()
    print('inputs: reused installed original data after checking sealed file hashes and pinned modules', flush=True)
    return True
