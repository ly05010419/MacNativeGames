"""The Launcher's offline store: everything a build would download, carried inside the app, so a player packages any game
with no network (user 2026-11-20: "launcher 的目标 用户安装就能用 不需要 联网， 所有东西 我们打包的时候 都放进去";
2026-11-19: "我的 用户 大多在中国", where GitHub cannot be reached). tools/deps/offline_pack.py makes it at a release;
MNG-Launcher's bundled_job.py names it in MNG_OFFLINE_STORE (Contents/Resources/offline).

    files/<sha256 or sha512>    every pinned download by its digest: source archives, fonts, icons
    wheels/                     every Python wheel a build installs (pip --no-index --find-links)
    git/<name>.git              shallow bare repositories holding every pinned commit (git's url.<store>.insteadOf)
    built/<folder>/             each library and program of tools/deps made at the release, as tools/deps installs it
                                into its cache (<folder> is that cache folder's name), with its licences: a player's
                                Mac compiles none of them, and the store needs none of their sources (2026-11-22)
    manifest.json               what is in it, the git address each repository stands for, each built file's SHA-256

activate() points pip and git at the store through their own settings in the environment, which every process a job
starts inherits: no build script needs to know about it, and anything missing from the store fails at once instead of
reaching the network. fetch() (core.py) takes a file from files/ before it would download; prefix() (__init__.py) takes
a built/ folder before it would make one. Python 3.9 safe, as core.py."""
import hashlib
import json
import os
from pathlib import Path

STORE_ENV = 'MNG_OFFLINE_STORE'
MANIFEST = 'manifest.json'
SCHEMA = 2

# Pinned in tools/deps/pins.py (SOURCES, PACKAGES, BUILD_TOOLS) but not carried: no Mac build of a game the Launcher makes
# uses them, and a checkout still makes them for what does. The .NET SDK (231 MB) and LibHac: user 2026-11-21 "去掉 .net
# sdk"; Skyward Sword HD and WORLDEND SYNDROME, which ran .NET programs, are no longer built by the Launcher (2026-11-22)
# and TMNT's translator is prebuilt. CMake (54 MB), glslang and XcodeGen: iPhone / iPad builds and glslang only, since
# spirv-cross is compiled with clang alone (user 2026-11-22: "好").
NOT_CARRIED = ('dotnet', 'libhac', 'cmake', 'glslang', 'xcodegen')


def store():
    """The store folder when MNG_OFFLINE_STORE names one with a manifest, else None (a checkout: downloads as before)."""
    named = os.environ.get(STORE_ENV)
    if not named:
        return None
    folder = Path(named)
    return folder if (folder / MANIFEST).is_file() else None


def manifest(folder=None):
    folder = folder or store()
    return json.loads((folder / MANIFEST).read_text()) if folder else None


def stored_file(want):
    """The store's copy of a pinned download (want: its SHA-256 or SHA-512), or None."""
    folder = store()
    if folder is None:
        return None
    path = folder / 'files' / want
    return path if path.is_file() else None


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_files(folder):
    """{relative name: SHA-256 (a link: 'link:' and its target)} of everything in a folder."""
    folder = Path(folder)
    out = {}
    for p in sorted(folder.rglob('*')):
        if p.is_symlink():
            out[p.relative_to(folder).as_posix()] = 'link:' + os.readlink(p)
        elif p.is_file():
            out[p.relative_to(folder).as_posix()] = file_sha256(p)
    return out


def built(name):
    """The store's built/<name> (a tools/deps cache folder made at the release), or None: no store, or not carried built.
    One whose files differ from the manifest is refused: a player's build would otherwise link a damaged library."""
    folder = store()
    if folder is None:
        return None
    files = (manifest(folder).get('built') or {}).get(name)
    if files is None:
        return None
    path = folder / 'built' / name
    if not path.is_dir() or tree_files(path) != files:
        raise ValueError(f'{path}: the Launcher\'s copy of {name} is damaged; reinstall the Launcher')
    return path


def environment(folder, base=None):
    """The variables that send pip and git to the store (folder): a new dict from base (os.environ)."""
    base = dict(os.environ if base is None else base)
    data = manifest(folder)
    env = dict(base)
    env[STORE_ENV] = str(folder)
    env['PIP_NO_INDEX'] = '1'
    # A file: URL, not the path: pip splits PIP_FIND_LINKS at spaces, and the store is under "Application Support" or
    # inside "MNG Launcher.app" (2026-11-24: 'Location /Users/…/Library/Application is ignored', no wheel found).
    env['PIP_FIND_LINKS'] = Path(folder, 'wheels').resolve().as_uri()
    env['PIP_DISABLE_PIP_VERSION_CHECK'] = '1'
    repos = data.get('git', {})
    count = int(base.get('GIT_CONFIG_COUNT', '0') or 0)
    for name, urls in sorted(repos.items()):
        for url in urls:
            env[f'GIT_CONFIG_KEY_{count}'] = f'url.{folder / "git" / (name + ".git")}.insteadOf'
            env[f'GIT_CONFIG_VALUE_{count}'] = url
            count += 1
    env['GIT_CONFIG_COUNT'] = str(count)
    env['GIT_TERMINAL_PROMPT'] = '0'
    env['DOTNET_CLI_TELEMETRY_OPTOUT'] = '1'
    env['DOTNET_NOLOGO'] = '1'
    env['DOTNET_SKIP_FIRST_TIME_EXPERIENCE'] = '1'
    env['DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE'] = '1'
    return env


def activate():
    """Puts environment(store()) into os.environ when a store is named; returns the store or None. Idempotent."""
    folder = store()
    if folder is None or os.environ.get('MNG_OFFLINE_ACTIVE') == str(folder):
        return folder
    os.environ.update(environment(folder))
    os.environ['MNG_OFFLINE_ACTIVE'] = str(folder)
    return folder
