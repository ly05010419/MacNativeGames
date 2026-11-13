"""What each dropped file or folder is, and the build it makes (pure apart from reading the files' headers).

Contract: classify(paths) never raises for a bad file: every path becomes an Item, and one that is not a game file is
'ignored' with the reason. A folder is looked into (two levels) unless it is itself a Switch 2 dump, a firmware folder
or a shared-fonts folder. The game comes from the files: a package's ticket (title ID of its rights ID), a Switch 2
dump's main.npdm, a .nup package (Ocarina of Time), or a 16-digit title ID in the file name. An XCI without one in its
name is a game card whose game build_app.py tells at the start of the build. plan(items) picks one game and its files,
and lists what still blocks a build."""
from dataclasses import asdict, dataclass, field
from pathlib import Path
import re

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

GAME_SUFFIXES = {'.xci': 'xci', '.xcz': 'xci', '.nsp': 'nsp', '.nsz': 'nsp'}
_TITLE_IN_NAME = re.compile(r'(?<![0-9A-Fa-f])(0[14]00[0-9A-Fa-f]{12})(?![0-9A-Fa-f])')
_VERSION_IN_NAME = re.compile(r'(?<![0-9A-Za-z])v(\d{1,10})(?![0-9]|\.\d)', re.IGNORECASE)   # [v720896], (v131072), v720896
_DISPLAY_IN_NAME = re.compile(r'(?<![0-9.])(\d+\.\d+\.\d+)(?![0-9]|\.\d)')                      # 1.2.1
_HEADER_KEY = re.compile(r'^\s*header_key\s*=\s*[0-9A-Fa-f]{64}\s*$', re.MULTILINE)


@dataclass(frozen=True)
class Item:
    path: str
    role: str                 # keys | base | update | dlc | ns2_dump | nup | firmware | fonts | ignored
    game: str | None = None
    title: str | None = None
    version: int | None = None   # from the file name's [vNNN], to pick the newest of several updates
    note: str = ''


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
    notes: list = field(default_factory=list)


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


def _version(name):
    """The title version a package's name gives: [vNNN], or a display version (1.2.1) of a version build_app.py knows."""
    found = _VERSION_IN_NAME.search(name)
    if found:
        return int(found.group(1))
    found = _DISPLAY_IN_NAME.search(name)
    if found:
        for v in repo.build_app().VERSIONS.values():
            if v.get('display_version') == found.group(1) and str(v.get('version', '')).isdigit():
                return int(v['version'])
    return None


def _package(path):
    b = repo.build_app()
    try:
        if path.stat().st_size == 0:
            return Item(str(path), 'ignored', note=t('empty file', '空文件'))
        kind = GAME_SUFFIXES[path.suffix.lower()]
        titles = b.ticket_title_ids(path) if kind == 'nsp' else []
    except OSError as e:
        return Item(str(path), 'ignored', note=t(f'cannot be read: {e.strerror}', f'读不了：{e.strerror}'))
    titles = titles or [m.group(1).upper() for m in _TITLE_IN_NAME.finditer(path.name)]
    version = _version(path.name)
    if not titles:
        if kind == 'xci':
            return Item(str(path), 'base', note=t('game card: its game is read with your keys', '游戏卡带：用你的 keys 读出是哪个游戏'), version=version)
        return Item(str(path), 'ignored', note=t('no title ID in its tickets or name: rename it with its [title ID] or use an XCI',
                                                 '票据和文件名里都没有标题 ID：文件名里加上 [标题 ID]，或用 XCI'))
    title = titles[0]
    _, role = b.title_kind(title)
    game = repo.game_of_title(title)
    if game is None:
        return Item(str(path), 'ignored', title=title, note=t(f'title {title} is not a game this repository builds', f'{title} 不是本项目能打包的游戏'))
    return Item(str(path), role, game=game, title=title, version=version)


def _folder(path):
    """A Switch 2 dump, a firmware or a shared-fonts folder, or None for a folder to look into."""
    b = repo.build_app()
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
        out.append(_package(path))
    elif suffix == '.nup':
        out.append(Item(str(path), 'nup', game='oot', title=repo.OOT_TITLE))
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
    return list(dict.fromkeys(out))   # the same file dropped twice counts once


def _newest(items):
    return max(items, key=lambda i: (i.version or 0, Path(i.path).suffix.lower() == '.nsp', Path(i.path).stat().st_mtime))


def _supported(game):
    """The title versions of game build_app.py has verified values for (its VERSIONS), the base's 0 included."""
    return {int(v['version']) for v in repo.build_app().VERSIONS.values()
            if str(v.get('version', '')).isdigit() and repo.game_of_title(str(v.get('title_id', '0' * 16))) == game}


