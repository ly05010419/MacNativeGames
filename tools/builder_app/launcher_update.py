#!/usr/bin/env python3
"""MNG Launcher puts a newer MNG Launcher in its own place and opens it (user 2026-11-18: a real update button, "点一下
就自动下载、替换、重开"; until then the version label only linked to the disk image).

    python3 launcher_update.py update --app <the running app> --support <the Builder support folder> --pid <its pid>
                                      [--feed <release list address>] [--same]

One JSON object a line for the window: found, download (bytes so far), verify, install (the window quits now), or error
(with the address of the manual download, when one is known). Nothing is replaced unless every check passed:

- the release list names a launcher-v* release with the app's "-macOS-arm64.zip" on GitHub, higher than this Launcher;
- the download is the size the list says and holds one app, whose Info.plist says this bundle ID and that version;
- the app's code signature is whole (codesign --verify --deep --strict) and satisfies the designated requirement of the
  Launcher that is running: for a release that is the same Developer ID team and bundle ID, what Sparkle asks of an
  update too; and when the running Launcher is a Developer ID one, Gatekeeper accepts the new one as notarized (spctl);
- the folder the Launcher sits in can take the new app (not a read-only disk image, not macOS's translocated copy).

The replacement itself is a small shell script that runs once the Launcher has quit, from outside the app (the app's
own Python is inside the bundle being replaced): the old app is moved aside, the new one moved in, the Launcher opened;
when the new one cannot be put in place the old one comes back. The design is Sparkle's (MIT; read, not copied): verify
before replacing, replace after the app has quit, from outside the bundle, never leave the place empty.

This file lives in the signed app beside source_updates.py (make_app.py): the code that trusts or refuses a download is
never taken from a source archive. --feed and --same are for tests (the window passes them only with
MNG_LAUNCHER_UPDATE_TEST): another release list, and the same version again. They change no check: a Developer ID
Launcher still takes only a notarized app of its own team.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import time
import urllib.parse
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_updates as su   # noqa: E402  (beside this file, in the app and in the checkout)

ZIP_SUFFIX = '-macOS-arm64.zip'
MAX_ZIP = 1 << 30                 # a Launcher archive is about 195 MB; refuse anything near a gigabyte
MAX_UNPACKED, MAX_ENTRIES = 4 << 30, 200_000   # and one that unpacks to more than an app could be
FOLDER = 'launcher-update'        # in the Builder support folder: the download, the new app, the script, its log
CODESIGN, SPCTL, DITTO = '/usr/bin/codesign', '/usr/sbin/spctl', '/usr/bin/ditto'
LOCAL = ('http://127.0.0.1:', 'http://localhost:')   # the tests' server, as source_updates.curl allows

# The Launcher has quit when this runs. $1 its pid, $2 the verified new app, $3 the app to replace, $4 where the outcome
# is written for the next start, $5 the log. MNG_LAUNCHER_UPDATE_NO_OPEN=1 (tests): the app is not opened afterwards.
REPLACE = r'''#!/bin/sh
pid=$1; new=$2; app=$3; result=$4; log=$5
exec >>"$log" 2>&1
echo "$(date '+%Y-%m-%d %H:%M:%S') replacing $app with $new once pid $pid has quit"
finish() {
  printf '%s\n' "$1" >"$result"
  # -n: this app at this place, also while another copy of MNG Launcher is open from somewhere else
  [ "${MNG_LAUNCHER_UPDATE_NO_OPEN:-0}" = 1 ] || /usr/bin/open -n "$app"
  exit "$2"
}
n=0
while kill -0 "$pid" 2>/dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 600 ]; then
    echo "the Launcher did not quit in 60 s: nothing was replaced"
    printf '%s\n' '{"ok": false, "why": "running"}' >"$result"
    exit 1
  fi
  sleep 0.1
done
[ -d "$new/Contents/MacOS" ] || { echo "the new app is gone: nothing was replaced"; finish '{"ok": false, "why": "missing"}' 1; }
old="$(dirname "$app")/.$(basename "$app").replaced-$$"
if ! mv "$app" "$old"; then
  echo "the old app could not be moved aside: nothing was replaced"
  finish '{"ok": false, "why": "aside"}' 1
fi
if mv "$new" "$app"; then
  echo "replaced"
  rm -rf "$old"
  rmdir "$(dirname "$new")" 2>/dev/null   # the folder the new app was unpacked into, now empty
  finish '{"ok": true}' 0
fi
echo "the new app could not be put in place: the old one is back"
mv "$old" "$app"
finish '{"ok": false, "why": "place"}' 1
'''


class UpdateError(ValueError):
    """Why this Launcher is not replaced: said in the window, with the address of the manual download once a release
    is known."""
    manual = None


def emit(event, **fields):
    print(json.dumps({'event': event, **fields}, ensure_ascii=False), flush=True)


def is_local(url):
    return str(url or '').startswith(LOCAL)


def version_or_none(value):
    """A release's (major, minor, patch); None for a Launcher built from a checkout, which has no version."""
    try:
        return su.version_tuple(value)
    except su.SourceError:
        return None


