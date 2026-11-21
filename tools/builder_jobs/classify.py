"""What each dropped file or folder is, and the build it makes (pure apart from reading the files' headers).

Contract: classify(paths) never raises for a bad file: every path becomes an Item, and one that is not a game file is
'ignored' with the reason. A folder is looked into (two levels) unless it is itself a Switch 2 dump, a firmware folder
or a shared-fonts folder. The game comes from the files, never from their names (as nxnative, user 2026-11-21: a
Pokémon card was taken for Zelda): a package's content metas read with the keys (identify.contents: title ID, kind and
version), else its ticket (the title ID of its rights ID; no version), a Switch 2 dump's main.npdm or a .nup package
(Ocarina of Time). A package told by neither waits for the keys. plan(items) picks one game and its files, and lists what
still blocks a build: files of more than one game, a game this repository does not build among them included."""
from dataclasses import asdict, dataclass, field
from pathlib import Path
import re
import subprocess

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

GAME_SUFFIXES = {'.xci': 'xci', '.xcz': 'xci', '.nsp': 'nsp', '.nsz': 'nsp'}
_VERSION_IN_NAME = re.compile(r'(?<![0-9A-Za-z])v(\d{1,10})(?![0-9]|\.\d)', re.IGNORECASE)   # [v720896], (v131072), v720896
_DISPLAY_IN_NAME = re.compile(r'(?<![0-9.])(\d+\.\d+\.\d+)(?![0-9]|\.\d)')                      # 1.2.1
_HEADER_KEY = re.compile(r'^\s*header_key\s*=\s*[0-9A-Fa-f]{64}\s*$', re.MULTILINE)


@dataclass(frozen=True)
class Item:
    path: str
    role: str                 # keys | base | update | dlc | ns2_dump | nup | firmware | fonts | ignored
    game: str | None = None
    title: str | None = None
    version: int | None = None   # the content meta's, to pick the newest of several updates (None: no keys yet)
    note: str = ''
    problem: str = ''         # the keys cannot read the file, or are too old for it (a block of the plan, at the keys)
    inside: bool = False      # found in a dropped folder, not dropped itself: another game's file there is left out
    holds_update: int | None = None   # a base game file that also holds an update (a combined card): that update's version


@dataclass
class Plan:
    game: str | None = None
    app_name: str | None = None
    base: str | None = None
    update: str | None = None
    update_version: int | None = None   # the chosen update's title version, to tell an installed app of the same version
    dlc: list = field(default_factory=list)
    keys: str | None = None
    firmware: str | None = None
    fonts: str | None = None
    games_found: list = field(default_factory=list)
    needs_keys: bool = True    # a Switch 2 dump and a .nup need none
    keys_problems: list = field(default_factory=list)   # the blocks about the keys (the window shows them at the keys)
    blocks: list = field(default_factory=list)
    tool_blocks: list = field(default_factory=list)   # the blocks about this Mac's tools, not the files (also in blocks)
    tool_needs: list = field(default_factory=list)    # what each of them is: 'metal' (Xcode's) or 'clt' (the window's fix)
    notes: list = field(default_factory=list)
    font_block: str | None = None   # the block about missing system fonts: the Launcher offers 用开源字体 under it
    open_fonts: bool = False        # the build makes open-source Noto fonts (build_job.py) in place of firmware ones


def _hidden(path):
    return path.name.startswith('._') or path.name == '.DS_Store'


def _keys(path):
    try:
        if path.stat().st_size > 1 << 20:
            return Item(str(path), 'ignored', note=t('too big for a keys file', '太大，不是 keys 文件'))
        text = path.read_text(errors='replace')
    except OSError as e:
        return Item(str(path), 'ignored', note=t(f'cannot be read: {e.strerror}', f'读不了：{e.strerror}'))
    if not _HEADER_KEY.search(text):
        return Item(str(path), 'ignored', note=t('a keys file without header_key: not a complete keys file', '没有 header_key 的 keys 文件：不是完整的 keys 文件'))
    return Item(str(path), 'keys')


