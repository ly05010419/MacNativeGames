"""Game folders kept on a developer's disk but not in the repository: the .gitignore lines of the form /games/<name>/.

A test that walks games/ passes them over (in_repository), so the suite gives the same result with such a folder on the
disk as in a fresh clone, where it is absent."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL_ONLY = frozenset(re.findall(r'^/games/([\w.-]+)/$', (ROOT / '.gitignore').read_text(), re.M))


def in_repository(path):
    """False for a path under a local-only game folder (path: absolute, or relative to the repository)."""
    parts = Path(path).resolve().relative_to(ROOT).parts if Path(path).is_absolute() else Path(path).parts
    return not (len(parts) > 1 and parts[0] == 'games' and parts[1] in LOCAL_ONLY)
