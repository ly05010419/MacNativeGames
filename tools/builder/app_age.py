"""Whether the chosen game's app is older than the code that builds it (S8.1, revision 2). The model: no Tk.

older(runner, repo_root, spec, fields) answers True when a commit that changes what a build uses (build_app.py,
vendor/, games/totk/, which both games share, and the game's own folder) is in this checkout's history and not in the
app's. The app names the commit it was built from in its Info.plist (MNGSourceRevision, or build_app.py's short
MNGSourceCommit); when it names none, or one this checkout does not have, the newest such commit's time is compared
with the app's build time instead (the Info.plist's mtime: the build writes it, nothing else touches it). The app's
path is the build's own (paths.effective_paths: the chosen output, else next to the game file). None when nothing can
be said: no game file and no output chosen, no app there, or git cannot answer (a ZIP download, no git). Only git
runs; nothing is written."""
import dataclasses
from pathlib import Path
import plistlib

from tools.builder.checks import BuildPaths
from tools.builder.paths import effective_paths
from tools.builder.ports import Outcome

SHARED = ('build_app.py', 'vendor', 'games/totk')


def watched(spec):
    """The repository paths whose changes make an app of spec's game older than the code."""
    return tuple(sorted({*SHARED, Path(spec.assets_dir).parent.as_posix()}))


def app_path(spec, fields):
    """The app a build of these path fields ((field, value) pairs, any subset) makes, or None when neither the game
    file nor the output is chosen."""
    given = dict(fields)
    raw = BuildPaths(**{f.name: Path(given[f.name]) if given.get(f.name) else None
                        for f in dataclasses.fields(BuildPaths)})
    if raw.out is None and raw.game is None:
        return None
    if raw.out is not None:
        return Path(raw.out).expanduser().resolve()
    return effective_paths(spec, raw).out


def built_from(plist):
    """The commit the app at plist was built from, or None."""
    try:
        info = plistlib.loads(plist.read_bytes())
    except (OSError, ValueError, plistlib.InvalidFileException):
        return None
    commit = info.get('MNGSourceRevision') or info.get('MNGSourceCommit') if isinstance(info, dict) else None
    return commit if isinstance(commit, str) and commit.isalnum() else None


def older(runner, repo_root, spec, fields):
    app = app_path(spec, fields)
    plist = app / 'Contents/Info.plist' if app is not None else None
    if plist is None or not plist.is_file():
        return None
    git = ['git', '-C', str(repo_root)]
    commit = built_from(plist)
    if commit is not None:
        r = runner.run([*git, 'rev-list', '--count', f'{commit}..HEAD', '--', *watched(spec)])
        if r.outcome is Outcome.OK and r.stdout.strip().isdigit():
            return int(r.stdout) > 0
    r = runner.run([*git, 'log', '-1', '--format=%ct', '--', *watched(spec)])
    stamp = r.stdout.strip() if r.outcome is Outcome.OK else ''
    if not stamp.isdigit():
        return None
    return int(stamp) > plist.stat().st_mtime
