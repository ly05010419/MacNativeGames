#!/usr/bin/env python3
"""The environment of every build step, made from nothing: only what is listed here reaches them.
The build tools read many variables (DM_GAMES_DIR, DM_SPIN_LIST, XLATE, DMSH_CAS, PYTHONPATH...), so an inherited one could change a build without any file saying so.
zstd, spirv-cross and the .NET SDK 10 come from MacNativeGames' tools/deps, which makes them once per Mac from pinned sources (~/Library/Caches/MacNativeGames/deps),
first on PATH: no Homebrew, and every Mac builds with the same tools. The rest of PATH is the system's (Xcode's make, clang, xcrun, codesign, git; python3).
DOTNET_ROOT is not inherited either: it names that SDK. The `dotnet` command finds its runtime by itself, but a published apphost such as nx_extract or the
shader translator looks only at DOTNET_ROOT, the registered location and /usr/local/share/dotnet, and stopped with "You must install .NET" without it (#2073)."""
import os
from pathlib import Path
import sys

KEPT = ('HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE')
PATH = ('/usr/bin', '/bin', '/usr/sbin', '/sbin')
TOOLS = ('zstd', 'spirv-cross', 'dotnet')   # what the steps run from PATH; xcodegen and glslangValidator only an iPhone build or an OpenGL game need
XCODE = '/Applications/Xcode.app/Contents/Developer'
# games/la/scripts/la_env.py → parents[3] is the MacNativeGames checkout; MNG_REPO when a process that imported tools/deps started this one
REPO = Path(os.environ.get('MNG_REPO') or Path(__file__).resolve().parents[3])


def pinned_tools():
    """The PATH folders of TOOLS and the .NET SDK's folder (DOTNET_ROOT), made the first time on this Mac."""
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    from tools.deps import DepsError, env, path
    try:
        return path(*TOOLS), env('dotnet')['DOTNET_ROOT']
    except DepsError as error:
        raise ValueError(f'a build tool could not be made from its pinned source: {error}') from error


def environment(work, title, source=None):
    work = Path(work)
    folders, dotnet_root = pinned_tools()
    env = {k: os.environ[k] for k in KEPT if k in os.environ}
    env.update(PATH=os.pathsep.join([*folders, *PATH]), DOTNET_ROOT=dotnet_root, DEVELOPER_DIR=XCODE, LA_BUILD_ENV='1', DM_GAME=title,
               DM_METAL_RUNTIME='0', J='8', TMPDIR=str(work / 'tmp'), NUGET_PACKAGES=str(work / 'cache/nuget'), DOTNET_CLI_HOME=str(work / 'cache/dotnet'),
               DM_NUGET_SOURCE=str(work / 'dependencies/nuget'), PYTHONDONTWRITEBYTECODE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true', MACOSX_DEPLOYMENT_TARGET='26.0')
    for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env
