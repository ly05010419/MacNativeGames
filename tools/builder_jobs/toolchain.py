"""Where the Metal compiler is, when xcode-select does not point at it.

Every game's shader libraries are built with `metal`, which lives in Xcode's Metal toolchain and not in the Command
Line Tools. On a Mac whose `xcode-select -p` is /Library/Developer/CommandLineTools -- what `xcode-select --install`
leaves, and what many Macs have -- `metal` is not on the path, so a cold build fails in the middle of step 3 with
"no Metal compiler: install the Metal toolchain ... or set DEVELOPER_DIR to Xcode", after minutes of work. The
compiler is usually installed all the same: it is reachable by pointing DEVELOPER_DIR at an Xcode.

Found 2026-11-16 building Metroid Dread: the earlier games had passed only because their shader libraries were
already built, so nothing called the compiler, and the window had said "This Mac has everything the build needs".

  metal_ready(env)            `metal` is reachable with this environment.
  developer_dir_for_metal()   the DEVELOPER_DIR that makes it reachable, or None when it already is (nothing to set)
                              or no Xcode here has it (nothing can be done from a build: checks.py says so first).

Nothing here changes the Mac: xcode-select is the player's, and switching it needs their password.
"""
import os
from pathlib import Path
import subprocess

CANDIDATES = ('/Applications/Xcode.app/Contents/Developer',)


def _find_metal(env):
    try:
        r = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, timeout=60, env=env)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def metal_ready(env=None):
    """Whether `metal` is reachable with this environment (the build's own, by default)."""
    return _find_metal(dict(env if env is not None else os.environ))


def xcode_developer_dirs():
    """Every developer directory on this Mac that could hold the Metal toolchain, in the order to try them.

    The one xcode-select points at comes first, so a player who has already chosen an Xcode keeps it."""
    out, seen = [], set()
    try:
        r = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True, timeout=60)
        chosen = r.stdout.strip() if r.returncode == 0 else ''
    except (OSError, subprocess.TimeoutExpired):
        chosen = ''
    globbed = sorted(str(p / 'Contents/Developer') for p in Path('/Applications').glob('Xcode*.app'))
    for path in (chosen, *CANDIDATES, *globbed):
        if path and path not in seen and Path(path).is_dir():
            seen.add(path)
            out.append(path)
    return out


def developer_dir_for_metal(env=None):
    """The DEVELOPER_DIR a build needs for `metal`, or None.

    None means either that `metal` is already reachable, so nothing must be set, or that no Xcode here has the
    toolchain, so setting anything would not help and the player has to install it."""
    base = dict(env if env is not None else os.environ)
    if _find_metal(base):
        return None
    for path in xcode_developer_dirs():
        if _find_metal({**base, 'DEVELOPER_DIR': path}):
            return path
    return None
