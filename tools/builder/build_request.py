"""Preparing one build's request (S1.7b, C3; S2.1, T6): the log's name, the argv build_app.py gets, and the request the
worker hands to BuildRun. Moved out of controller.py in S5.2 to keep it under 300 lines; the controller re-exports
them."""
from datetime import datetime
import os
from pathlib import Path

from tools.builder.build_run import BuildRequest
from tools.builder.checks import BuildPaths
from tools.builder.command import build_argv
from tools.builder.config import SavedPaths
from tools.builder.paths import effective_paths
from tools.builder.ports import Outcome as CommandOutcome

LOG_NAMES = 100


def reserve_log(logs_dir, game, wall):
    """C3: an exclusive, never-overwriting log name; the file is created empty and closed."""
    logs_dir = Path(logs_dir)
    logs_dir.mkdir(parents=True, exist_ok=True)
    base = f'builder-{game}-{datetime.fromtimestamp(wall).strftime("%Y%m%d-%H%M%S")}'
    for n in range(1, LOG_NAMES + 1):
        path = logs_dir / (f'{base}.log' if n == 1 else f'{base}-{n}.log')
        try:
            os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644))
            return path
        except FileExistsError:
            continue
    raise FileExistsError(f'no free log name for {base} (tried {LOG_NAMES})')


def request_argv(spec, fields, repo_root, python, code_mod=None):
    """A snapshot's raw BuildPaths and the argv build_app.py gets; shared by prepare_request and planned_command
    (S2.1, T6). With checks ready, build_argv raises no ValueError (input_rules and input_files block first)."""
    raw = BuildPaths(**{f: Path(v) if v else None for f, v in fields})
    return raw, build_argv(spec, raw, repo_root, python, code_mod)


def prepare_request(spec, fields, repo_root, python, logs_dir, wall, runner, cancel, compose_code=None):
    """Worker job: the BuildRequest, or None when the cancel came first (no log is reserved then). compose_code
    (S5.4) composes the code mods of this snapshot here, at the build's start: (folder, record, on_success)."""
    if cancel.is_set():
        return None
    code_mod, record, on_success = compose_code() if compose_code else (None, (), None)
    raw, argv = request_argv(spec, fields, repo_root, python, code_mod)
    log_path = reserve_log(logs_dir, spec.id, wall)
    git = runner.run(['git', '-C', str(repo_root), 'rev-parse', 'HEAD'])
    commit = git.stdout.strip() if git.outcome is CommandOutcome.OK and git.stdout.strip() else 'unknown'
    return BuildRequest(spec.id, effective_paths(spec, raw), SavedPaths(**dict(fields)), tuple(argv), log_path,
                        commit, python, record, on_success)
