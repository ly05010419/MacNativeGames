"""A texture pack's tables fitted to the app (S9.2 Textures, C3-C4). The model: no Tk.

A pack made for an older version ships that version's heap table (HeapDef) and resource size tables; main's
games/totk/scripts/texture_pack_tables.py writes the app's own version of both, with the heaps the pack raises and the
entries of the files it grows (docs/mods.md, "Large texture packs"). fit() runs it in place on a composed RomFS folder,
at Apply; heap() runs it on a folder holding only the pack's HeapDef, at a build's start, for the --mod folder: the
app's own 2K overlays replace a mod's HeapDef, so the pack's heaps reach the game only through the build. Both read the
existing app's RomFS. Only Tears of the Kingdom has a HeapDef (Breath of the Wild's heaps are in its code).

S19.1: the script runs through cancellable.run, so a cancel (the build's, or the Mods part's at Apply) kills its process
group and waits for it; only that cancel leaves fit, as cancellable.Stopped('cancelled'). A timeout, a script that
cannot start and a non-zero exit are failures (the tuple)."""
from pathlib import Path
import shutil
import sys

from tools.builder import cancellable

SCRIPT = 'games/totk/scripts/texture_pack_tables.py'
HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
FIT_S = 1800.0
LINES = 4                           # the script's last lines a failure keeps


def fit(repo, app, version, romfs, cancel=None):
    """texture_pack_tables.py on romfs, in place: None when it worked, else its last lines (a tuple)."""
    argv = [sys.executable, str(Path(repo) / SCRIPT), str(romfs), '--app', str(app), '--version', version]
    try:
        code, stdout, stderr = cancellable.run(argv, cancel, FIT_S)
    except cancellable.Stopped as e:
        if e.reason == 'cancelled':
            raise
        return (f'{Path(SCRIPT).name}: {e}',)
    if code == 0:
        return None
    out = (stdout.decode(errors='replace') + stderr).strip().splitlines()
    return tuple(out[-LINES:]) or (f'{Path(SCRIPT).name} exited with {code}',)


def heap(repo, app, version, pack_heapdef, work, cancel=None):
    """The pack's HeapDef fitted to the app, written under work (removed first): its path. RuntimeError with the
    script's last lines when it fails."""
    work = Path(work)
    shutil.rmtree(work, ignore_errors=True)
    target = work / 'romfs' / HEAPDEF
    target.parent.mkdir(parents=True)
    shutil.copyfile(pack_heapdef, target)
    failed = fit(repo, app, version, work / 'romfs', cancel)
    if failed is not None:
        raise RuntimeError('the texture pack\'s heap table could not be fitted: ' + ' / '.join(failed))
    return target
