#!/usr/bin/env python3
"""The environment of every build step, made from nothing: only what is listed here reaches them.
The build tools read many variables (DM_GAMES_DIR, DM_SPIN_LIST, XLATE, DMSH_CAS, PYTHONPATH...), so an inherited one could change a build without any file saying so."""
import os
from pathlib import Path

KEPT = ('HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE')
PATH = ('/usr/bin', '/bin', '/usr/sbin', '/sbin', '/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet')
XCODE = '/Applications/Xcode.app/Contents/Developer'


def environment(work, title, source=None):
    work = Path(work)
    env = {k: os.environ[k] for k in KEPT if k in os.environ}
    env.update(PATH=os.pathsep.join(PATH), DEVELOPER_DIR=XCODE, SS_BUILD_ENV='1', DM_GAME=title, DM_METAL_RUNTIME='0', J='8', DM_ALLOW_OTHER_METAL='1',
               TMPDIR=str(work / 'tmp'), NUGET_PACKAGES=str(work / 'cache/nuget'), DOTNET_CLI_HOME=str(work / 'cache/dotnet'),
               DM_NUGET_SOURCE=str(work / 'dependencies/nuget'), PYTHONDONTWRITEBYTECODE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true', MACOSX_DEPLOYMENT_TARGET='26.0')
    for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env