# --- the release list -------------------------------------------------------------------------------------------------------
def newest(listing, local=False):
    """The highest launcher-v* release that carries the app's ZIP: {'version', 'url', 'size', 'manual'}, or None. Only an
    https address on github.com is taken (with a release list on this Mac, the tests', also that server's); 'manual' is
    where a person downloads it instead (source_updates.newest_launcher's rule: the disk image, else the page)."""
    def ours(url):
        parts = urllib.parse.urlsplit(str(url or ''))
        return (parts.scheme == 'https' and parts.hostname == 'github.com') or (local and is_local(url))

    best = None
    for r in listing if isinstance(listing, list) else []:
        tag = str(r.get('tag_name', '')) if isinstance(r, dict) else ''
        if not tag.startswith(su.LAUNCHER_TAG) or r.get('draft'):
            continue
        name = tag[len(su.LAUNCHER_TAG):]
        version = version_or_none(name)
        if version is None or (best is not None and version <= best[0]):
            continue
        for a in r.get('assets') or []:
            if not isinstance(a, dict) or a.get('name') != f'MNG-Launcher-{name}{ZIP_SUFFIX}':
                continue
            size, url = a.get('size'), a.get('browser_download_url')
            if type(size) is int and 0 < size <= MAX_ZIP and ours(url):
                manual = su.newest_launcher([r])
                best = (version, {'version': name, 'url': str(url), 'size': size, 'manual': manual['url'] if manual else None})
    return best[1] if best else None


# --- code signatures --------------------------------------------------------------------------------------------------------
def signing(app):
    """How an app is signed: its identifier, its team ('' for an ad-hoc one, a build from a checkout) and the text of its
    designated requirement."""
    done = subprocess.run([CODESIGN, '-d', '-r-', '--verbose=2', str(app)], capture_output=True, text=True)
    if done.returncode:
        raise UpdateError(su.text(f'This Launcher\'s code signature cannot be read: {done.stderr.strip()}',
                                  f'读不出这个 Launcher 的签名：{done.stderr.strip()}'))
    out = done.stdout + '\n' + done.stderr

    def field(name):
        m = re.search(rf'^{name}=(.*)$', out, re.M)
        return m.group(1).strip() if m else ''

    requirement = re.search(r'^(?:# )?designated => (.*)$', out, re.M)
    team = field('TeamIdentifier')
    return {'identifier': field('Identifier'), 'team': '' if team in ('', 'not set') else team,
            'requirement': re.sub(r'/\*.*?\*/', '', requirement.group(1)).strip() if requirement else ''}