# The display versions of the ports with their own build.py whose versions build_app.py's VERSIONS does not list:
# Animal Crossing's names its updates [3.0.3] (games/acnh/assets/expected-v*.json); without them several updates were
# told apart by date only, and its add-ons were refused next to a 3.0.3 update (a player's report, 2026-11-20)
_DISPLAY_VERSIONS = {'acnh': {'1.2.1': 458752, '3.0.1': 2097152, '3.0.3': 2228224}}


def _version(name, game=None):
    """The title version a package's name gives: [vNNN], or a display version (1.2.1) of a version build_app.py knows.
    Only compared with the content meta's version (a name that says otherwise is noted, the contents win)."""
    found = _VERSION_IN_NAME.search(name)
    if found:
        return int(found.group(1))
    found = _DISPLAY_IN_NAME.search(name)
    if found and found.group(1) in _DISPLAY_VERSIONS.get(game, {}):
        return _DISPLAY_VERSIONS[game][found.group(1)]
    if found:
        for v in repo.build_app().VERSIONS.values():
            if v.get('display_version') == found.group(1) and str(v.get('version', '')).isdigit():
                return int(v['version'])
    return None


_KIND_NAMES = {'base': ('base game', '本体'), 'update': ('update', '更新'), 'dlc': ('add-on', '追加内容')}


def _package(path, keys=None, inside=False):
    """A package by what it holds: its content metas with the keys, else its ticket; its name is never read for its
    game (a [vNNN] that says otherwise than its content meta is noted)."""
    b = repo.build_app()
    try:
        if path.stat().st_size == 0:
            return Item(str(path), 'ignored', note=t('empty file', '空文件'), inside=inside)
        kind = GAME_SUFFIXES[path.suffix.lower()]
        tickets = b.ticket_title_ids(path) if kind == 'nsp' else []
    except OSError as e:
        return Item(str(path), 'ignored', note=t(f'cannot be read: {e.strerror}', f'读不了：{e.strerror}'), inside=inside)
    metas, problem = (None, None)
    if keys:
        from tools.builder_jobs.identify import contents
        metas, problem = contents(path, keys)
    if metas is None:   # no keys yet (or no cryptography, or keys that cannot open it): the tickets, without versions
        found = [(b.title_kind(title)[1], title, None) for title in tickets]
    else:
        found = [(m.kind, m.title, m.version) for m in metas]
        if not found:
            return Item(str(path), 'ignored', note=t('holds no base game, update or add-on', '里面没有本体、更新或追加内容'),
                        problem=problem or '', inside=inside)
    if not found:
        if problem:
            note = problem
        elif keys:   # keys given and read, but no cryptography package here: build_app.py tells the game at its start
            note = t('its game is told at the start of the build', '打包开始时认出是哪个游戏')
        else:
            note = t('its game is read with your keys', '选好 keys 后认出是哪个游戏')
        return Item(str(path), 'base', note=note, problem=problem or '', inside=inside)
    # a file of several titles (a card with its update and add-ons) is taken for its base game, else its update
    role, title, version = min(found, key=lambda f: ('base', 'update', 'dlc').index(f[0]))
    game = repo.game_of_title(title)
    notes = []
    if len(found) > 1:
        kinds = sorted({k for k, _, _ in found}, key=('base', 'update', 'dlc').index)
        notes.append(t('holds ' + ' + '.join(_KIND_NAMES[k][0] for k in kinds), '里面有' + '、'.join(_KIND_NAMES[k][1] for k in kinds)))
    named = _version(path.name, game)   # a combined card's name gives the version of its update: any of its titles
    if version is not None and named is not None and named not in {v for _, _, v in found}:
        notes.append(t(f'its name says v{named}, its contents v{version}: taken as v{version}',
                       f'文件名写 v{named}，内容是 v{version}：按内容'))
    application = b.title_kind(title)[0]
    if game is None or application in b.NS2:
        if application in b.NS2:   # Pokopia, Bananza: from a Switch 2 dump folder only
            why = t(f'{repo.GAMES.get(b.NS2[application]["version"], application)} is built from its Switch 2 dump folder '
                    '(base/, updates/), not from an XCI or NSP',
                    f'{repo.GAMES.get(b.NS2[application]["version"], application)}要用 Switch 2 游戏文件夹（base/、updates/）打包，不能用 XCI 或 NSP')
        else:
            why = t(f'{title} ({_KIND_NAMES[role][0]}) is not a game this repository builds',
                    f'{title}（{_KIND_NAMES[role][1]}）不是本项目能打包的游戏')
        return Item(str(path), 'ignored', title=title, note='; '.join([why, *notes]), inside=inside)
    updates = [v for k, _, v in found if k == 'update' and v is not None]
    return Item(str(path), role, game=game, title=title, version=version, note='; '.join(notes),
                problem=problem or '', inside=inside, holds_update=max(updates) if role == 'base' and updates else None)


