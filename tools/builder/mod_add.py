"""Add mod (S5.2 M2; S5.4 C1-C2; S5.5 C2-C3): find the mods in what the player picks, clone each into the library and
validate it. The model: no Tk. Moved out of mod_library.py in S5.5 to keep it under 300 lines.

A folder, or an archive extracted by mod_archive into <library>/.unzip (removed in every case), is searched by mod_find.
One candidate becomes one mod named after what was picked; a pack of several becomes one mod each, named "<picked> /
<its folder>", all off. The Add is atomic: every id is reserved first, every candidate cloned and validated, and
library.json saved once at the end; any exception removes every folder this Add made and puts the library's state back
as it was. Links are refused (the validation would not follow them, the composition would)."""
import copy
from pathlib import Path
import shutil

from tools.builder import mod_archive, mod_find, mod_library, mod_tools

UNZIP = '.unzip'


def is_archive(path):
    return Path(path).is_file()


def add(runner, repo, app, version, library, source, name=None, cancel=None):
    """The ids added, in order; mod_find.Refused says why when nothing usable is there."""
    source, unzip = Path(source), library.root / UNZIP
    name = name or (source.stem if source.is_file() else source.name)
    before, made = copy.deepcopy(library.state), []
    try:
        if source.is_file():
            shutil.rmtree(unzip, ignore_errors=True)
            source = mod_archive.extract(source, unzip, cancel)
        tops, title_id = mod_tools.top_folders(repo, app, version), mod_library.TITLE_IDS.get(version, '')
        found = mod_find.find(source, tops, title_id)
        for candidate in found:
            parts = [part for part in (candidate.romfs, candidate.exefs) if part]
            links = [p for part in parts for p in part.rglob('*') if p.is_symlink()]
            if links:
                raise mod_find.Refused('links', links[0].name)
        for candidate in found:
            label = f'{name} / {candidate.name}' if len(found) > 1 and candidate.name else name
            mod_id = mod_library._new_id(library, label)
            library.state['order'].append(mod_id)              # reserved: the next id differs
            made.append(mod_id)
            library.state['names'][mod_id] = label
            library.state['notes'][mod_id] = list(candidate.notes)
            for part, folder in (('romfs', candidate.romfs), ('exefs', candidate.exefs)):
                if folder:
                    mod_library.clone(runner, folder, library.path(mod_id) / part)
        mod_library.revalidate(repo, app, version, library, made)
        mod_library.save(library)
    except BaseException:
        for mod_id in made:
            shutil.rmtree(library.path(mod_id), ignore_errors=True)   # no half copy left in the library
        library.state.clear()
        library.state.update(before)
        raise
    finally:
        shutil.rmtree(unzip, ignore_errors=True)
    return tuple(made)
