"""Is a newer version of MacNativeGames out, and the fast-forward update (S4.1, U1-U2; S4.5 revision 2). The model:
no view, no rule about when to ask.

Updates come from MacNativeGames only, found by URL: the remotes whose fetch URL normalises to PROJECT (the one main
follows if several, else the first by name; none: NO_PROJECT, nothing fetched). Only that remote is fetched (`git fetch
--quiet <remote>`, never a bare fetch, so a fork or a private remote is not contacted), and HEAD is compared with
`<remote>/main`. On main the states are as in S4.1 and Update is `git merge --ff-only <remote>/main`; on any other
branch waiting commits are OTHER_BRANCH (merge by hand) and there is no update. Only the fetch and that fast-forward
change anything, and only the repository's refs and files: nothing here resets, stashes or makes a merge commit. Every
git call goes through the injected runner (CommandRunner) with a timeout and GIT_TERMINAL_PROMPT=0, so a remote that
asks for a password fails at once instead of waiting.

S4.4b (B2, E2): only the builder's own clone is checked or updated: git's top level must be the builder's folder
(`samefile`: symlinks and letter case), so a ZIP unpacked inside another work tree is "not a git clone". A git call
that fails while looking for local changes counts as changes, and a failed count as offline, never as "clean"."""
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
import re

from tools.builder.ports import Outcome

LOCAL_S, FETCH_S, PULL_S = 10.0, 30.0, 120.0      # PULL_S bounds the fast-forward itself
MAX_SUBJECTS = 20
PROJECT = 'github.com/ly05010419/MacNativeGames'
MAIN = 'main'


class UpdateState(Enum):
    UP_TO_DATE = 'up_to_date'
    AVAILABLE = 'available'           # the upstream has commits this checkout lacks, and a fast-forward is possible
    LOCAL_CHANGES = 'local_changes'   # commits are waiting, but tracked files were changed here
    DIVERGED = 'diverged'             # commits are waiting, but this branch also has its own: no fast-forward
    OTHER_BRANCH = 'other_branch'     # commits are waiting, and the checkout is not main: merge by hand
    NOT_GIT = 'not_git'               # a ZIP download, or a folder without git
    NO_PROJECT = 'no_project'         # no remote points at MacNativeGames
    OFFLINE = 'offline'               # the fetch failed: no network, a moved remote, a remote that wants a password


@dataclass(frozen=True)
class UpdateStatus:
    state: UpdateState
    branch: str | None = None
    upstream: str | None = None
    waiting: int = 0                  # commits on the upstream that this checkout lacks
    subjects: tuple = ()              # their '<short hash> <subject>', newest first, at most MAX_SUBJECTS
    detail: str = ''


@dataclass(frozen=True)
class UpdateResult:
    ok: bool
    old: str | None                   # HEAD before, and after: the same when nothing moved
    new: str | None
    detail: str = ''


def _git(runner, repo, *args, timeout_s=LOCAL_S):
    return runner.run(['/usr/bin/env', 'GIT_TERMINAL_PROMPT=0', 'git', '-C', str(repo), *args], timeout_s=timeout_s)


def _out(result):
    return result.stdout.strip() if result.outcome is Outcome.OK else None


def _count(runner, repo, spec):
    """The number of commits in spec, or None when git did not answer with one."""
    value = _out(_git(runner, repo, 'rev-list', '--count', spec))
    return int(value) if value and value.isdigit() else None


def _own_clone(runner, repo):
    """git's top level is the builder's folder itself (OSError when that folder is gone: the caller reports it)."""
    top = _out(_git(runner, repo, 'rev-parse', '--show-toplevel'))
    return bool(top) and Path(top).samefile(repo)


def _tail(result, limit=300):
    text = (result.stderr or result.stdout or result.outcome.name).strip()
    return text if len(text) <= limit else '...' + text[-limit:]


def normalise(url):
    """host/path of a git URL (https, ssh://, git@host:path; user, port, trailing .git and slashes dropped; the host
    in lower case); a local path stays a path, less its trailing slashes and .git."""
    url = url.strip()
    found = (re.match(r'^[a-z][a-z0-9+.-]*://(?:[^@/]+@)?([^/:]+)(?::\d+)?/(.*)$', url, re.I)
             or re.match(r'^(?:[^@/:]+@)?([^/:]+):(?!//)(.+)$', url))
    host, path = (found[1].lower() + '/', found[2]) if found and not url.startswith('/') else ('', url)
    path = path.rstrip('/')
    return host + (path[:-4] if path.endswith('.git') else path).rstrip('/')