def _folder(path):
    """A Switch 2 dump, a firmware or a shared-fonts folder, or None for a folder to look into."""
    b = repo.build_app()
    if (path/'ExeFS/main.npdm').is_file() and (path/'RomFS').is_dir():
        title = b.npdm_program_id(path/'ExeFS/main.npdm')
        if title == '01002DA013484000':
            return Item(str(path), 'ss_dump', game='ss', title=title, version=65536)
    if (path / 'base').is_dir() or (path / 'updates').is_dir():
        npdm = b.ns2_dump_exefs(path) / 'main.npdm'
        title = b.npdm_program_id(npdm) if npdm.is_file() else None
        game = repo.game_of_title(title) if title else None
        if game:
            return Item(str(path), 'ns2_dump', game=game, title=title)
        return Item(str(path), 'ignored', title=title, note=t('a dump folder of a game this repository does not build', '本项目不能打包这个游戏的文件夹')
                    if title else t('a dump folder without base/exefs/main.npdm', '游戏文件夹里没有 base/exefs/main.npdm'))
    names = [p.name.lower() for p in path.iterdir() if not _hidden(p)]
    if any(n.endswith('.bfttf') for n in names):
        return Item(str(path), 'fonts')
    if sum(n.endswith('.nca') for n in names) >= 20:   # an unpacked firmware has hundreds of NCAs
        return Item(str(path), 'firmware')
    return None


def _nup(path, inside):
    from tools import nup_input

    b = repo.build_app()
    try:
        title, version = nup_input.inspect(path, b.NUP_VERSIONS)
    except nup_input.InputError as error:
        if error.title and error.title not in b.NUP_VERSIONS:
            note = t(f'This Launcher cannot build NUP for Title ID {error.title}.',
                     f'此版本不支持打包这个游戏的 NUP（游戏编号 {error.title}）。')
        else:
            note = t(f'Cannot use this NUP: {error}', f'无法使用这个 NUP：{error}')
        return Item(str(path), 'ignored', title=error.title, note=note, inside=inside)
    except nup_input.READ_ERRORS as error:
        note = t(f'Cannot read this NUP: {error}', f'无法读取这个 NUP：{error}')
        return Item(str(path), 'ignored', note=note, inside=inside)
    return Item(str(path), 'nup', game=repo.game_of_title(title), title=title, version=version, inside=inside)


