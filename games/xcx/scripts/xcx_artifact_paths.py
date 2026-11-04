"""Keep generated data outside every source worktree, including symlinked output paths."""
from pathlib import Path
import subprocess
import sys

REPOSITORY = Path(__file__).resolve().parents[3]


def outside_source(path):
    path = Path(path).expanduser().resolve()
    roots = [REPOSITORY]
    if (REPOSITORY / ".git").exists():
        result = subprocess.run(["git", "-C", str(REPOSITORY), "worktree", "list", "--porcelain"],
                                capture_output=True, text=True, check=True, timeout=10)
        roots = [Path(line[9:]).resolve() for line in result.stdout.splitlines() if line.startswith("worktree ")]
    if any(path == root or root in path.parents for root in roots):
        raise ValueError(f"Generated output must stay outside all source worktrees: {path}")
    return path


if __name__ == "__main__":
    try:
        for arg in sys.argv[1:]:
            outside_source(arg)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
