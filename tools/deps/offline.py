"""The Launcher's offline store: everything a build would download, carried inside the app, so a player packages any game
with no network (user 2026-11-20: "launcher 的目标 用户安装就能用 不需要 联网， 所有东西 我们打包的时候 都放进去";
2026-11-19: "我的 用户 大多在中国", where GitHub cannot be reached). tools/deps/offline_pack.py makes it at a release;
MNG-Launcher's bundled_job.py names it in MNG_OFFLINE_STORE (Contents/Resources/offline).

    files/<sha256 or sha512>    every pinned download by its digest: source archives, fonts, icons
    wheels/                     every Python wheel a build installs (pip --no-index --find-links)
    git/<name>.git              shallow bare repositories holding every pinned commit (git's url.<store>.insteadOf)
    manifest.json               what is in it, and the git address each repository stands for

activate() points pip and git at the store through their own settings in the environment, which every process a job
starts inherits: no build script needs to know about it, and anything missing from the store fails at once instead of
reaching the network. fetch() (core.py) takes a file from files/ before it would download.
Python 3.9 safe, as core.py."""
import json
import os
from pathlib import Path

STORE_ENV = 'MNG_OFFLINE_STORE'
MANIFEST = 'manifest.json'
SCHEMA = 2

# Pinned in tools/deps/pins.py but not carried: no game the Launcher builds uses them (user 2026-11-21: "去掉 .net sdk",
# 231 MB; 2026-11-22: Skyward Sword HD and WORLDEND SYNDROME, the games that ran .NET programs and LibHac, are no longer
# built by the Launcher; TMNT's translator is prebuilt). A checkout still makes them for those games' command-line builds.
NOT_CARRIED = ('dotnet', 'libhac')


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


def environment(folder, base=None):
    """The variables that send pip and git to the store (folder): a new dict from base (os.environ)."""
    base = dict(os.environ if base is None else base)
    data = manifest(folder)
    env = dict(base)
    env[STORE_ENV] = str(folder)
    env['PIP_NO_INDEX'] = '1'
    env['PIP_FIND_LINKS'] = str(folder / 'wheels')
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