def _one(path, depth, out):
    if _hidden(path):
        return
    if path.is_dir():
        try:
            found = _folder(path)
            if found:
                out.append(found)
                return
            if depth >= 2:
                return
            for child in sorted(path.iterdir()):
                _one(child, depth + 1, out)
        except OSError as e:
            out.append(Item(str(path), 'ignored', note=t(f'cannot be read: {e.strerror}', f'读不了：{e.strerror}')))
        return
    suffix = path.suffix.lower()
    if path.name.lower() == 'title.keys':
        out.append(Item(str(path), 'ignored', note=t('title keys are not needed: the game files carry their tickets', '不需要 title.keys：游戏文件自带票据')))
    elif suffix == '.keys' or path.name.lower() in ('prod.keys', 'keys.txt'):
        out.append(_keys(path))
    elif suffix in GAME_SUFFIXES:
        out.append(('package', path, depth > 0))   # read once the keys among the paths are known (classify)
    elif suffix == '.nup':
        out.append(_nup(path, depth > 0))
    elif depth == 0:
        out.append(Item(str(path), 'ignored', note=t('not a game file, keys file or game folder', '不是游戏文件、keys 或游戏文件夹')))


def classify(paths):
    out = []
    for p in paths:
        path = Path(p).expanduser()
        if not path.exists():
            out.append(Item(str(path), 'ignored', note=t('no such file', '文件不存在')))
            continue
        _one(path.resolve(), 0, out)
    keys = [i.path for i in out if isinstance(i, Item) and i.role == 'keys']
    out = [_package(i[1], keys[-1] if keys else None, i[2]) if isinstance(i, tuple) else i for i in out]
    once = {}   # the same file dropped twice counts once, as dropped itself when it was also in a dropped folder
    for i in out:
        if i.path not in once or (once[i.path].inside and not i.inside):
            once[i.path] = i
    return list(once.values())


def _newest(items):
    return max(items, key=lambda i: (i.version or 0, Path(i.path).suffix.lower() == '.nsp', Path(i.path).stat().st_mtime))


def _supported(game):
    """The title versions of game build_app.py has verified values for (its VERSIONS), the base's 0 included."""
    if game == 'ss':
        return {65536}
    if game == 'dread':
        return {0, 327680}
    if game in ('worldend', 'tmnt'):
        return {0}
    if game == 'odyssey':   # 1.3.0 (262144) or 1.4.1, checked by hash in the build (scripts/inspect_version.py), not by number
        return None
    if game == 'eow':   # games/eow/assets/expected-v1.1.0.json: base 0 and update 1.1.0
        return {0, 196608}
    if game == 'acnh':   # games/acnh/prepare.py SUPPORTED_UPDATES; 3.0.3 is the newest, picked first
        return {0, 458752, 2097152, 2228224}
    return {int(v['version']) for v in repo.build_app().VERSIONS.values()
            if str(v.get('version', '')).isdigit() and repo.game_of_title(str(v.get('title_id', '0' * 16))) == game}


def no_metal_compiler():
    """True when no Xcode on this Mac has its Metal toolchain (build_app.metal_compiler, which the builds use)."""
    return repo.build_app().metal_compiler() is None


def oot_needs_metal():
    """Ocarina of Time compiles its 40,000-odd shaders into libraries with Xcode's Metal compiler (games/oot/scripts/
    build_shader_libs.py stops without it, after the translation), where the other games need only the command line
    tools: said before its build or update starts (user 2026-11-17: "修")."""
    return t('Ocarina of Time compiles its shaders with Xcode\'s Metal compiler: install the full Xcode and its Metal toolchain '
             '(Xcode › Settings › Components › Metal Toolchain, or xcodebuild -downloadComponent MetalToolchain)',
             '时之笛要用 Xcode 的 Metal 编译器编着色器：请安装完整的 Xcode 和它的 Metal 工具链'
             '（Xcode › 设置 › 组件 › Metal Toolchain，或 xcodebuild -downloadComponent MetalToolchain）')


