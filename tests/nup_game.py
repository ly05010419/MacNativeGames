"""A small real .nup for the tests: packages with folders and files (made-up bytes, no game data)."""
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games/oot/third_party/nup'))

from nup import format as fmt  # noqa: E402

BANANZA = 0x040074A01BF12000
BANANZA_UPDATE = 458752


class Table:
    """The directory, file and name tables of one .nup, children sorted by name as the format asks."""

    def __init__(self):
        self.dirs, self.files, self.pool = [], [], bytearray()

    def name(self, text):
        off = len(self.pool)
        self.pool += text.encode()
        return off, len(text.encode())

    def add_dir(self, name, tree):
        index = len(self.dirs)
        self.dirs.append(dict(name=self.name(name), children=[]))
        for child in sorted(tree):
            value = tree[child]
            if isinstance(value, dict):
                self.dirs[index]['children'].append(('dir', self.add_dir(child, value)))
            else:
                self.files.append(dict(name=self.name(child), data=value))
                self.dirs[index]['children'].append(('file', len(self.files) - 1))
        return index


def nested(files):
    """{'a/b.bin': bytes} as nested dictionaries."""
    tree = {}
    for path, data in files.items():
        *folders, name = path.split('/')
        node = tree
        for folder in folders:
            node = node.setdefault(folder, {})
        node[name] = data
    return tree


def make_game_nup(path, *packages):
    """packages: (title, kind, version, exefs files or None, romfs files or None)."""
    table, records = Table(), []
    for title, kind, version, exefs, romfs in packages:
        root = {}
        if exefs is not None:
            root['exefs'] = nested(exefs)
        if romfs is not None:
            root['romfs'] = nested(romfs)
        index = table.add_dir('', root)
        by_name = {table.pool[d['name'][0]:d['name'][0] + d['name'][1]].decode(): i
                   for kind_, i in table.dirs[index]['children'] if kind_ == 'dir' for d in [table.dirs[i]]}
        records.append(fmt.Package(title_id=title, kind=kind, version=version, root_dir=index,
                                   exefs_dir=by_name.get('exefs', fmt.NO_DIR), romfs_dir=by_name.get('romfs', fmt.NO_DIR),
                                   flags=fmt.PKG_FLAG_STANDALONE_UPDATE if kind == fmt.KIND_UPDATE else 0))
    head_size = fmt._toc_head_s.size + len(records) * fmt.PKG_SIZE + len(table.dirs) * fmt.DIR_SIZE + len(table.files) * fmt.FILE_SIZE
    slots = []
    first = []
    for d in table.dirs:
        first.append(len(slots))
        slots += [i if k == 'dir' else len(table.dirs) + i for k, i in d['children']]
    pool_offset = head_size + len(slots) * 4
    toc_size = pool_offset + len(table.pool)
    blob = (fmt.HEADER_SIZE + toc_size + 15) & ~15
    body, offset = bytearray(), blob
    rows = bytearray()
    for f in table.files:
        data = f['data']
        extent = offset if data else 0
        rows += fmt._file_s.pack(f['name'][0], f['name'][1], len(data), extent, len(data), hashlib.sha256(data).digest(),
                                 fmt.CODEC_RAW, 0, 0)
        body += data + bytes(-len(data) % 16)
        offset += len(data) + (-len(data) % 16)
    dirs = b''.join(fmt._dir_s.pack(d['name'][0], d['name'][1], first[i], len(d['children']), 0) for i, d in enumerate(table.dirs))
    toc = (fmt._toc_head_s.pack(fmt.TOC_MAGIC, len(records), len(table.dirs), len(table.files), pool_offset, len(table.pool), 0)
           + b''.join(fmt.pack_pkg(p) for p in records) + dirs + bytes(rows)
           + b''.join(i.to_bytes(4, 'little') for i in slots) + bytes(table.pool))
    end = blob + len(body)
    head = fmt.pack_super(fmt.MAGIC, fmt.HEADER_SIZE, 1, fmt.HEADER_SIZE, len(toc), hashlib.sha256(toc).digest(), blob, end,
                          len(records), 0, bytes(16))
    path.write_bytes(head + toc + bytes(blob - len(head) - len(toc)) + bytes(body))
    return path
