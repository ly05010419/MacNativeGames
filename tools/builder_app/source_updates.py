"""Newer build scripts for a downloaded Launcher, from signed source releases (user 2026-11-16: "做根本方案，手动挑版本发").

A Release Launcher carries the source it was made from (bundled_job.prepare). Without this module every job, an express
update too, ran that source for ever: a fix reached players only through a new Launcher download. Now the maintainer
publishes chosen commits as GitHub releases tagged source-*, each with three assets:

  mng-source.json        format, commit, commit_time, archive, archive_sha256, archive_size, min_launcher
  mng-source.json.sig    the base64 Ed25519 signature of those exact bytes (the private release tool; the key is in its keychain)
  mng-source-<12>.tar.gz `git archive` of the commit, as make_app.source_archive makes the bundled one

The Launcher (this file and bundled_job.py ship inside the signed app, never inside a source archive) trusts a release only
when the signature verifies with the public key in its own Info.plist (MNGSourcePublicKey), and the archive has the size and
SHA-256 the manifest names. It runs the newest verified source that is newer than its own (by commit time) and asks for no
newer Launcher; a source that needs one is reported, not used. The home page job only reads the release list (once in ten
minutes) and starts a background download; an update job downloads before it runs, so pressing an update always builds
with the newest published code. No network, a bad release or a failed download leaves the source already on this Mac.
GitHub is asked with a generic User-Agent and nothing about the player.
"""
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.parse

FORMAT = 1
MANIFEST = 'mng-source.json'
SIGNATURE = MANIFEST + '.sig'
TAG_PREFIX = 'source-'
LAUNCHER_TAG = 'launcher-v'      # MNG Launcher's own releases (launcher-v1.0.6)
# Where the version label's click may take a player (user 2026-11-18, "直接下载 DMG"): the release's disk image on GitHub,
# the same file the website's Download button links to, or the website. Any other address in the list opens nothing.
LAUNCHER_HOSTS = ('github.com', 'mac-native-games.com', 'www.mac-native-games.com')
CHECK_EVERY = 600                 # seconds between two release-list reads by the home page
MAX_ARCHIVE = 1 << 30             # a source archive is about 140 MB; refuse anything near a gigabyte
FIELDS = {'format', 'commit', 'commit_time', 'archive', 'archive_sha256', 'archive_size', 'min_launcher'}
CURL = '/usr/bin/curl'            # python.org's Python has no CA certificates until its installer step ran (CLAUDE.md)
USER_AGENT = 'MNG-Launcher'
REQUIRED = ('tools/builder_jobs/__main__.py', 'build_app.py')


class SourceError(ValueError):
    pass


def zh():
    return os.environ.get('MNG_LANG') == 'zh'


def text(en, cn):
    return cn if zh() else en


