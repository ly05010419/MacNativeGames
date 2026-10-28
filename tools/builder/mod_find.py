"""Find the mods in what the player picks (S5.4 C1, C4; S5.5 C3, C4). The model: no Tk.

Mods come in several layouts; the picked path may be the folder with romfs/ and/or exefs/, the romfs/ folder itself, a
folder with such pairs up to MAX_DEPTH levels below it (one folder too many, an emulator's contents/<title id>/,
atmosphere/contents/<title id>/), or a flat RomFS (every top-level folder is a top-level folder of the app's RomFS;
loose files such as a readme are ignored). Archives are mod_archive's. find() returns every candidate: one, or a pack of
up to MAX_PACK (S5.5 C3: each becomes its own mod); more is refused as several. A candidate under another game's title
id is set aside (refused only when nothing else is left), the __MACOSX/ folder Finder's Compress adds is not searched,
and a folder too big to search, only cheat files, a TKMM package (.tkcl), a BCML package (.bnp, or an extracted one:
info.json and logs/; S9.2) or nothing usable is refused with a reason id the view turns into words."""
from dataclasses import dataclass
from pathlib import Path
import re

MAX_DEPTH, MAX_WALK, MAX_PACK = 4, 20000, 12
TITLE = re.compile(r'^0100[0-9A-Fa-f]{12}$')
SKIPPED = '__MACOSX'                # resource forks a Mac's Compress adds next to the real files


class Refused(ValueError):
    def __init__(self, reason, detail=''):
        super().__init__(f'{reason}: {detail}')
        self.reason, self.detail = reason, detail


@dataclass(frozen=True)
class Found:
    romfs: Path | None              # the folder whose content goes into the library's romfs/
    exefs: Path | None
    notes: tuple = ()               # 'cheats_ignored' when cheats/ came with it
    name: str = ''                  # S5.5 C3: its folder below the picked one ('' when it is the picked one)


def _pair(folder):
    romfs, exefs = folder / 'romfs', folder / 'exefs'
    return (romfs if romfs.is_dir() else None), (exefs if exefs.is_dir() else None)


def _notes(folder):
    return ('cheats_ignored',) if (folder / 'cheats').is_dir() else ()


def _other_title(path, base, title_id):
    """The other game's title id in path below base, or None."""
    return next((part for part in Path(path).relative_to(base).parts
                 if TITLE.match(part) and part.upper() != title_id.upper()), None)


def find(source, top_folders, title_id):
    """The mods in a picked folder: a list of Found (C1, C3). top_folders: the app RomFS's top-level folder names."""
    source = Path(source)
    if source.name == 'romfs' and source.is_dir():
        source = source.parent
    if TITLE.match(source.name) and source.name.upper() != title_id.upper():
        raise Refused('other_game', source.name)
    if (source / 'info.json').is_file() and (source / 'logs').is_dir():
        raise Refused('bcml_package', source.name)                  # S9.2: an extracted BCML package
    romfs, exefs = _pair(source)
    if romfs or exefs:
        return [Found(romfs, exefs, _notes(source))]
    roots, cheats, packages, walked, frontier = [], False, [], 0, [(source, 0)]
    while frontier:
        folder, depth = frontier.pop(0)
        for child in sorted(folder.iterdir()):
            if child.is_symlink():
                continue
            if child.suffix.lower() in ('.tkcl', '.bnp') and child.is_file():
                packages.append(child.name)
            if not child.is_dir() or child.name == SKIPPED:
                continue
            walked += 1
            if walked > MAX_WALK:
                raise Refused('too_big', str(source))
            if child.name in ('romfs', 'exefs'):
                if folder not in roots:
                    roots.append(folder)
            elif child.name == 'cheats':
                cheats = True
            elif depth + 1 < MAX_DEPTH:
                frontier.append((child, depth + 1))
    others = [other for other in (_other_title(r, source, title_id) for r in roots) if other]
    roots = [r for r in roots if not _other_title(r, source, title_id)]
    if others and not roots:
        raise Refused('other_game', others[0])
    if len(roots) > MAX_PACK:
        raise Refused('several', ', '.join(str(r.relative_to(source)) for r in roots[:4]))
    if roots:
        return [Found(*_pair(r), _notes(r), '' if r == source else r.relative_to(source).as_posix()) for r in roots]
    tops = [p.name for p in source.iterdir() if p.is_dir() and p.name != SKIPPED]
    if tops and all(name in top_folders for name in tops):
        return [Found(source, None)]
    if packages:
        raise Refused('bcml_package' if packages[0].lower().endswith('.bnp') else 'tkmm_package', packages[0])
    raise Refused('cheats_only' if cheats else 'nothing', str(source))
