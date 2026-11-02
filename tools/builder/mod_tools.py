"""main's own mod tools, used by the Mods tab (S5.2): whether a version's mods are supported, validating a mod against
the built app, and re-checking the load folder after a build. The model: no Tk.

games/totk/scripts' make_mod_manifest.py and mod_folder.py are imported (standard library only) and called in-process,
so the builder validates exactly as main installs.
- supported: by behaviour (O5): the version is in make_mod_manifest.APP_VERSIONS and mod_folder.mods_folder(version)
  is that game's own folder. BotW 1.9.0 turns on by itself when main's tools know it.
- validate: make_mod_manifest.manifest() on the mod's romfs/ against the app's RomFS, with the app's own overlays and
  the merges main()'s --check would do. Not valid when a path is misspelt (wrong case), or when it replaces no file of
  the game: it only adds files (a folder one level off, or another game), or everything it changes the app already
  replaces (that needs a build with this mod). A code part (exefs/) is only marked: each .pchtxt's @nsobid (and an .ips
  named after a build id) compared on its first 32 hex digits with 1.4.3's main key in the repo's own table
  (third_party/d2r/patch_nso.py, under "added in TOTK 1.4.3"), the same comparison mod_exefs makes with a 16-byte id.
- recheck: the load folder's manifest.tsv compared with the one make_mod_manifest would write for the current app.
- S5.5 C1: a mod made for another version. A family is a RomFS name with its Product.<n> replaced by Product.*; when the
  mod holds a family the app has, with other numbers and none of the app's, it is not valid (other_version, the versions
  named); when it holds the app's number too, the other ones are only noted (other_ignored). RULES stamps each stored
  validation, so mods checked by older rules are checked again (controller_mods)."""
from dataclasses import dataclass
from pathlib import Path
import re
import sys

SCRIPTS = 'games/totk/scripts'
RULES = 2                           # S5.5: the validation rules a stored validation was made with
PRODUCT = re.compile(r'\.Product\.(\d{3})(?=\.)')
VERSION_NAMES = {'100': '1.0.0', '110': '1.1.0', '111': '1.1.1', '112': '1.1.2', '120': '1.2.0', '121': '1.2.1',
                 '140': '1.4.0', '141': '1.4.1', '142': '1.4.2', '143': '1.4.3'}
KEY_SOURCE = 'games/totk/third_party/d2r/patch_nso.py'
KEY_COMMENT = 'Signed 32-bit offset tables added in TOTK 1.4.3'


@dataclass(frozen=True)
class Validation:
    valid: bool
    reason: str = ''        # '' or: misspelt, other_version, only_adds, all_taken, no_romfs, no_app_romfs
    replaced: int = 0
    added: int = 0
    taken: tuple = ()       # paths the app replaces itself: they need a build with this mod
    misspelt: tuple = ()
    code: str = 'none'      # none, ok, other_build, unchecked, whole_module
    versions: tuple = ()    # S5.5: the other versions its versioned files are for ('1.1.0', ...)
    other_ignored: bool = False     # S5.5: it also holds files for other versions, which the game ignores
    rules: int = 0          # RULES when it was made; 0 for a validation stored before S5.5


def tools(repo):
    """make_mod_manifest and mod_folder from the repository (imported once)."""
    scripts = str(Path(repo) / SCRIPTS)
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import make_mod_manifest
    import mod_folder
    return make_mod_manifest, mod_folder


def supported(repo, spec, version):
    manifest_tool, folder_tool = tools(repo)
    own = Path.home() / 'Library/Application Support/MacNativeGames' / spec.data_folder / 'mods' / version
    return version in manifest_tool.APP_VERSIONS and Path(folder_tool.mods_folder(version)) == own


def main_key(repo):
    """1.4.3's main build id as the repository keeps it: 32 hex digits, the first 16 bytes of the 20."""
    text = (Path(repo) / KEY_SOURCE).read_text()
    found = re.search(re.escape(KEY_COMMENT) + r'.*?SIGNED_TABLES = \{\s*\'([0-9a-f]{32})\'', text, re.S)
    return found[1] if found else None


def _build_id(raw):
    value = raw.strip().lower()
    if len(value) == 64 and value.endswith('0' * 24):
        value = value[:40]                           # some tools pad the 20-byte id with zeros to 32 bytes
    return value if re.fullmatch(r'[0-9a-f]{32}([0-9a-f]{8})?', value) else None