def _hfs0_names(f, offset, length):
    """(name, absolute offset) of each entry of the HFS0 table at offset (the layout extract_exefs.partition_entries
    reads, bounded the same way), or [] when there is no table there."""
    import struct
    f.seek(offset)
    hdr = f.read(16)
    if len(hdr) < 16 or hdr[:4] != b'HFS0':
        return []
    count, strings_size = struct.unpack_from('<II', hdr, 4)
    if count > 4096 or strings_size > 1024 * 1024:
        return []
    size = 16 + count * 64 + strings_size
    if offset + size > length:
        return []
    f.seek(offset)
    raw = f.read(size)
    strings = raw[16 + count * 64:]
    out = []
    for i in range(count):
        off, _, nameoff = struct.unpack_from('<QQI', raw, 16 + i * 64)
        end = strings.find(b'\0', nameoff)
        if nameoff >= len(strings) or end < 0:
            return []
        out.append((strings[nameoff:end].decode('utf-8', 'replace'), offset + size + off))
    return out


def card_titles(path):
    """How many titles (content metas) a game card image's secure partition holds, read from its HFS0 tables without
    keys: more than one for a combined card ("1G+1U+3D": the base game, its update and add-ons), None when the file does
    not read as a card image."""
    import struct
    try:
        with open(path, 'rb') as f:
            length = f.seek(0, 2)
            f.seek(0x100)
            if f.read(4) != b'HEAD':
                return None
            f.seek(0x130)
            root = struct.unpack('<Q', f.read(8))[0]
            secure = [o for n, o in _hfs0_names(f, root, length) if n == 'secure']
            if not secure:
                return None
            return sum(1 for n, _ in _hfs0_names(f, secure[0], length) if n.endswith('.cnmt.nca'))
    except (OSError, struct.error, ValueError):
        return None


def card_update_ncas(path):
    """How many NCAs (besides metadata) a game card image's system update partition holds, read from its HFS0 tables,
    which need no keys: 0 for a merged or trimmed card ("1G+1U"), whose missing or empty update partition has no system
    fonts (#2242), None when the file does not read as a card image (an .xcz, a damaged file: left to the build)."""
    import struct
    try:
        with open(path, 'rb') as f:
            length = f.seek(0, 2)
            f.seek(0x100)
            if f.read(4) != b'HEAD':
                return None
            f.seek(0x130)
            root = struct.unpack('<Q', f.read(8))[0]
            update = [o for n, o in _hfs0_names(f, root, length) if n == 'update']
            if not _hfs0_names(f, root, length):
                return None
            if not update:
                return 0
            return sum(1 for n, _ in _hfs0_names(f, update[0], length) if n.endswith('.nca') and not n.endswith('.cnmt.nca'))
    except (OSError, struct.error, ValueError):
        return None


# The games whose Mac build tools/prebuilt.py covers (build_app.py's runtimes of games/totk/Makefile), so a release's
# Launcher builds them with no developer tools; Ocarina of Time needs Xcode (oot_needs_metal), which brings them. The
# others (ss, dread, worldend, tmnt, acnh: their own games/<game>/build.py) compile on this Mac.
PREBUILT_GAMES = {'totk', 'botw', 'dkb', 'pkp', 'oot'}


