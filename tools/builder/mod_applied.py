"""Whether the game holds the library's mods: every Apply recorded (S7.3 N2, R2). It runs on the Mods part's job,
which is the library's only writer; controller_mods calls it. The model: no Tk.

- Every Apply, of the tab or of the icons install, goes through apply_and_record:
  - a successful result (nothing and set_aside included) keeps the applied key and clears applied_failed;
  - any other result, or an exception, sets applied_failed. The original exception is re-raised, with a failed save
    attached as its cause.
- The applied key is stat only: the enabled ids and each one's romfs files (path, size, mtime_ns), and whatever else
  the caller passes as extra (more of the library's state the game's folder follows). A copy keeps its source's
  mtime, so a replaced file with the same path, size and mtime is not noticed; that is accepted.
- not_applied (both games) is True when the key differs or the last Apply failed. A library with nothing enabled, no
  load folder and no Apply yet is not flagged."""
import hashlib
import json

from tools.builder import mod_library


def applied_key(library, extra=()):
    items = []
    for mod_id in library.state['enabled']:
        root = library.path(mod_id) / 'romfs'
        files = sorted(p for p in root.rglob('*') if p.is_file()) if root.is_dir() else []
        items.append([mod_id, [[p.relative_to(root).as_posix(), p.stat().st_size, p.stat().st_mtime_ns]
                               for p in files]])
    return hashlib.sha256(json.dumps([items, *extra], sort_keys=True).encode()).hexdigest()


def not_applied(library, extra=()):
    if library.state['applied'] is None and not library.state['enabled'] and not library.load.exists():
        return False                                # nothing ever applied, nothing to apply
    return library.state['applied_failed'] or library.state['applied'] != applied_key(library, extra)


def apply_and_record(repo, spec, app, version, library, data_dir, extra=()):
    """mod_library.apply, recorded (the only caller of it in tools/builder)."""
    try:
        result = mod_library.apply(repo, spec, app, version, library, data_dir)
    except BaseException as error:
        try:
            library.state['applied_failed'] = True
            mod_library.save(library)
        except Exception as save_error:
            raise error from save_error
        raise
    if result.ok:
        library.state['applied'], library.state['applied_failed'] = applied_key(library, extra), False
    else:
        library.state['applied_failed'] = True
    mod_library.save(library)
    return result
