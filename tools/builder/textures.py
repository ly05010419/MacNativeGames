"""The Textures tab's model (S9.2): one texture pack per version, kept in the mod library as the reserved, hidden mod
mod_library.TEXTURES. The model: no Tk; controller_textures runs it on the Mods part's job (the library's one writer).

- add: a folder or archive, found as any mod (RomFS/ in any case), replaces the pack there (the old copy is removed
  first: a failed Add leaves no pack) and is switched on when valid. Its size tables for other versions are dropped
  before the validation: the game reads only its own version's table, which Apply writes (texture_fit), and a pack made
  for an older version would otherwise be refused as made for another version.
- Apply is the Mods tab's (mod_library.apply fits the tables while the pack is enabled).
- A build of Tears of the Kingdom with an update file carries the pack's HeapDef fitted to the existing app
  (build_heap, in the --mod folder) while the pack is enabled; mod_code records it by the pack HeapDef's SHA-256.
- play_env: TOTK_DRAM_8GB=1 when the app was built with a pack's heaps (its heaps need the 8 GB mode, pack on or off).
- tab_state: what the tab shows; heap_pending when the app's heaps do not match the pack's state (build again)."""
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re

from tools.builder import mod_add, mod_code, mod_library, mod_tools, texture_fit

GAMES = ('totk',)                   # BotW has no HeapDef; its packs come later (E9 S9.4)
MEMORY_CHECKED_GB = 24              # docs/mods.md: the 4K pack was checked on a 24 GB Mac
ENV = {'TOTK_DRAM_8GB': '1'}
HEAP_WORK = '.textures-heap'


@dataclass(frozen=True)
class TexturesState:
    present: bool = False
    name: str = ''
    enabled: bool = False
    valid: bool = False
    reason: str = ''                # the validation's reason when not valid
    files: int = 0
    size: int = 0                   # bytes
    heap_pending: bool = False      # the app's heaps differ from what the pack's state wants: build again
    built_with_heap: bool = False   # the app was built with a pack's heaps (Play uses the 8 GB mode)
    memory_gb: int = 0              # this Mac's memory


def memory_gb():
    try:
        return round(os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE') / (1 << 30))
    except (ValueError, OSError):
        return 0


def pack_heapdef(library):
    path = library.path(mod_library.TEXTURES) / 'romfs' / texture_fit.HEAPDEF
    return path if path.is_file() else None


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wanted_heap(library):
    """The pack HeapDef's SHA-256 a build would carry now, or None (no pack, off, or no HeapDef)."""
    heapdef = pack_heapdef(library) if mod_library.TEXTURES in library.state['enabled'] else None
    return _sha(heapdef) if heapdef else None


def tab_state(library, app):
    if mod_library.TEXTURES not in library.state['order']:
        return TexturesState(memory_gb=memory_gb())
    root = library.path(mod_library.TEXTURES) / 'romfs'
    files = [p for p in root.rglob('*') if p.is_file()] if root.is_dir() else []
    v = mod_library.validation(library, mod_library.TEXTURES)
    built = mod_code.built_heap(library.root, app)
    return TexturesState(True, library.state['names'].get(mod_library.TEXTURES, ''),
                         mod_library.TEXTURES in library.state['enabled'], v.valid, v.reason, len(files),
                         sum(p.stat().st_size for p in files), built != wanted_heap(library), built is not None,
                         memory_gb())


def _pruner(version):
    """Drops a pack's size tables for other versions than version (v143 -> Product.143)."""
    keep = version.lstrip('v')
    def prune(folder):
        resource = Path(folder) / 'romfs/System/Resource'
        for path in sorted(resource.glob('ResourceSizeTable.Product.*')) if resource.is_dir() else ():
            number = mod_tools.PRODUCT.search(path.name)
            if number and number.group(1) != keep:
                path.unlink()
    return prune


def add(runner, repo, app, version, library, source, cancel=None):
    remove(library)
    name = re.sub(r'\s+', ' ', Path(source).stem if Path(source).is_file() else Path(source).name).strip()
    mod_add.add(runner, repo, app, version, library, source, name=name, cancel=cancel, reserved=mod_library.TEXTURES,
                prune=_pruner(version))
    if mod_library.validation(library, mod_library.TEXTURES).valid:
        mod_library.set_enabled(library, mod_library.TEXTURES, True)


def set_on(library, on):
    if mod_library.TEXTURES not in library.state['order']:
        raise ValueError('no texture pack')
    mod_library.set_enabled(library, mod_library.TEXTURES, on)


def remove(library):
    if mod_library.TEXTURES in library.state['order'] or library.path(mod_library.TEXTURES).exists():
        mod_library.remove(library, mod_library.TEXTURES)


def build_heap(repo, library, app, version, cancel=None):
    """S9.2 C4, at the build's start: (the pack's HeapDef fitted to the app, its SHA-256), or None when the pack is off
    or has no HeapDef. RuntimeError when the app is missing or the fit fails: the build does not start without it;
    cancellable.Stopped('cancelled') when the build's cancel stopped the fit (S19.1)."""
    sha = wanted_heap(library)
    if sha is None:
        return None
    if not Path(app).is_dir():
        raise RuntimeError(f'the texture pack\'s heap table is fitted to the existing app, and there is none at {app}: '
                           'build once without the pack')
    work = library.root / HEAP_WORK
    return texture_fit.heap(repo, app, version, pack_heapdef(library), work, cancel), sha


def play_env(library, app):
    """C6: the 8 GB mode when the app was built with a pack's heaps."""
    return dict(ENV) if mod_code.built_heap(library.root, app) else {}
