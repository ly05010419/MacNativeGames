"""Small, game-independent file and path rules for native Mac App packaging."""

import hashlib
from pathlib import Path
import shutil
import subprocess


def _git_worktree_roots(repository: Path) -> tuple[Path, ...]:
    """Include every checkout of this repository when keeping artifacts outside source."""
    if not (repository / ".git").exists():
        return (repository,)
    try:
        process = subprocess.Popen(["git", "-C", str(repository), "worktree", "list", "--porcelain"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            output, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise ValueError("cannot enumerate source worktrees") from None
    except OSError as error:
        raise ValueError("cannot enumerate source worktrees") from error
    if process.returncode:
        raise ValueError("cannot enumerate source worktrees")
    roots = tuple(Path(line[9:]).resolve() for line in output.splitlines()
                  if line.startswith("worktree "))
    if repository not in roots:
        raise ValueError("source worktree missing from Git listing")
    return roots


def outside_repository(path: Path, repository: Path) -> Path:
    """Resolve an output or work path, excluding every source worktree."""
    resolved = path.expanduser().resolve()
    root = repository.resolve()
    if any(resolved == checkout or checkout in resolved.parents
           for checkout in _git_worktree_roots(root)):
        raise ValueError(f"generated files must stay outside the source project: {resolved}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def verified_file(path: Path, expected: str | None = None) -> str:
    """Require a regular, unlinked input and optionally its expected digest."""
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing regular input: {path}")
    actual = sha256_file(path)
    if expected is not None and actual != expected:
        raise ValueError(f"SHA-256 mismatch: {path}: {actual}")
    return actual


def clone_file(source: Path, target: Path) -> None:
    """Use an APFS clone where possible and a normal copy across volumes."""
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"missing regular input: {source}")
    if target.is_symlink():
        raise ValueError(f"output is a symlink: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(["cp", "-c", str(source), str(target)], capture_output=True).returncode:
        shutil.copy2(source, target)