def code_check(repo, version, exefs):
    """'none' without exefs/, 'other_build' when a patch names another main, 'ok' when every named one is 1.4.3's,
    'unchecked' when none names its build (an .ips called main) or the version has no key here; 'not_for_game' for
    Breath of the Wild (S9.2: its exefs/ is never used)."""
    exefs = Path(exefs)
    if not exefs.is_dir():
        return 'none'
    if version.startswith('botw'):
        return 'not_for_game'                        # S9.2: code mods are merged for Tears of the Kingdom only
    if (exefs / 'main').is_file():
        return 'whole_module'                        # S5.4 R3: mod_exefs refuses a whole replacement main
    key = main_key(repo) if version == 'v143' else None
    ids = []
    for path in sorted(exefs.iterdir()):
        if path.suffix == '.pchtxt':
            line = next((ln for ln in path.read_text(errors='replace').splitlines() if ln.startswith('@nsobid-')), '')
            ids.append(_build_id(line[len('@nsobid-'):]) if line else None)
        elif path.suffix in ('.ips', '.ips32') and path.stem.lower() != 'main':
            ids.append(_build_id(path.stem))
    named = [i for i in ids if i]
    if key is None or not named:
        return 'unchecked'
    return 'ok' if all(i[:32] == key for i in named) else 'other_build'


def top_folders(repo, app, version):
    """The app RomFS's top-level folder names (S5.4 C1: what a flat RomFS mod may hold), from its index."""
    manifest_tool, _ = tools(repo)
    index = _app_romfs(app, version) / 'runtime-index.bin'
    return {name.split('/', 1)[0] for name in manifest_tool.read_index(index)} if index.is_file() else set()


def _app_romfs(app, version):
    return Path(app) / 'Contents/Resources/totk/extracted' / version / 'romfs'


def app_index(app, version):
    """The app's RomFS index (runtime-index.bin); validating needs it."""
    return _app_romfs(app, version) / 'runtime-index.bin'


def _found(repo, app, version, folder):
    """make_mod_manifest's Manifest for folder against the app, built as its main() does for --check."""
    manifest_tool, _ = tools(repo)
    romfs = _app_romfs(app, version)
    index = manifest_tool.read_index(romfs / 'runtime-index.bin')
    replaced = manifest_tool.app_overlays(Path(app), version)
    _, patches = manifest_tool.build_patches(romfs)
    merged = {}
    for name, build_sha in patches.items():
        present = (folder / name).is_file() or (folder / (name + manifest_tool.KEPT)).is_file()
        if name in index and present and manifest_tool.changes_original(name, folder, romfs, index):
            merged[name] = build_sha
    return manifest_tool.manifest(folder, romfs, [], replaced, merged)


def other_versions(mod_romfs, app_names):
    """(the other versions, sorted; whether files for other versions sit beside the app's) for a mod's RomFS (B4)."""
    def families(names):
        found = {}
        for name in names:
            match = PRODUCT.search(name)
            if match:
                found.setdefault(PRODUCT.sub('.Product.*', name, count=1), set()).add(match[1])
        return found
    app = families(app_names)
    mod = families(p.relative_to(mod_romfs).as_posix() for p in Path(mod_romfs).rglob('*') if p.is_file())
    other, ignored = set(), False
    for family, numbers in mod.items():
        if family not in app or not numbers - app[family]:
            continue
        if numbers & app[family]:
            ignored = True
        else:
            other |= numbers - app[family]
    if other:                                   # the range: its other versioned files too (older names differ, such
        for family, numbers in mod.items():     # as a size table without .Nin_NX_NVN)
            if family not in app:
                other |= numbers
    return tuple(VERSION_NAMES.get(n, n) for n in sorted(other)), ignored


def validate(repo, app, version, mod_dir):
    code = code_check(repo, version, Path(mod_dir) / 'exefs')
    romfs = Path(mod_dir) / 'romfs'
    if not romfs.is_dir():
        return Validation(False, 'no_romfs', code=code, rules=RULES)
    index = _app_romfs(app, version) / 'runtime-index.bin'
    if not index.is_file():
        return Validation(False, 'no_app_romfs', code=code, rules=RULES)
    found = _found(repo, app, version, romfs)
    added, replaced = len(found.added), len(found.lines) - len(found.added)
    taken, misspelt = tuple(sorted(found.taken)), tuple(name for name, _ in found.misspelt)
    versions, ignored = other_versions(romfs, tools(repo)[0].read_index(index))
    if misspelt:
        reason = 'misspelt'
    elif versions:
        reason = 'other_version'
    elif replaced == 0:
        reason = 'all_taken' if taken else 'only_adds'
    else:
        reason = ''
    return Validation(not reason, reason, replaced, added, taken, misspelt, code, versions, ignored, RULES)


def recheck(repo, app, version, load):
    """True when the load folder's manifest.tsv is no longer what make_mod_manifest would write for this app."""
    manifest_tool, _ = tools(repo)
    written = Path(load) / 'manifest.tsv'
    if not written.is_file():
        return False
    return manifest_tool.manifest_text(_found(repo, app, version, Path(load))) != written.read_text()