def plan(items, work=None):
    """The build these items make. work: the work folder the build will use (fonts extracted there count)."""
    p = Plan()
    by_role = lambda role, game=None: [i for i in items if i.role == role and (game is None or i.game in (game, None))]
    keys = by_role('keys')
    p.keys = keys[-1].path if keys else None
    bases = [i for i in items if i.role in ('base', 'ns2_dump', 'nup')]
    p.games_found = sorted({i.game for i in items if i.game})
    known = [i for i in bases if i.game]
    if len({i.game for i in known}) > 1:
        names = ', '.join(repo.GAMES[g] for g in sorted({i.game for i in known}))
        p.blocks.append(t(f'files of several games: drop the files of one game ({names})',
                          f'放进来的是几个游戏的文件：一次只放一个游戏的（{names}）'))
        return p
    base = (known or bases or [None])[0]
    if base is None:
        updates = by_role('update')
        p.blocks.append(t('no game: drop the base game (XCI or NSP), a Switch 2 dump folder or a .nup package' +
                          (' (an update alone is not enough)' if updates else ''),
                          '还没有游戏本体：放入 XCI 或 NSP、Switch 2 游戏文件夹或 .nup 包' + ('（只有更新不够）' if updates else '')))
        return p
    p.base, p.game = base.path, base.game
    p.app_name = repo.GAMES.get(base.game) if base.game else None
    if base.role in ('ns2_dump', 'nup'):
        p.needs_keys = False
        p.fonts = (by_role('fonts') or [Item('', '')])[-1].path or None
        if base.game == 'oot' and not p.fonts:
            p.notes.append(t('Ocarina of Time needs the six shared-font files of a Switch 2 firmware unless your work folder '
                             'already has them: drop their folder too',
                             '时之笛需要 Switch 2 固件的六个系统字体文件（工作目录里已有就不用）：把它们的文件夹也放进来'))
        return p
    if not p.keys:
        p.keys_problems.append(t('no keys: choose the prod.keys dumped from your Switch', '还没有 keys：选择从你的 Switch 导出的 prod.keys'))
    else:
        from tools.builder_jobs.identify import identify
        title, problem = identify(p.base, p.keys)
        if problem:
            p.keys_problems.append(problem)
        if title and not p.game:
            p.game = repo.game_of_title(title)
            p.app_name = repo.GAMES.get(p.game)
            p.games_found = sorted(set(p.games_found) | {p.game})
    if not p.game and p.keys and not p.keys_problems:
        p.keys_problems.append(t('the game of this card cannot be read here (this Python has no cryptography package): '
                          'rename the file with its [title ID], or build it with build_app.py',
                          '这里读不出卡带是哪个游戏（这个 Python 没有 cryptography）：文件名里加上 [标题 ID]，或用 build_app.py 打包'))
    updates = by_role('update', p.game)
    if updates:
        supported = _supported(p.game)
        usable = [u for u in updates if u.version in supported] or updates
        newest = _newest(usable)
        p.update, p.update_version = newest.path, newest.version
        skipped = [Path(u.path).name for u in updates if u not in usable]
        if skipped:
            p.notes.append(t('not a version this repository builds, left out: ', '本项目不支持的版本，不用：') + ', '.join(skipped))
        if len(usable) > 1:
            p.notes.append(t(f'{len(usable)} updates: the newest one is used', f'{len(usable)} 个更新：用最新的一个'))
    p.dlc = [i.path for i in by_role('dlc', p.game)]
    firmware, fonts = by_role('firmware'), by_role('fonts')
    p.firmware = firmware[-1].path if firmware else None
    p.fonts = fonts[-1].path if fonts else None
    b = repo.build_app()
    has_fonts = p.firmware or p.fonts or (work and b.fonts_valid(Path(work) / 'totk/extracted/firmware/shared-fonts'))
    # build_app.py: an NSP holds no system fonts, nor does a Breath of the Wild card image (step_data_botw)
    if (GAME_SUFFIXES.get(Path(p.base).suffix.lower()) == 'nsp' or p.game == 'botw') and not has_fonts:
        p.blocks.append(t('this game file holds no system fonts (an NSP, or a Breath of the Wild card): drop your unpacked firmware folder (or the shared-fonts folder of '
                          'an earlier build) too',
                          '这个游戏文件不含系统字体（NSP，或旷野之息的卡带）：把解开的固件文件夹（或以前打包留下的 shared-fonts 文件夹）也放进来'))
    p.blocks = p.keys_problems + p.blocks
    return p


def report(paths, work=None):
    items = classify(paths)
    return {'items': [asdict(i) for i in items], 'plan': asdict(plan(items, work))}
