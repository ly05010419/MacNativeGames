"""Preflight checks: their result, the context they read, and the runner that runs them in order (S1.2a, K1-K4).

Checks return message ids, not text (S2.1 has the text)."""
from dataclasses import dataclass, field
from enum import Enum
import os
from pathlib import Path
import platform
from typing import Callable, Mapping, Protocol

from tools.builder.games import GameSpec


class Status(Enum):
    OK = 'ok'
    WARN = 'warn'
    BLOCK = 'block'
    SKIP = 'skip'       # not run: a check it requires ended BLOCK or SKIP


@dataclass(frozen=True)
class CheckResult:
    id: str
    status: Status
    message_id: str
    detail: str = ''
    fix: str | None = None      # an exact command, or None when no single command is safe


@dataclass(frozen=True)
class BuildPaths:
    """The files and folders of one build (read by S1.2b's checks)."""
    game: Path
    keys: Path
    update: Path | None = None
    firmware: Path | None = None
    shared_fonts: Path | None = None
    work: Path | None = None
    out: Path | None = None


def _mac_version():
    return platform.mac_ver()[0]


@dataclass(frozen=True)
class CheckContext:
    """Everything a check reads, injected so tests need no real machine state."""
    runner: object                      # tools.builder.ports.CommandRunner or a fake with the same run()
    repo_root: Path = Path('.')
    spec: GameSpec | None = None
    paths: BuildPaths | None = None
    disk: object = None                 # tools.builder.ports.DiskInfo (S1.2b)
    tool_paths: tuple[str, ...] = ('/opt/homebrew/bin', '/usr/local/bin')
    brew_prefix: Path = Path('/opt/homebrew')
    mac_version: Callable[[], str] = _mac_version
    environ: Mapping[str, str] = field(default_factory=lambda: os.environ)   # read only for DEVELOPER_DIR
    applications_dirs: tuple[Path, ...] = field(default_factory=lambda: (Path('/Applications'),
                                                                          Path.home() / 'Applications'))
    command_timeout_s: float = 10.0


class Check(Protocol):
    id: str
    requires: tuple[str, ...]

    def run(self, ctx: CheckContext) -> CheckResult: ...


def run_checks(checks, ctx: CheckContext) -> tuple[CheckResult, ...]:
    """One result per check, in the given order; see K4 for validation, skipping, isolation and result integrity."""
    return tuple(iter_checks(checks, ctx))


def iter_checks(checks, ctx: CheckContext):
    """run_checks one result at a time (S1.7b, C2): the list is validated before the first result, and each next()
    runs exactly one check, so a caller can run them one per job."""
    checks = list(checks)
    _validate(checks)
    done = {}
    for check in checks:
        blocked_by = next((r for r in check.requires if done[r].status in (Status.BLOCK, Status.SKIP)), None)
        if blocked_by:
            result = CheckResult(check.id, Status.SKIP, 'skipped', f'requires {blocked_by}')
        else:
            result = _run_one(check, ctx)
        done[check.id] = result
        yield result


def ready(results) -> bool:
    return not any(r.status is Status.BLOCK for r in results)


def _validate(checks):
    seen = set()
    for check in checks:
        if check.id in seen:
            raise ValueError(f'two checks share the id {check.id!r}')
        for required in check.requires:
            if required not in seen:
                raise ValueError(f'{check.id!r} requires {required!r}, which is unknown or not earlier in the list')
        seen.add(check.id)


def _run_one(check, ctx):
    try:
        result = check.run(ctx)
    except Exception as e:      # K4 isolation: the checks' one deliberate broad except
        return CheckResult(check.id, Status.BLOCK, 'check_crashed', repr(e))
    if not isinstance(result, CheckResult):
        return CheckResult(check.id, Status.BLOCK, 'check_crashed', f'returned {type(result).__name__}')
    if result.id != check.id:
        return CheckResult(check.id, Status.BLOCK, 'check_crashed', f'returned id {result.id!r}')
    return result
