"""The files the player's enabled mods share, and which of them are conflicts (S5.2's conflicts line). The model: no Tk.

A path two or more enabled mods replace is a conflict (the one lower in the list wins) unless every one of them holds
the same bytes there: then nothing is in dispute and the tab says nothing. Whether the bytes are the same is found
only by the full read (it reads the shared files of equal size), and recorded in the library's state as each owner's
(id, size, mtime_ns). A quick read stays stat only (R1): a recorded path is not a conflict while every owner's file
still has the recorded size and mtime; a path shared since the last full read, or changed, is shown as a conflict."""
import hashlib


def owners(library, ids):
    """{path: [mod ids]} of the romfs paths that more than one of ids replaces."""
    found = {}
    for mod_id in ids:
        root = library.path(mod_id) / 'romfs'
        for file in sorted(p for p in root.rglob('*') if p.is_file()):
            found.setdefault(file.relative_to(root).as_posix(), []).append(mod_id)
    return {path: found_ids for path, found_ids in found.items() if len(found_ids) > 1}


def _signature(library, ids, path):
    stats = [(mod_id, (library.path(mod_id) / 'romfs' / path).stat()) for mod_id in ids]
    return [[mod_id, s.st_size, s.st_mtime_ns] for mod_id, s in stats]


def record_same(library, shared):
    """The full read's part: library.state['same'] becomes the shared paths whose files are the same bytes in every
    owner; True when that changed."""
    same = {}
    for path, ids in shared.items():
        files = [library.path(mod_id) / 'romfs' / path for mod_id in ids]
        if (len({f.stat().st_size for f in files}) == 1
                and len({hashlib.sha256(f.read_bytes()).digest() for f in files}) == 1):
            same[path] = _signature(library, ids, path)
    changed = same != library.state['same']
    library.state['same'] = same
    return changed


def conflicts(library, shared):
    """shared less the paths recorded as the same whose files have not changed since (stat only)."""
    return {path: ids for path, ids in shared.items()
            if library.state['same'].get(path) != _signature(library, ids, path)}
