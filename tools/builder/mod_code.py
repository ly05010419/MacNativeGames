"""Code mods through the build (S5.4, C3 with R1-R3). The model: no Tk.

A valid library mod whose code check is ok or unchecked can be included in the next build; build_app.py merges it
(--mod) and decides. --mod is passed only for Tears of the Kingdom and only with an update file (the build's version is
the update's; a patch for another build of main stops it early, in the update's step_code, and the app is not replaced).
At the build's start, inside prepare_request, the included mods' patch files (.pchtxt, .ips, .ips32 only) are copied
into <library>/Builder code mods/exefs: a .pchtxt as <mod id>-<name>.pchtxt (its name carries no meaning, its @nsobid
does), an .ips under its own name, and two IPS patches for main are refused before the build. The composed set (mod ids
and each file's SHA-256) travels with the request, and code-in-app.json records it under the app's path (a player may
have two apps) only on that run's success; a failed or cancelled build leaves the record as it was. A successful TOTK
build with no code (none switched on, or no update file) records an empty set (O2)."""
import hashlib
import json
import os
from pathlib import Path
import shutil

FOLDER, RECORD = 'Builder code mods', 'code-in-app.json'
PATCHES = ('.pchtxt', '.ips', '.ips32')
INCLUDABLE = ('ok', 'unchecked')


def includable(validation):
    return validation.code in INCLUDABLE


def included(library):
    return [m for m in library.state.get('code', []) if m in library.state['order']]


def set_code(library, mod_id, on, validation):
    if on and not includable(validation):
        raise ValueError(f'{mod_id} has no code that a build can merge')
    code = [m for m in library.state.get('code', []) if m != mod_id]
    library.state['code'] = [m for m in library.state['order'] if m in code or (on and m == mod_id)]


def planned(library, game_id, has_update):
    """The mods whose code the next build would include ((), when --mod would not be passed)."""
    return tuple(included(library)) if game_id == 'totk' and has_update else ()


def folder(library):
    return library.root / FOLDER


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _patches(exefs):
    """exefs/'s files, sorted; none when a toggled mod has no exefs/ (any more)."""
    return sorted(exefs.iterdir()) if exefs.is_dir() else []


def compose(library, mod_ids):
    """The patch files of mod_ids into <library>/Builder code mods/exefs; (that folder, the record) or (None, ())."""
    if not mod_ids:
        return None, ()
    target = folder(library)
    shutil.rmtree(target, ignore_errors=True)
    (target / 'exefs').mkdir(parents=True)
    record, ips = [], {}
    for mod_id in mod_ids:
        files = []
        for patch in _patches(library.path(mod_id) / 'exefs'):
            if patch.suffix.lower() not in PATCHES or not patch.is_file():
                continue                            # main.npdm, subsdkN...: left out, as mod_exefs leaves them
            if patch.suffix.lower() == '.pchtxt':
                name = f'{mod_id}-{patch.name}'
            else:
                name = patch.name
                if name.lower() in ips:
                    shutil.rmtree(target, ignore_errors=True)
                    raise ValueError(f'two IPS patches for main ({ips[name.lower()]} and {mod_id}): combine them in a '
                                     'mod manager first')
                ips[name.lower()] = mod_id
            shutil.copy2(patch, target / 'exefs' / name)
            files.append((name, _sha(patch)))
        record.append((mod_id, tuple(files)))
    return target, tuple(record)


def write_record(library_root, record, app):
    """On the build's success only: BuildRun runs it as a save job with the app the build announced. Other apps'
    entries are kept."""
    path = Path(library_root) / RECORD
    path.parent.mkdir(parents=True, exist_ok=True)
    apps = read_records(library_root)
    apps[str(app)] = [{'id': m, 'files': dict(files)} for m, files in record]
    partial = path.with_name(RECORD + '.partial')
    partial.write_text(json.dumps({'format': 1, 'apps': apps}, indent=2) + '\n')
    os.replace(partial, path)


def read_records(library_root):
    """{app path: [{'id', 'files'}]}; {} when there is no readable record."""
    try:
        apps = json.loads((Path(library_root) / RECORD).read_text()).get('apps', {})
    except (OSError, ValueError, AttributeError):
        return {}
    return apps if isinstance(apps, dict) else {}


def pending(library, app):
    """True when the code the app was built with differs from the code toggles now (a build is needed)."""
    built = {m['id']: m['files'] for m in read_records(library.root).get(str(app), [])}
    now = {}
    for mod_id in included(library):
        exefs = library.path(mod_id) / 'exefs'
        now[mod_id] = {(f'{mod_id}-{p.name}' if p.suffix.lower() == '.pchtxt' else p.name): _sha(p)
                       for p in _patches(exefs) if p.is_file() and p.suffix.lower() in PATCHES}
    return built != now
