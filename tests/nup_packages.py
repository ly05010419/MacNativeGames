"""Made-up .nup packages for the tests: a valid package table over one empty folder, no game data."""
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games/oot/third_party/nup'))

from nup import format as fmt  # noqa: E402

OOT = 0x0400486024DEC000
POKOPIA = 0x0400A3A01EE4C000
KART_WORLD = 0x0400C3F00006E000


def make_nup(path, *packages):
    """A valid NUP of (title, kind, version) packages."""
    records = b''.join(fmt.pack_pkg(fmt.Package(title_id=title, kind=kind, version=version,
                                                flags=fmt.PKG_FLAG_STANDALONE_UPDATE if kind == fmt.KIND_UPDATE else 0,
                                                root_dir=0, exefs_dir=fmt.NO_DIR, romfs_dir=fmt.NO_DIR))
                       for title, kind, version in packages)
    root = fmt._dir_s.pack(0, 0, 0, 0, 0)
    pool = fmt._toc_head_s.size + len(records) + len(root)
    toc = fmt._toc_head_s.pack(fmt.TOC_MAGIC, len(packages), 1, 0, pool, 0, 0) + records + root
    blob = (fmt.HEADER_SIZE + len(toc) + 15) & ~15
    head = fmt.pack_super(fmt.MAGIC, fmt.HEADER_SIZE, 1, fmt.HEADER_SIZE, len(toc), hashlib.sha256(toc).digest(),
                          blob, blob, len(packages), 0, bytes(16))
    path.write_bytes(head + toc + bytes(blob - len(head) - len(toc)))
    return path