def version_tuple(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', value):
        raise SourceError(f'not a major.minor.patch version: {value!r}')
    return tuple(int(part) for part in value.split('.'))


# --- signature and manifest ----------------------------------------------------------------------------------------------
def _public_key(public_b64):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    try:
        return Ed25519PublicKey.from_public_bytes(base64.b64decode(public_b64, validate=True))
    except (ValueError, TypeError) as error:
        raise SourceError(f'the Launcher has no valid source key: {error}') from None


def sign(data, private_b64):
    """The private release tool's signature (and the tests'): base64 Ed25519 over the exact manifest bytes."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.from_private_bytes(base64.b64decode(private_b64, validate=True))
    return base64.b64encode(key.sign(data)).decode()


def parse_manifest(data):
    try:
        m = json.loads(data)
    except (ValueError, UnicodeDecodeError) as error:
        raise SourceError(f'{MANIFEST} is not JSON: {error}') from None
    if not isinstance(m, dict) or set(m) != FIELDS:
        raise SourceError(f'{MANIFEST} must hold exactly {sorted(FIELDS)}')
    if m['format'] != FORMAT:
        raise SourceError(f'{MANIFEST} format {m["format"]!r}: this Launcher reads format {FORMAT}')
    if not (isinstance(m['commit'], str) and re.fullmatch(r'[0-9a-f]{40}', m['commit'])):
        raise SourceError(f'{MANIFEST}: bad commit')
    if type(m['commit_time']) is not int or m['commit_time'] <= 0:
        raise SourceError(f'{MANIFEST}: bad commit_time')
    if not (isinstance(m['archive'], str) and re.fullmatch(r'mng-source-[0-9a-f]{12}\.tar\.gz', m['archive'])
            and m['archive'] == f'mng-source-{m["commit"][:12]}.tar.gz'):
        raise SourceError(f'{MANIFEST}: bad archive name')
    if not (isinstance(m['archive_sha256'], str) and re.fullmatch(r'[0-9a-f]{64}', m['archive_sha256'])):
        raise SourceError(f'{MANIFEST}: bad archive_sha256')
    if type(m['archive_size']) is not int or not 0 < m['archive_size'] <= MAX_ARCHIVE:
        raise SourceError(f'{MANIFEST}: bad archive_size')
    version_tuple(m['min_launcher'])
    return m


def verify(data, signature_b64, public_b64):
    """The manifest, only when the Launcher's key signed exactly these bytes."""
    from cryptography.exceptions import InvalidSignature
    key = _public_key(public_b64)
    try:
        signature = base64.b64decode(signature_b64.strip() if isinstance(signature_b64, str) else signature_b64, validate=True)
        key.verify(signature, data)
    except (ValueError, TypeError, InvalidSignature):
        raise SourceError(f'{SIGNATURE} does not verify with this Launcher\'s key') from None
    return parse_manifest(data)


# --- the Launcher's own settings (its signed Info.plist) -------------------------------------------------------------------
def settings(resources):
    """What the signed Launcher says about source updates, or None when it carries no key (a local build)."""
    info = plistlib.loads((Path(resources).parent / 'Info.plist').read_bytes())
    key, repo = info.get('MNGSourcePublicKey'), info.get('MNGSourceRepo')
    if not key or not repo:
        return None
    return {
        'public_key': key,
        'api': info.get('MNGSourceAPI') or f'https://api.github.com/repos/{repo}/releases?per_page=30',
        'launcher': info.get('CFBundleShortVersionString', '0.0.0'),
        'bundled_time': int(info.get('MNGSourceCommitTime') or 0),
    }


# --- sources on this Mac ---------------------------------------------------------------------------------------------------
def installed(support):
    """Downloaded sources whose manifest is kept beside them: (manifest, folder)."""
    found = []
    for folder in sorted((Path(support) / 'sources').glob('*')):
        kept = folder / '.mng-source.json'
        if not (folder / '.ready').is_file() or not kept.is_file():
            continue
        try:
            m = parse_manifest(kept.read_bytes())
        except SourceError:
            continue
        if folder.name == m['archive_sha256'] and all((folder / name).is_file() for name in REQUIRED):
            found.append((m, folder))
    return found


def usable(m, conf):
    return m['commit_time'] > conf['bundled_time'] and version_tuple(m['min_launcher']) <= version_tuple(conf['launcher'])


def choose(support, bundled, conf):
    """The source a job runs: the newest usable download, else the Launcher's own."""
    best = None
    for m, folder in installed(support):
        if usable(m, conf) and (best is None or m['commit_time'] > best[0]['commit_time']):
            best = (m, folder)
    return best[1] if best else Path(bundled)


def current_time(support, conf):
    times = [m['commit_time'] for m, _ in installed(support) if usable(m, conf)]
    return max(times + [conf['bundled_time']])


# --- the release list ---------------------------------------------------------------------------------------------------
def curl(url, output=None, max_time=20, max_size=None, api=False):
    """Bytes of url (or the file written), through the system curl; raises SourceError on any failure. HTTPS only, also
    after redirects; plain HTTP only to this Mac (the tests' local server)."""
    local = url.startswith(('http://127.0.0.1:', 'http://localhost:'))
    cmd = [CURL, '-fsSL', '--proto', '=https,http' if local else '=https', '--proto-redir', '=https,http' if local else '=https',
           '-A', USER_AGENT, '--max-time', str(max_time)]
    if api:
        cmd += ['-H', 'Accept: application/vnd.github+json']
    if max_size:
        cmd += ['--max-filesize', str(max_size)]
    if output:
        cmd += ['--speed-limit', '1024', '--speed-time', '60', '-o', str(output)]
    try:
        done = subprocess.run(cmd + [url], capture_output=True, timeout=max_time + 10)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SourceError(f'{url}: {error}') from None
    if done.returncode:
        raise SourceError(f'{url}: curl {done.returncode} {done.stderr.decode(errors="replace").strip()}')
    return output if output else done.stdout


def releases(listing):
    """Source releases newest first, each with the URLs of its three assets."""
    out = []
    for r in listing if isinstance(listing, list) else []:
        if not isinstance(r, dict) or r.get('draft') or not str(r.get('tag_name', '')).startswith(TAG_PREFIX):
            continue
        assets = {a.get('name'): a.get('browser_download_url') for a in r.get('assets') or [] if isinstance(a, dict)}
        if assets.get(MANIFEST) and assets.get(SIGNATURE):
            out.append({'tag': r['tag_name'], 'published': str(r.get('published_at') or ''), 'assets': assets})
    return sorted(out, key=lambda r: r['published'], reverse=True)


def newest_launcher(listing):
    """The highest MNG Launcher release in the list, {'version', 'url'}, or None: the window's version label turns blue
    when it is newer than this Launcher (a player had no other way to know one was out), and its click downloads the
    url: the release's disk image, as the website's Download button does (its page when it has none). Only an https
    address on LAUNCHER_HOSTS is kept; a release with none is left out."""
    def ours(url):
        parts = urllib.parse.urlsplit(str(url or ''))
        return parts.scheme == 'https' and parts.hostname in LAUNCHER_HOSTS

    best = None
    for r in listing if isinstance(listing, list) else []:
        tag = str(r.get('tag_name', '')) if isinstance(r, dict) else ''
        if not tag.startswith(LAUNCHER_TAG) or r.get('draft'):
            continue
        try:
            version = version_tuple(tag[len(LAUNCHER_TAG):])
        except SourceError:
            continue
        if best is None or version > best[0]:
            dmg = [a.get('browser_download_url') for a in r.get('assets') or []
                   if isinstance(a, dict) and str(a.get('name', '')).endswith('-macOS-arm64.dmg')]
            url = next((u for u in (*dmg, r.get('html_url')) if ours(u)), None)
            if url:
                best = (version, {'version': tag[len(LAUNCHER_TAG):], 'url': str(url)})
    return best[1] if best else None


def _state_path(support):
    return Path(support) / 'sources' / 'update-check.json'


def _save_state(support, state):
    path = _state_path(support)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(state))
    os.replace(tmp, path)


