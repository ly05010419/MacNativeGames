#!/usr/bin/env python3
"""The environment of every build step, made from nothing: only what is listed here reaches them.
The build tools read many variables (DM_GAMES_DIR, DM_SPIN_LIST, XLATE, DMSH_CAS, PYTHONPATH...), so an inherited one could change a build without any file saying so.
zstd, spirv-cross, the .NET SDK 10, libvpx and dav1d come from MacNativeGames' tools/deps (tools/deps/pins.py), made once per Mac from pinned sources
(~/Library/Caches/MacNativeGames/deps): no Homebrew, and every Mac builds with the same tools. The rest of PATH is the system's (Xcode's make, clang, xcrun, codesign, git).
SPIRV_CROSS names the pinned spirv-cross for the shader chain (dm/gfx/tools/dmsh_chain.py), VPX_PREFIX and DAV1D_PREFIX the libraries for the host (dm/host/Makefile),
DOTNET_ROOT the SDK, which the shader translator's apphost needs to find its runtime."""
import os
from pathlib import Path
import sys
from functools import lru_cache

KEPT = ('HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE')
PATH = ('/usr/bin', '/bin', '/usr/sbin', '/sbin')
TOOLS = ('zstd', 'spirv-cross', 'dotnet')   # what the steps run from PATH
# games/worldend/scripts/worldend_env.py → parents[3] is the MacNativeGames checkout; MNG_REPO when a process that imported tools/deps started this one
REPO = Path(os.environ.get('MNG_REPO') or Path(__file__).resolve().parents[3])


def pinned():
    """PATH folders of TOOLS and the variables naming the pinned spirv-cross, .NET SDK, libvpx and dav1d, made the first time on this Mac."""
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    from tools.deps import DepsError, env, path, program
    try:
        return path(*TOOLS), dict(env('dotnet', 'libvpx', 'dav1d'), SPIRV_CROSS=str(program('spirv-cross')))
    except DepsError as error:
        raise ValueError(f'a build dependency could not be made from its pinned source: {error}') from error


@lru_cache(maxsize=4)
def developer_directory(given):
    if given:
        return given
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    from build_app import metal_compiler
    found = metal_compiler()
    if not found:
        raise ValueError('Xcode with the Metal toolchain is required; select it with DEVELOPER_DIR')
    return found[2]['DEVELOPER_DIR']


def environment(work, title, source=None):
    work = Path(work)
    folders, deps = pinned()
    env = {k: os.environ[k] for k in KEPT if k in os.environ}
    env.update(deps)
    developer = developer_directory(os.environ.get('DEVELOPER_DIR'))
    env.update(PATH=os.pathsep.join([*folders, *PATH]), DEVELOPER_DIR=developer, MNG_REPO=str(REPO), WORLDEND_BUILD_ENV='1', DM_GAME=title, DM_METAL_RUNTIME='0', J='8',
               TMPDIR=str(work / 'tmp'), NUGET_PACKAGES=str(work / 'cache/nuget'), DOTNET_CLI_HOME=str(work / 'cache/dotnet'),
               DM_NUGET_SOURCE=str(work / 'dependencies/nuget'), PYTHONDONTWRITEBYTECODE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true', MACOSX_DEPLOYMENT_TARGET='26.0')
    for key in ('TMPDIR', 'NUGET_PACKAGES', 'DOTNET_CLI_HOME'):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env
