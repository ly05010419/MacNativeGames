#!/usr/bin/env python3
"""The environment of every build step, made from nothing: only what is listed here reaches them.
The build tools read many variables (DM_GAMES_DIR, DM_SPIN_LIST, XLATE, DMSH_CAS, PYTHONPATH...), so an inherited one could change a build without any file saying so.
DOTNET_ROOT is not inherited either: it is found on disk from the steps' own PATH (dotnet_root)."""
import os
import shutil
from pathlib import Path

KEPT = ('HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE')
PATH = ('/usr/bin', '/bin', '/usr/sbin', '/sbin', '/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet')
XCODE = '/Applications/Xcode.app/Contents/Developer'


def dotnet_root(path=PATH):
    """The .NET folder (the one holding host/fxr) of the first dotnet on the steps' PATH, or None. The `dotnet` command
    finds its runtime by itself, but a published apphost such as nx_extract looks only at DOTNET_ROOT, the registered
    location and /usr/local/share/dotnet, so with Homebrew's .NET it stopped with "You must install .NET" (#2073).
    Homebrew's bin/dotnet is a link to libexec/dotnet, or a script beside libexec, depending on the formula version.
    Only the dotnet the steps would run counts (the first executable one), as a later one is never used."""
    command = shutil.which('dotnet', path=os.pathsep.join(path))
    if not command:
        return None
    real = Path(command).resolve().parent
    return next((str(c) for c in (real, real.parent / 'libexec') if (c / 'host/fxr').is_dir()), None)


def environment(work, title, source=None):
    work = Path(work)
    env = {k: os.environ[k] for k in KEPT if k in os.environ}
    env.update(PATH=os.pathsep.join(PATH), DEVELOPER_DIR=XCODE, LA_BUILD_ENV='1', DM_GAME=title, DM_METAL_RUNTIME='0', J='8',
               TMPDIR=str(work / 'tmp'), NUGET_PACKAGES=str(work / 'cache/nuget'), DOTNET_CLI_HOME=str(work / 'cache/dotnet'),
               DM_NUGET_SOURCE=str(work / 'dependencies/nuget'), PYTHONDONTWRITEBYTECODE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true', MACOSX_DEPLOYMENT_TARGET='26.0')
    root = dotnet_root()
    if root:
        env['DOTNET_ROOT'] = root
    for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env