def check(support, conf, now=None):
    """Reads the release list. Returns ('new', found) for the newest verified usable release newer than what this Mac runs,
    ('launcher', manifest) when the newest newer release needs a newer Launcher, or ('none', None). The found release is
    kept in update-check.json (its signed manifest too) for the background download. Raises SourceError when the list
    cannot be read."""
    now = time.time() if now is None else now
    try:
        listing = json.loads(curl(conf['api'], max_time=15, api=True))
    except ValueError as error:   # SourceError is one too, and passes as it is
        raise error if isinstance(error, SourceError) else SourceError(f'the release list is not JSON: {error}') from None
    have = current_time(support, conf)
    launcher = newest_launcher(listing)   # kept with the check for the window's version label (App.swift VersionLabel)
    needs_launcher = None
    for r in releases(listing)[:10]:
        try:
            data = curl(r['assets'][MANIFEST], max_time=15, max_size=1 << 16)
            signature = curl(r['assets'][SIGNATURE], max_time=15, max_size=1 << 12).decode()
            m = verify(data, signature, conf['public_key'])
        except (SourceError, UnicodeDecodeError):
            continue
        if m['commit_time'] <= have:
            continue
        url = r['assets'].get(m['archive'])
        if not url:
            continue
        if version_tuple(m['min_launcher']) > version_tuple(conf['launcher']):
            needs_launcher = needs_launcher or m
            continue
        found = {'tag': r['tag'], 'manifest': base64.b64encode(data).decode(), 'signature': signature, 'archive_url': url}
        _save_state(support, {'checked_at': now, 'found': found, 'launcher': launcher})
        return 'new', found
    _save_state(support, {'checked_at': now, 'found': None, 'launcher': launcher})
    return ('launcher', needs_launcher) if needs_launcher else ('none', None)