def required_of_update(own):
    """What the new app's signature must satisfy: the running Launcher's designated requirement, and for a Developer ID
    Launcher its team said once more (should a release ever carry a requirement that names only the bundle ID)."""
    if not own['identifier'] or not own['requirement']:
        raise UpdateError(su.text('This Launcher has no code signature to compare an update with.',
                                  '这个 Launcher 没有可以用来核对更新的签名。'))
    if not own['team']:
        return own['requirement']
    if not re.fullmatch(r'[A-Z0-9]{10}', own['team']):
        raise UpdateError(su.text(f'Unexpected signing team: {own["team"]}', f'签名团队号不对：{own["team"]}'))
    return f'({own["requirement"]}) and anchor apple generic and certificate leaf[subject.OU] = "{own["team"]}"'


def check_app(new, own, version, running, same=False):
    """Raises UpdateError unless `new` is this Launcher's own newer release: bundle ID, version, a whole signature that
    satisfies the running one's requirement, and for a Developer ID Launcher Gatekeeper's word that it is notarized."""
    refuse = lambda en, cn: UpdateError(su.text(f'The download was not installed: {en}', f'下载的更新没有安装：{cn}'))
    try:
        info = plistlib.loads((Path(new) / 'Contents/Info.plist').read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError) as error:
        raise refuse(f'its Info.plist cannot be read ({error}).', f'读不出它的 Info.plist（{error}）。') from None
    if info.get('CFBundleIdentifier') != own['identifier']:
        raise refuse(f'it is another app ({info.get("CFBundleIdentifier")!r}).', f'它是另一个 App（{info.get("CFBundleIdentifier")!r}）。')
    if info.get('CFBundleShortVersionString') != version:
        raise refuse(f'it says version {info.get("CFBundleShortVersionString")!r}, the release {version}.',
                     f'它写的版本是 {info.get("CFBundleShortVersionString")!r}，发布的是 {version}。')
    if running is not None and (su.version_tuple(version) < running or (su.version_tuple(version) == running and not same)):
        raise refuse(f'{version} is not newer than this Launcher.', f'{version} 不比这个 Launcher 新。')
    whole = subprocess.run([CODESIGN, '--verify', '--deep', '--strict', str(new)], capture_output=True, text=True)
    if whole.returncode:
        raise refuse(f'its code signature is not whole ({whole.stderr.strip()}).', f'它的签名不完整（{whole.stderr.strip()}）。')
    ours = subprocess.run([CODESIGN, '--verify', '-R=' + required_of_update(own), str(new)], capture_output=True, text=True)
    if ours.returncode:
        raise refuse('it is not signed as this Launcher is.', '它的签名和这个 Launcher 不是同一个。')
    if own['team']:
        gate = subprocess.run([SPCTL, '--assess', '--type', 'execute', '--verbose=2', str(new)], capture_output=True, text=True)
        if gate.returncode or 'Notarized Developer ID' not in gate.stdout + gate.stderr:
            raise refuse('macOS does not accept it as a notarized app.', 'macOS 不认它是公证过的 App。')


# --- where the app is -------------------------------------------------------------------------------------------------------
def not_replaceable(app):
    """Why the app cannot be replaced where it is, in the player's words, or None."""
    app = Path(app)
    if '/AppTranslocation/' in str(app):
        return su.text('macOS runs this Launcher from a temporary copy. Move MNG Launcher into Applications with Finder, '
                       'open it from there, and update again.',
                       'macOS 正从一个临时副本运行这个 Launcher。请用访达把 MNG Launcher 拖进“应用程序”，从那里打开，再更新。')
    if not os.access(app.parent, os.W_OK | os.X_OK) or not os.access(app, os.W_OK):
        return su.text(f'{app.parent} cannot be written to from this account (or is a disk image).',
                       f'这个账户不能写 {app.parent}（或者它是磁盘映像）。')
    return None


