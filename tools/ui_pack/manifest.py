"""<pack folder>/manifest.tsv: what this tool wrote, and the only pictures it may rewrite or delete.
Columns: key, status (written | skipped | failed | legacy), reason, F, s_d, screen_height, gate, digest.
A picture is owned when its row says written (or legacy). Rows of an old ai-manifest.tsv (4 or 5 cells: the PR builder; 7 cells: v3's
pack3) are read as legacy rows: owned, with no digest, so the next run rebuilds or deletes them; the run then removes ai-manifest.tsv."""
import os
from pathlib import Path
from typing import NamedTuple

MANIFEST = 'manifest.tsv'
OLD_MANIFEST = 'ai-manifest.tsv'
HEADER = ('key', 'status', 'reason', 'F', 's_d', 'screen_height', 'gate', 'digest')
OWNED = ('written', 'legacy')


class Row(NamedTuple):
    status: str
    reason: str = ''
    F: str = ''
    s_d: str = ''
    screen_height: str = ''
    gate: str = ''
    digest: str = ''


def clean(text):
    """One cell: no tab or line break."""
    return ' '.join(str(text).split())


def read(out):
    """{key: Row} from manifest.tsv, plus the legacy rows of ai-manifest.tsv for keys that manifest.tsv does not have."""
    rows, out = {}, Path(out)
    path = out / MANIFEST
    if path.is_file():
        for line in path.read_text().splitlines()[1:]:
            cells = line.split('\t')
            if len(cells) == len(HEADER):
                rows[cells[0]] = Row(*cells[1:])
    old = out / OLD_MANIFEST
    if old.is_file():
        for line in old.read_text().splitlines()[1:]:
            cells = line.split('\t')
            if len(cells) in (4, 5, 7) and cells[0] not in rows:
                rows[cells[0]] = Row('legacy', 'from ai-manifest.tsv')
    return rows


def write(out, rows):
    path = Path(out) / MANIFEST
    text = '\t'.join(HEADER) + '\n' + ''.join('\t'.join((key, *map(clean, rows[key]))) + '\n' for key in sorted(rows))
    tmp = Path(str(path) + '.tmp')
    tmp.write_text(text)
    os.replace(tmp, path)


def remove_old(out):
    (Path(out) / OLD_MANIFEST).unlink(missing_ok=True)