def project_remote(runner, repo, project=PROJECT):
    """The remote that points at the project, or None (several: the one main follows, else the first by name)."""
    listed = _out(_git(runner, repo, 'config', '--get-regexp', r'^remote\..*\.url$')) or ''
    want, names = normalise(project), []
    for line in listed.splitlines():
        key, _, url = line.partition(' ')
        if key.startswith('remote.') and key.endswith('.url') and normalise(url) == want:
            names.append(key[len('remote.'):-len('.url')])
    followed = _out(_git(runner, repo, 'config', '--get', f'branch.{MAIN}.remote'))
    return followed if followed in names else min(names, default=None)


def _dirty(runner, repo):
    """Tracked files changed here (untracked files do not stop a fast-forward); a failed status counts as changed."""
    result = _git(runner, repo, 'status', '--porcelain', '--untracked-files=no')
    return result.outcome is not Outcome.OK or bool(result.stdout.strip())


def check_updates(runner, repo, project=PROJECT):
    """U1: fetch the project's remote, then compare with its main. Read-only for the player's files."""
    if not _own_clone(runner, repo):
        return UpdateStatus(UpdateState.NOT_GIT)
    branch = _out(_git(runner, repo, 'rev-parse', '--abbrev-ref', 'HEAD'))
    remote = project_remote(runner, repo, project)
    if remote is None:
        return UpdateStatus(UpdateState.NO_PROJECT, branch)
    upstream = f'{remote}/{MAIN}'
    fetched = _git(runner, repo, 'fetch', '--quiet', remote, timeout_s=FETCH_S)
    if fetched.outcome is not Outcome.OK:
        return UpdateStatus(UpdateState.OFFLINE, branch, upstream, detail=_tail(fetched))
    waiting, ahead = _count(runner, repo, f'HEAD..{upstream}'), _count(runner, repo, f'{upstream}..HEAD')
    if waiting is None or ahead is None:
        return UpdateStatus(UpdateState.OFFLINE, branch, upstream, detail='git could not count the commits')
    if waiting == 0:
        return UpdateStatus(UpdateState.UP_TO_DATE, branch, upstream)
    log = _out(_git(runner, repo, 'log', f'--max-count={MAX_SUBJECTS}', '--format=%h %s', f'HEAD..{upstream}')) or ''
    subjects = tuple(log.splitlines())
    if branch != MAIN:
        state = UpdateState.OTHER_BRANCH
    elif ahead:
        state = UpdateState.DIVERGED
    elif _dirty(runner, repo):
        state = UpdateState.LOCAL_CHANGES
    else:
        state = UpdateState.AVAILABLE
    return UpdateStatus(state, branch, upstream, waiting, subjects)


def apply_update(runner, repo, project=PROJECT):
    """U2: on main only, fetch the project's remote, then a fast-forward to its main, refused before it starts when
    tracked files were changed here. A branch that cannot fast-forward (it has commits of its own) fails in git
    itself and changes nothing."""
    if not _own_clone(runner, repo):
        return UpdateResult(False, None, None, 'not the builder\'s own git clone: download the new version')
    old = _out(_git(runner, repo, 'rev-parse', '--short', 'HEAD'))
    if _out(_git(runner, repo, 'rev-parse', '--abbrev-ref', 'HEAD')) != MAIN:
        return UpdateResult(False, old, old, f'not on {MAIN}: merge by hand')
    remote = project_remote(runner, repo, project)
    if remote is None:
        return UpdateResult(False, old, old, f'no remote points at MacNativeGames ({PROJECT})')
    if _dirty(runner, repo):
        return UpdateResult(False, old, old, 'local changes: update by hand')
    fetched = _git(runner, repo, 'fetch', '--quiet', remote, timeout_s=FETCH_S)
    if fetched.outcome is not Outcome.OK:
        return UpdateResult(False, old, old, _tail(fetched))
    merged = _git(runner, repo, 'merge', '--ff-only', '--quiet', f'{remote}/{MAIN}', timeout_s=PULL_S)
    new = _out(_git(runner, repo, 'rev-parse', '--short', 'HEAD'))
    if merged.outcome is not Outcome.OK:
        return UpdateResult(False, old, new, _tail(merged))
    return UpdateResult(True, old, new)