def work_folder(support, app):
    """(where the download, the script and its log are kept, where the new app is unpacked): the support folder for both
    when it is on the app's disk (the move into place is then a rename), else a hidden folder beside the app for the
    new app."""
    base = Path(support) / FOLDER
    base.mkdir(parents=True, exist_ok=True)
    if os.stat(base).st_dev == os.stat(Path(app).parent).st_dev:
        return base, base
    return base, Path(app).parent / f'.{Path(app).stem} update'


# --- the steps ---------------------------------------------------------------------------------------------------------------
def download(url, target, size):
    """The archive through the system curl (source_updates.CURL: python.org's Python has no CA certificates), with the
    bytes so far every quarter of a second."""
    local = is_local(url)
    cmd = [su.CURL, '-fsSL', '--proto', '=https,http' if local else '=https', '--proto-redir', '=https,http' if local else '=https',
           '-A', su.USER_AGENT, '--max-filesize', str(MAX_ZIP), '--speed-limit', '1024', '--speed-time', '60',
           '--max-time', '3600', '-o', str(target), url]
    with subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True) as p:
        while p.poll() is None:
            time.sleep(0.25)
            emit('download', done=target.stat().st_size if target.exists() else 0, total=size)
        problem = p.stderr.read().strip()
    if p.returncode:
        raise UpdateError(su.text(f'The download failed (curl {p.returncode} {problem}).', f'下载失败（curl {p.returncode} {problem}）。'))
    if target.stat().st_size != size:
        raise UpdateError(su.text(f'The download is {target.stat().st_size} bytes, the release says {size}.',
                                  f'下载到 {target.stat().st_size} 字节，发布写的是 {size}。'))
    emit('download', done=size, total=size)


def check_archive(archive):
    """Raises UpdateError for an archive that is not one app's: a name or a link that would land outside the app when it
    is unpacked (nothing of the download is trusted before its signature is checked, and that is after unpacking), more
    than the app could hold, or anything beside the app at the top."""
    refuse = UpdateError(su.text('The download is not an archive of the app alone.', '下载的文件不是只装着这个 App 的压缩包。'))
    try:
        with zipfile.ZipFile(archive) as z:
            entries = z.infolist()
            if len(entries) > MAX_ENTRIES or sum(e.file_size for e in entries) > MAX_UNPACKED:
                raise refuse
            tops = set()
            for e in entries:
                parts = e.filename.rstrip('/').split('/')
                if e.filename.startswith('/') or '\\' in e.filename or '\0' in e.filename or any(p in ('', '.', '..') for p in parts):
                    raise refuse
                tops.add(parts[0])
                if stat.S_ISLNK(e.external_attr >> 16):   # a link must end inside the app it is in
                    target, depth = z.read(e).decode(), len(parts) - 1
                    if target.startswith('/') or '\0' in target:
                        raise refuse
                    for piece in target.split('/'):
                        depth += -1 if piece == '..' else 0 if piece in ('', '.') else 1
                        if depth < 1:
                            raise refuse
    except (zipfile.BadZipFile, OSError, UnicodeDecodeError, ValueError, NotImplementedError) as error:
        if isinstance(error, UpdateError):
            raise
        raise refuse from None
    if len(tops) != 1 or not next(iter(tops)).endswith('.app'):
        raise refuse


def unpack(archive, folder):
    """The one app the archive holds, unpacked with ditto (links and permissions as they were signed)."""
    check_archive(archive)
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    done = subprocess.run([DITTO, '-x', '-k', str(archive), str(folder)], capture_output=True, text=True)
    apps = [p for p in folder.iterdir() if p.suffix == '.app' and p.is_dir() and not p.is_symlink()]
    if done.returncode or len(apps) != 1:
        raise UpdateError(su.text('The download does not unpack to one app.', '下载的文件解不出一个 App。'))
    return apps[0]


def start_replacement(base, pid, new, app):
    """Starts the script on its own (a session of its own: the Launcher's quit does not end it) and returns its log."""
    script, result, log = base / 'replace.sh', base / 'result.json', base / 'replace.log'
    script.write_text(REPLACE)
    script.chmod(0o700)
    result.unlink(missing_ok=True)
    subprocess.Popen(['/bin/sh', str(script), str(pid), str(new), str(app), str(result), str(log)], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True, close_fds=True, cwd='/')
    return log