def checked_recently(support, now=None):
    now = time.time() if now is None else now
    try:
        state = json.loads(_state_path(support).read_text())
        return 0 <= now - float(state['checked_at']) < CHECK_EVERY
    except (OSError, ValueError, KeyError, TypeError):
        return False


def pending(support):
    """The release the last check found, if it is not installed yet."""
    try:
        return json.loads(_state_path(support).read_text()).get('found')
    except (OSError, ValueError, AttributeError):
        return None


# --- download and install -------------------------------------------------------------------------------------------------
def install(support, archive, m):
    """Extracts a verified archive into sources/<sha256> with its manifest and commit; returns the folder."""
    root = Path(support) / 'sources'
    target = root / m['archive_sha256']
    if (target / '.ready').is_file():
        if not (target / '.mng-source.json').is_file():
            (target / '.mng-source.json').write_text(json.dumps(m))
        return target
    stage = Path(tempfile.mkdtemp(prefix='.source-', dir=root))
    try:
        with tarfile.open(archive) as tar:
            tar.extractall(stage, filter='data')
        missing = [name for name in REQUIRED if not (stage / name).is_file()]
        if missing:
            raise SourceError('the source archive is incomplete: ' + ', '.join(missing))
        (stage / '.mng-source-commit').write_text(m['commit'] + '\n')
        (stage / '.mng-source.json').write_text(json.dumps(m))
        (stage / '.ready').write_text(m['archive_sha256'] + '\n')
        if target.exists():
            shutil.rmtree(target)   # an earlier attempt that never got its .ready
        stage.rename(target)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return target


def download(support, found, conf, say=lambda line: None):
    """Downloads, checks and installs a release check() found (its manifest is verified again here). One download at a
    time on this Mac: a second caller waits for the first and then finds the source installed."""
    data = base64.b64decode(found['manifest'])
    m = verify(data, found['signature'], conf['public_key'])
    root = Path(support) / 'sources'
    root.mkdir(parents=True, exist_ok=True)
    with (root / '.download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        target = root / m['archive_sha256']
        if (target / '.ready').is_file() and (target / '.mng-source.json').is_file():
            return target
        part = root / f'.download-{m["archive_sha256"]}.part'
        try:
            say(text(f'Code update: downloading {m["commit"][:12]} ({m["archive_size"] / 1e6:.0f} MB)…',
                     f'代码更新：正在下载 {m["commit"][:12]}（{m["archive_size"] / 1e6:.0f} MB）…'))
            started = time.monotonic()
            curl(found['archive_url'], output=part, max_time=3600, max_size=m['archive_size'])
            if part.stat().st_size != m['archive_size']:
                raise SourceError(f'the archive has {part.stat().st_size} bytes, the manifest says {m["archive_size"]}')
            with part.open('rb') as f:
                digest = hashlib.file_digest(f, 'sha256').hexdigest()
            if digest != m['archive_sha256']:
                raise SourceError('the archive does not have the SHA-256 its signed manifest names')
            folder = install(support, part, m)
            say(text(f'Code update: {m["commit"][:12]} verified and installed in {time.monotonic() - started:.0f} s',
                     f'代码更新：{m["commit"][:12]} 已核对并装好（{time.monotonic() - started:.0f} 秒）'))
            return folder
        finally:
            part.unlink(missing_ok=True)
