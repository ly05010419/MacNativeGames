"""The work folder as make can name it (issue #2284; first games/oot/build.py, #2045, and games/ssbu, 2026-10-24).

make splits every target and prerequisite name at spaces, and MNG Launcher's default work folder sits next to the game,
named after it ("/Volumes/Games/Super Mario Odyssey/build/odyssey"). A folder with a space is handed over as a link
without one in the temporary folder; the build's files still land in the work folder. Every port's `make ... WORK=`
goes through this one copy (tests/test_make_path.py); standard library only, so any build script can import it."""
import hashlib
import tempfile
from pathlib import Path


def make_path(work, game):
    """work itself when its path has no space, else <temporary folder>/<game>-work-<12 hex of its real path>, a link
    to it. ValueError when the temporary folder has a space too, or something else already has the link's name."""
    work = Path(work)
    if ' ' not in str(work):
        return work
    real = work.resolve()
    alias = Path(tempfile.gettempdir()) / f'{game}-work-{hashlib.sha256(str(real).encode()).hexdigest()[:12]}'
    if ' ' in str(alias):
        raise ValueError(f'the work folder {work} has a space and the temporary folder {alias.parent} too: '
                         'set TMPDIR to a folder without one')
    for _ in range(2):
        if alias.is_symlink():
            if alias.resolve() == real:
                return alias
            alias.unlink()   # a link of ours, left pointing at a folder that has since moved
        elif alias.exists():
            raise ValueError(f'{alias} is in the way of the work folder\'s link: remove it, or set TMPDIR elsewhere')
        try:
            alias.symlink_to(real, target_is_directory=True)
            return alias
        except FileExistsError:   # another build of the same folder made it meanwhile: check what is there now
            continue
    raise ValueError(f'could not make {alias}, a link to the work folder {real}')