def stage(found, own, running, app, support, pid, same):
    """Downloads and checks the release, then starts the replacement. The download is removed either way, the unpacked
    app when it is refused."""
    base, place = work_folder(support, app)
    with (base / '.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise UpdateError(su.text('An update is already under way.', '已经在更新了。')) from None
        need = found['size'] * 4   # the archive, the app it unpacks to, and room to spare
        if shutil.disk_usage(base).free < need or shutil.disk_usage(Path(app).parent).free < need:
            raise UpdateError(su.text(f'Not enough free space for the update (about {need >> 20} MB).',
                                      f'磁盘空间不够放这次更新（约需 {need >> 20} MB）。'))
        emit('found', version=found['version'], size=found['size'])
        archive, folder = base / f'MNG-Launcher-{found["version"]}.zip', place / 'new'
        try:
            archive.unlink(missing_ok=True)
            download(found['url'], archive, found['size'])
            emit('verify', version=found['version'])
            new = unpack(archive, folder)
            check_app(new, own, found['version'], running, same)
        except BaseException:
            shutil.rmtree(folder, ignore_errors=True)
            raise
        finally:
            archive.unlink(missing_ok=True)
        start_replacement(base, pid, new, app)
        emit('install', version=found['version'])


def update(app, support, pid, feed=None, same=False):
    app = Path(app).resolve()
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    api = feed or (su.settings(app / 'Contents/Resources') or {}).get('api')
    if not api:
        raise UpdateError(su.text('A Launcher built from a checkout has no release list; update the checkout instead.',
                                  '从源码目录编出的 Launcher 没有发布列表；请更新源码目录。'))
    try:
        listing = json.loads(su.curl(api, max_time=15, api=True))
    except ValueError as error:   # SourceError is one too
        raise UpdateError(su.text(f'The release list could not be read ({error}).', f'读不到发布列表（{error}）。')) from None
    found = newest(listing, local=is_local(api))
    if not found:
        raise UpdateError(su.text('No MNG Launcher release with a download was found.', '没有找到带下载文件的 MNG Launcher 发布。'))
    try:
        running, latest = version_or_none(info.get('CFBundleShortVersionString')), su.version_tuple(found['version'])
        if running is not None and (latest < running or (latest == running and not same)):
            raise UpdateError(su.text(f'This is the newest MNG Launcher ({info.get("CFBundleShortVersionString")}).',
                                      f'已经是最新的 MNG Launcher（{info.get("CFBundleShortVersionString")}）。'))
        problem = not_replaceable(app)
        if problem:
            raise UpdateError(problem)
        own = signing(app)
        required_of_update(own)   # an unsigned Launcher is refused before anything is downloaded
        stage(found, own, running, app, support, pid, same)
    except UpdateError as error:
        error.manual = found['manual']   # the window offers the plain download with what went wrong
        raise


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='job', required=True)
    u = sub.add_parser('update', help='download, check and stage the newest Launcher; then replace this one once it quits')
    u.add_argument('--app', required=True, type=Path, help='the running MNG Launcher.app')
    u.add_argument('--support', required=True, type=Path, help='~/Library/Application Support/MacNativeGames/Builder')
    u.add_argument('--pid', required=True, type=int, help="the Launcher's process: the replacement waits for it to quit")
    u.add_argument('--feed', help='tests: another release list (https, or http on this Mac)')
    u.add_argument('--same', action='store_true', help='tests: the same version may be installed again')
    a = ap.parse_args(argv)
    try:
        update(a.app, a.support, a.pid, a.feed, a.same)
    except (UpdateError, su.SourceError, OSError, plistlib.InvalidFileException) as error:
        emit('error', message=str(error), manual=getattr(error, 'manual', None))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