def no_developer_tools():
    """True when the folder xcode-select names has no clang. Read from the files, never by running xcrun, git or make,
    which on a Mac without the tools open Apple's install dialog (/usr/bin/xcode-select is a program of its own)."""
    try:
        folder = subprocess.run(['/usr/bin/xcode-select', '-p'], capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return True
    # Command Line Tools keep clang in usr/bin; a full Xcode (xcode-select -s /Applications/Xcode.app) keeps it in its
    # default toolchain, and its Developer/usr/bin has none (issue #2284)
    return not folder or not any((Path(folder) / sub).is_file()
                                 for sub in ('usr/bin/clang', 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'))


def needs_developer_tools(game):
    """A game a release's prebuilt programs do not cover compiles on this Mac (user 2026-11-17: page 1 says the developer
    tools are not needed, true for the games tools/prebuilt.py covers only)."""
    name = repo.GAMES.get(game, game)
    return t(f'{name} is compiled on this Mac: install Apple\'s command line tools (xcode-select --install)',
             f'{name}要在这台 Mac 上编译：请安装 Apple 的命令行工具（xcode-select --install）')


def plan(items, work=None, open_fonts=False):
    """The build these items make. work: the work folder the build will use (fonts extracted there count). open_fonts:
    the player chose 用开源字体, so a game file without system fonts builds with the open-source Noto fonts."""
    p = _plan(items, work, open_fonts)
    if p.game and p.game not in PREBUILT_GAMES and no_developer_tools():
        p.blocks.append(needs_developer_tools(p.game))
        p.tool_blocks.append(p.blocks[-1])
        p.tool_needs.append('clt')
    return p


def _plan(items, work=None, open_fonts=False):
    p = Plan()
    by_role = lambda role, game=None: [i for i in items if i.role == role and (game is None or i.game in (game, None))]
    keys = by_role('keys')
    p.keys = keys[-1].path if keys else None
    bases = [i for i in items if i.role in ('base', 'ns2_dump', 'nup', 'ss_dump')]
    p.games_found = sorted({i.game for i in items if i.game})
    # Every file told by its contents counts, of a game this repository builds or not (user 2026-11-21: a Pokémon card
    # dropped beside Zelda files kept from before was built as Zelda): files of two games stop the plan, each named.
    # Another game's file inside a dropped folder is left out (Animal Crossing's folder holds its Island Transfer Tool).
    told = [i for i in items if i.title and (i.game or not i.inside) and i.role != 'keys']
    b = repo.build_app()
    owners = {}
    for i in told:
        owners.setdefault(b.title_kind(i.title)[0], []).append(i)
    if len(owners) > 1:
        def what(app, files):
            game = next((i.game for i in files if i.game), None)
            return repo.GAMES.get(game, game) if game else t(f'a game this repository does not build ({app})', f'本项目不能打包的游戏（{app}）')
        lines = [f'{Path(i.path).name}: {what(app, files)}' for app, files in owners.items() for i in files]
        p.blocks.append(t('files of several games: drop the files of one game (Choose Again)\n' + '\n'.join(lines),
                          '放进来的是几个游戏的文件：一次只放一个游戏的（点“重新选择”）\n' + '\n'.join(lines)))
        return p
    known = [i for i in bases if i.game]
    base = (known or bases or [None])[0]
    if base is None:
        other = [i for i in items if i.role == 'ignored' and not i.inside
                 and (i.title or Path(i.path).suffix.lower() == '.nup')]
        if other:   # a game file of a game this repository does not build: said as what it is
            p.blocks.append(other[0].note)
            return p
        updates = by_role('update')
        p.blocks.append(t('no game: drop the base game (XCI or NSP), a Switch 2 dump folder or a .nup package' +
                          (' (an update alone is not enough)' if updates else ''),
                          '还没有游戏本体：放入 XCI 或 NSP、Switch 2 游戏文件夹或 .nup 包' + ('（只有更新不够）' if updates else '')))
        return p
    p.base, p.game = base.path, base.game
    p.app_name = repo.GAMES.get(base.game) if base.game else None
    if base.role in ('ns2_dump', 'nup', 'ss_dump'):
        p.needs_keys = False
        if base.role == 'ss_dump':
            p.update_version = 65536
        elif base.role == 'nup':
            p.update_version = base.version
        p.fonts = (by_role('fonts') or [Item('', '')])[-1].path or None
        # a Switch 2 dump holds no fonts: games/oot/build.py prepare_fonts and build_app.py main_ns2 take open-source ones
        if base.role in ('ns2_dump', 'nup') and not p.fonts:
            p.notes.append(t('no system fonts given: the build uses open-source Noto fonts (downloaded once) unless your work '
                             'folder already has fonts; to use the firmware fonts of a Tears of the Kingdom or Breath of the '
                             'Wild build, drop their shared-fonts folder too',
                             '没有放系统字体：打包会用开源的 Noto 字体（只下载一次），工作目录里已有字体就用已有的；'
                             '想用王国之泪或旷野之息打包留下的固件字体，把那个 shared-fonts 文件夹也放进来'))
        if p.game == 'oot' and no_metal_compiler():
            p.blocks.append(oot_needs_metal())
            p.tool_blocks.append(p.blocks[-1])   # the Launcher shows it with this Mac's other needs, at the build
            p.tool_needs.append('metal')
        return p
    if not p.keys:
        p.keys_problems.append(t('no keys: choose the prod.keys dumped from your Switch', '还没有 keys：选择从你的 Switch 导出的 prod.keys'))
    else:
        from tools.builder_jobs.identify import load_keys
        loaded, problem = load_keys(p.keys)
        if problem:
            p.keys_problems.append(problem)
        # the keys cannot read, or are too old for, a file of this game (the base among them): said once each
        for i in items:
            if i.problem and (i.game == p.game or i.path == p.base) and i.problem not in p.keys_problems:
                p.keys_problems.append(i.problem)
    if not p.game and p.keys and not p.keys_problems:
        p.keys_problems.append(t('the game of this card cannot be read here (this Python has no cryptography package): '
                          'build it with build_app.py',
                          '这里读不出卡带是哪个游戏（这个 Python 没有 cryptography）：请用 build_app.py 打包'))
    updates = by_role('update', p.game)
    if updates:
        supported = _supported(p.game)
        usable = updates if supported is None else [u for u in updates if u.version in supported] or updates
        newest = _newest(usable)
        p.update, p.update_version = newest.path, newest.version
        skipped = [Path(u.path).name for u in updates if u not in usable]
        if skipped:
            p.notes.append(t('not a version this repository builds, left out: ', '本项目不支持的版本，不用：') + ', '.join(skipped))
        if len(usable) > 1:
            p.notes.append(t(f'{len(usable)} updates: the newest one is used', f'{len(usable)} 个更新：用最新的一个'))
    p.dlc = [i.path for i in by_role('dlc', p.game)]
    # Animal Crossing on a combined card (its update and add-ons on the card too): games/acnh/build.py splits it
    # (tools/split_card.py), its update given as the card itself unless one is dropped separately
    if (p.game == 'acnh' and not p.update and GAME_SUFFIXES.get(Path(p.base).suffix.lower()) == 'xci'
            and (card_titles(p.base) or 0) > 1):
        p.update, p.update_version = p.base, base.holds_update if base.holds_update is not None else base.version
        p.notes.append(t('this card image also holds the update and add-ons: the build takes them from it',
                         '这个卡带镜像里也有更新和追加内容：打包时直接从里面取'))
    firmware, fonts = by_role('firmware'), by_role('fonts')
    p.firmware = firmware[-1].path if firmware else None
    p.fonts = fonts[-1].path if fonts else None
    b = repo.build_app()
    has_fonts = p.firmware or p.fonts or (work and b.fonts_valid(Path(work) / 'totk/extracted/firmware/shared-fonts'))
    # build_app.py: an NSP holds no system fonts, nor does a Breath of the Wild card image (step_data_botw)
    font_block = None
    if p.game not in ('ss', 'dread', 'worldend', 'tmnt') and (GAME_SUFFIXES.get(Path(p.base).suffix.lower()) == 'nsp' or p.game == 'botw') and not has_fonts:
        font_block = t('this game file holds no system fonts (an NSP, or a Breath of the Wild card): drop your unpacked firmware folder (or the shared-fonts folder of '
                       'an earlier build) too, or use the open-source fonts',
                       '这个游戏文件不含系统字体（NSP，或旷野之息的卡带）：把解开的固件文件夹（或以前打包留下的 shared-fonts 文件夹）也放进来，或者用开源字体')
    # a card image whose system update partition is missing or empty (merged / trimmed, "1G+1U") has no fonts either: said
    # here, before the build, instead of at its font step (after PR #2247, which made that step's error say the same);
    # the games that take no system fonts are left out, as in the check above (TMNT: after PR #2125)
    elif (p.game not in ('ss', 'dread', 'worldend', 'tmnt') and GAME_SUFFIXES.get(Path(p.base).suffix.lower()) == 'xci'
          and not has_fonts and card_update_ncas(p.base) == 0):
        font_block = t('this card image keeps no system update partition (a merged or trimmed card, "1G+1U"), so no system '
                       'fonts: drop your unpacked firmware folder (or the shared-fonts folder of an earlier build) too, or use '
                       'the open-source fonts',
                       '这个卡带镜像没有系统更新分区（合并或精简过的卡带，“1G+1U”），所以没有系统字体：把解开的固件文件夹'
                       '（或以前打包留下的 shared-fonts 文件夹）也放进来，或者用开源字体')
    # No system fonts: the open-source stand-ins (tools/deps/open_fonts.py, Ocarina of Time's), at once and without a word
    # (user 2026-11-20: "如果遇到 没有字体的情况下 自动使用 开源字体 … 不要报错 不要提示 直接顺利安装"; until then a block
    # with the 用开源字体 button, 2026-11-18). Fonts the player drops (firmware, shared-fonts) still win: with them there
    # is no font_block. open_fonts (the button's) is kept for callers that pass it and changes nothing now.
    if font_block:
        p.open_fonts = True
    if p.game == 'ss' and (not p.update or p.update_version not in (None, 65536)):
        p.blocks.append(t('Skyward Sword HD needs update 1.0.1 (v65536)', '天空之剑 HD 需要 1.0.1 更新（v65536）'))
    if p.game in ('dread', 'worldend', 'tmnt') and p.dlc:
        p.blocks.append(t('This game does not accept add-on packages', '此游戏不接收附加内容包'))
    if p.game == 'eow' and p.update_version != 196608:
        p.blocks.append(t('Echoes of Wisdom needs its 1.1.0 update next to the base game', '智慧的再现需要和本体一起放入 1.1.0 更新'))
    if p.game == 'odyssey' and p.dlc:
        p.blocks.append(t('Super Mario Odyssey does not accept add-on packages', '超级马力欧 奥德赛不接收附加内容包'))
    if p.game == 'eow' and p.dlc:
        p.blocks.append(t('Echoes of Wisdom does not accept add-on packages', '智慧的再现不接收附加内容包'))
    if p.game == 'acnh' and not p.update:
        p.blocks.append(t('Animal Crossing needs its 3.0.3 update (or 3.0.1 / 1.2.1) next to the base game',
                          '动物森友会需要和本体一起放入 3.0.3 更新（或 3.0.1 / 1.2.1）'))
    if p.game == 'acnh' and p.dlc and p.update_version not in (2097152, 2228224):
        p.blocks.append(t('Animal Crossing\'s add-ons need the 3.0.1 or 3.0.3 update', '动物森友会的追加内容需要 3.0.1 或 3.0.3 更新'))
    if p.game == 'ss' and p.dlc:
        p.blocks.append(t('Skyward Sword HD does not accept add-on packages', '天空之剑 HD 不接收附加内容包'))
    p.blocks = p.keys_problems + p.blocks
    return p


def report(paths, work=None, open_fonts=False):
    items = classify(paths)
    return {'items': [asdict(i) for i in items], 'plan': asdict(plan(items, work, open_fonts))}


def open_fonts_note():
    return t('open-source fonts: the build makes the system fonts from Noto fonts (SIL OFL 1.1, downloaded once); the '
             'button icons of slot 5 have no open equivalent. Drop a firmware or shared-fonts folder to use the real ones',
             '用开源字体：打包时用开源的 Noto 字体（SIL OFL 1.1，只下载一次）代替系统字体，手柄按键图标没有开源替代；'
             '想用原版字体，把固件或 shared-fonts 文件夹放进来')
