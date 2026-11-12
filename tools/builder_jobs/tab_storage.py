"""The Storage tab of MNG Launcher: the work folder of the app's build and the game's shader cache -- where they are, how
much they hold, and the two things a player does with them, clean (after a confirmation in the window) and move (the
work folder only). The removed Tk window's Storage tab, over the same model,
tools/builder/storage.py, unchanged: its guards (refuse_reason: never a root, the home folder, a system folder, nor a
folder holding or held by the app, the saves or this checkout) decide what may be cleaned or moved.

The work folder is now the one the app records (Info.plist MNGWorkFolder, build_app.recorded_work_folder: what the
Launcher's update and update_runtime.py use; docs/install.md, Folders), no longer a folder next to the game file. The
window never sends a path to clean: every action finds the folder again here.

Actions: state() -> both folders without sizes (instant); measure(folder) -> one folder with its size (walks it: seconds
to a minute for a big work folder); clean(folder) -> what was removed; move(target, confirmed) -> the question to ask
first (confirmed false), then what was moved and how the app records the new folder."""
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import tempfile
import time
from types import SimpleNamespace

from tools.builder import saves, storage
from tools.builder_jobs import repo
from tools.builder_jobs.tabs import TabError
from tools.builder_jobs.text import t

FOLDERS = ('work', 'shader')
FALLBACK = {'totk': 'TOTK_build', 'botw': 'BotW_build'}   # update_runtime.py: an app that records no work folder
BUILDS = re.compile(r'(^|[/\s])(build_app|update_runtime)\.py(\s|$)')


def refusal(reason):
    """A guard's reason id (storage.refuse_reason, storage.move, this page's own) in words; other text as it is."""
    words = {
        'missing': t('That folder does not exist.', '这个文件夹不存在。'),
        'root': t('That is the disk root.', '这是磁盘的根目录。'),
        'home': t('That is your home folder, or one of its parents.', '这是你的用户文件夹，或它的上层文件夹。'),
        'system': t('That is a macOS system folder.', '这是 macOS 的系统文件夹。'),
        'protected': t('That folder holds the game, its app or its saves.', '这个文件夹里有游戏、它的 App 或存档。'),
        'file': t('That is a file, not a folder.', '这是一个文件，不是文件夹。'),
        'symlink': t('That is a link to another folder; choose the folder itself.', '这是指向别处的链接，请选文件夹本身。'),
        'source_missing': t('The work folder does not exist.', '工作文件夹不存在。'),
        'inside_source': t('The new folder is inside the old one.', '新文件夹在旧文件夹里面。'),
        'target_not_empty': t('The new folder is not empty.', '新文件夹不是空的。'),
        'checkout': t('That folder is inside the MacNativeGames checkout: a build refuses a work folder there.',
                      '这个文件夹在 MacNativeGames 源码目录里：打包不接受放在那里的工作文件夹。'),
        'apps': t('That folder is inside the folder that holds your apps: keep the work folder outside it '
                  '(docs/install.md, Folders).',
                  '这个文件夹在放 App 的文件夹里：工作文件夹要放在它外面（docs/install.md 的 Folders 一节）。'),
        'running': t('The game is running: close it first.', '游戏开着：先关掉游戏。'),
        'building': t('A build or an update is running: wait until it ends.', '正在打包或更新：等它结束。'),
        'unreachable': t('The folder the app records cannot be found, nor the folder it was in (a disk not connected?).',
                         '找不到 App 记录的文件夹，也找不到它的上层文件夹（磁盘没接上？）。'),
    }
    return words.get(reason, reason)


def running_builds():
    """The command lines of build_app.py and update_runtime.py running now (the Launcher's jobs run them too)."""
    try:
        out = subprocess.run(['ps', '-Ao', 'args='], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [line.strip() for line in out.splitlines() if BUILDS.search(line)]


def slot_taken(ctx):
    """Whether any copy of the game holds its slot: the lock in game-slot held by another process (the launcher's own
    test, game_state.game_running, without its 'a player's game' condition: a test game writes the shader cache too).
    Taking the lock for an instant is what the launcher does too."""
    try:
        fd = os.open(ctx.data_dir / 'game-slot/lock', os.O_RDONLY | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    except BlockingIOError:
        return True
    finally:
        os.close(fd)


def protected(ctx):
    """What no clean or move may hold or be held by: the app, the saves, nxnative's saves, this checkout."""
    return (ctx.app, saves.profile_dir(ctx.spec, ctx.home), saves.nxnative_root(ctx.home), ctx.repo)


def work_folder(ctx):
    """(the work folder the app's next update uses, kind): kind is recorded, recreated (recorded, deleted, made again
    there by the next update), unrecorded (an app built before 31 October 2026: the old folder next to it) or
    unreachable (recorded on a disk or in a folder that is gone)."""
    b = repo.build_app()
    fallback = FALLBACK.get(ctx.spec.id, f'{ctx.spec.data_folder}_build')
    try:
        path, _why = b.recorded_work_folder(ctx.app, fallback)
    except b.BuildError:
        info = plistlib.loads((ctx.app / 'Contents/Info.plist').read_bytes())
        return Path(info.get('MNGWorkFolder', '')), 'unreachable'
    if path == ctx.app.parent / fallback:
        return path, 'unrecorded'
    return path, 'recorded' if path.is_dir() else 'recreated'


def shader_folder(ctx):
    return storage.shader_dir(ctx.spec, ctx.home)


def folder_view(ctx, folder):
    """One folder: its label, path, where the path comes from, and what cleaning it means."""
    if folder == 'work':
        path, kind = work_folder(ctx)
        where = {
            'recorded': t(f'Recorded by {ctx.app.name} (MNGWorkFolder): its updates work here.',
                          f'{ctx.app.name} 记录的工作文件夹（MNGWorkFolder）：更新在这里进行。'),
            'recreated': t(f'Recorded by {ctx.app.name}, but deleted: the next update makes it again there.',
                           f'{ctx.app.name} 记录了它，但已被删除：下次更新会在原处重新建。'),
            'unrecorded': t(f'{ctx.app.name} records no work folder (built before 31 October 2026): its updates use this '
                            'folder next to it.',
                            f'{ctx.app.name} 没有记录工作文件夹（2026-10-31 以前打的包）：更新用它旁边的这个文件夹。'),
            'unreachable': refusal('unreachable'),
        }[kind]
        note = t('It holds the extracted game files, translated shaders, pipeline caches and logs of the build. Cleaning '
                 'it is safe for the app and the saves, but an update needs what is in it: after cleaning, the next '
                 'update makes it again (slower), and an app that still needs files from its build, or a fast update '
                 '(--seed), needs the game files and keys again (a full build). Clean it only when you would rebuild '
                 'anyway (docs/install.md, Folders).',
                 '里面是打包解出的游戏文件、翻译好的着色器、管线缓存和日志。清理不影响 App 和存档，但更新要用里面的东西：'
                 '清理之后，下次更新会重新生成（更慢）；还需要打包文件的旧 App，或快速更新（--seed），要重新提供游戏文件和'
                 '密钥（完整打包）。只在反正要重新打包时再清理（docs/install.md 的 Folders 一节）。')
        label = t('Build cache (work folder)', '打包缓存（工作文件夹）')
    elif folder == 'shader':
        path, kind = shader_folder(ctx), 'shader'
        name = storage.shader_env(ctx.spec)[0]
        where = t(f'The launcher writes this cache. To keep it elsewhere, start the game with {name} set to the new folder; '
                  'this page shows that folder after the Launcher is started again.',
                  f'这个缓存由游戏的启动器写。想放在别处，用 {name} 指定新文件夹再启动游戏；重新打开 Launcher 后这里会显示那个文件夹。')
        note = t('The game makes it again: the first minutes after cleaning can stutter while shaders are built again.',
                 '游戏会重新生成它：清理后开头几分钟可能因为重新编译着色器而卡顿。')
        label = t('Shader cache', '着色器缓存')
    else:
        raise TabError(t(f'no folder {folder!r}: one of {", ".join(FOLDERS)}', f'没有 {folder!r} 这个文件夹：只有 {", ".join(FOLDERS)}'))
    return {'id': folder, 'label': label, 'path': str(path), 'kind': kind, 'where': where, 'note': note,
            'exists': path.is_dir(), 'movable': folder == 'work' and kind != 'unreachable',
            'size': None, 'detail': '', 'confirm': confirm_work() if folder == 'work' else ''}


def confirm_work():
    """The work folder's question (user 2026-11-12: say what cleaning costs, then ask). update_runtime.plan: the app holds
    its game data, so the next update is still the express one, but it makes the work folder's files again first; only
    one that must also make the startup shaders again (fast update) needs the game files and keys."""
    return t('After cleaning, the next update first makes these files again, which takes much longer than usual; if that '
             'update must also make the startup shaders again, it needs the game files and keys (a full build). Clean it?',
             '清理之后，下次更新要先重新生成这些文件，比平时慢很多；如果那次更新还要重做启动着色器，就得用游戏文件和密钥'
             '完整打包。是否清理？')


def confirm_clean(path, size):
    shown = storage.human(size.bytes) if size else '?'
    files = size.files if size else '?'
    return t(f'Delete everything inside {path}?\n\n{shown} in {files} files. The game or the next build makes it again; '
             'saves and the app are not touched.',
             f'删除 {path} 里的全部内容？\n\n{files} 个文件，共 {shown}。游戏或下次打包会重新生成；存档和 App 不受影响。')


def size_line(path, size):
    if not Path(path).is_dir():
        return t('Does not exist yet.', '还不存在。')
    line = t(f'{storage.human(size.bytes)} in {size.files} files', f'{size.files} 个文件，共 {storage.human(size.bytes)}')
    if size.errors:
        line += ' ' + t(f'Some files could not be read: {size.errors[0]}', f'有些文件读不了：{size.errors[0]}')
    return line


def sizes_file(ctx):
    """The sizes measured before, by folder path: the page shows them until asked to measure again (user 2026-11-12)."""
    return ctx.builder_dir / 'storage-sizes.json'


def read_sizes(ctx):
    try:
        data = json.loads(sizes_file(ctx).read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def write_sizes(ctx, sizes):
    path = sizes_file(ctx)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', dir=path.parent, prefix='.storage-sizes.', delete=False) as f:
        json.dump(sizes, f, indent=1)
    os.replace(f.name, path)


def size_json(size):
    return {'bytes': size.bytes, 'files': size.files, 'shown': storage.human(size.bytes), 'errors': list(size.errors[:5])}


def with_size(view, size, measured_at=None):
    """The folder's view with a size; measured_at (seconds) when the size is a remembered one."""
    detail = size_line(view['path'], size)
    if measured_at is not None:
        when = time.strftime('%Y-%m-%d %H:%M', time.localtime(measured_at))
        detail += t(f' (measured {when})', f'（{when} 计算）')
    return {**view, 'size': size_json(size), 'detail': detail,
            'confirm': confirm_work() if view['id'] == 'work' else confirm_clean(view['path'], size)}


def remembered(ctx, view):
    """The view with its last measured size, when there is one for this path and the folder is still there."""
    record = read_sizes(ctx).get(view['path'])
    if not isinstance(record, dict) or not view['exists']:
        return view
    try:
        size = SimpleNamespace(bytes=int(record['bytes']), files=int(record['files']), errors=list(record.get('errors', [])))
        return with_size(view, size, float(record['at']))
    except (KeyError, TypeError, ValueError):
        return view


def state(ctx):
    return {'title': t('Caches', '缓存'),
            'note': t('Two folders the builds and the game write. Cleaning removes only what is inside them; the game or '
                      'the next build makes it again. Saves and the app are not touched.',
                      '打包和游戏写的两个文件夹。清理只删除里面的内容，游戏或下次打包会重新生成；存档和 App 不受影响。'),
            'folders': [remembered(ctx, folder_view(ctx, f)) for f in FOLDERS]}


def measure(ctx, folder):
    view = folder_view(ctx, folder)
    size = storage.measure(view['path'])
    if view['exists']:
        at = time.time()
        write_sizes(ctx, {**read_sizes(ctx), view['path']: {**size_json(size), 'at': at}})
    return with_size(view, size)   # measured just now: the time shows when the page opens on it later


def busy_reason(ctx, folder):
    """Why this folder may not be cleaned or moved right now, or None."""
    if slot_taken(ctx):
        return 'running'
    if folder == 'work' and running_builds():
        return 'building'
    return None


def clean(ctx, folder):
    """Empty one folder (storage.clean: the folder stays, its contents go), after the window's confirmation."""
    view = folder_view(ctx, folder)
    if view['kind'] == 'unreachable':
        raise TabError(refusal('unreachable'))
    reason = busy_reason(ctx, folder)
    if reason:
        raise TabError(t('Could not clean: ', '没能清理：') + refusal(reason))
    removed, reason = storage.clean(view['path'], ctx.home, protected(ctx))
    if reason is not None:
        raise TabError(t('Could not clean: ', '没能清理：') + refusal(reason))
    sizes = read_sizes(ctx)
    if view['path'] in sizes:   # the size measured before went with the files
        write_sizes(ctx, {k: v for k, v in sizes.items() if k != view['path']})
    return {'folder': folder, 'path': view['path'], 'bytes': removed.bytes, 'files': removed.files,
            'message': t(f'Removed {storage.human(removed.bytes)} in {removed.files} files.',
                         f'删除了 {removed.files} 个文件，共 {storage.human(removed.bytes)}。')}


def target_reason(ctx, target, source):
    """Why the work folder may not move into target (which need not exist yet), or None: storage.refuse_reason's guards,
    and the two places docs/install.md (Folders) keeps a work folder out of, this checkout and the folder of the apps."""
    target = Path(target).expanduser()
    if not target.is_absolute():
        return 'missing'
    resolved, home, checkout = target.resolve(), ctx.home.resolve(), ctx.repo.resolve()
    if resolved == checkout or checkout in resolved.parents:
        return 'checkout'
    apps = ctx.app.parent.resolve()
    if resolved == apps or apps in resolved.parents:
        return 'apps'
    if target.exists() or target.is_symlink():
        reason = storage.refuse_reason(target, ctx.home, protected(ctx))
        if reason is not None:
            return reason
    if resolved == Path(resolved.anchor):
        return 'root'
    if resolved == home or resolved in home.parents:
        return 'home'
    if len(resolved.parts) > 1 and resolved.parts[1] in storage.SYSTEM:
        return 'system'
    for item in protected(ctx):
        item = Path(item).expanduser().resolve()
        if resolved == item or item in resolved.parents or resolved in item.parents:
            return 'protected'
    source = source.resolve()
    if resolved == source or source in resolved.parents:
        return 'inside_source'
    if resolved.is_dir() and any(resolved.iterdir()):
        return 'target_not_empty'
    return None


def _device(path):
    """The device of a path's nearest existing folder (a target need not exist yet)."""
    path = Path(path)
    while not path.exists() and path != path.parent:
        path = path.parent
    return os.stat(path).st_dev


def record_command(ctx, target):
    """The one command that makes the app record the new folder: an express update given it (update_runtime.py --work)."""
    try:
        info = plistlib.loads((ctx.app / 'Contents/Info.plist').read_bytes())
    except (OSError, ValueError):
        info = {}
    python = info.get('MNGBuildPython') or 'python3.14'
    checkout = Path(info.get('MNGSourceRepo') or ctx.repo)
    return ' '.join(shlex.quote(str(p)) for p in (python, checkout / 'update_runtime.py', ctx.app, '--work', target))


def move(ctx, target, confirmed=False):
    """Move what is in the work folder into target (storage.move: target absent or empty). Unconfirmed: only the checks
    and the question for the window. The app still records the old folder until an update is given the new one, so the
    answer names that command."""
    view = folder_view(ctx, 'work')
    if not view['movable']:
        raise TabError(t('Could not move: ', '没能移动：') + refusal(view['kind']))
    source, target = Path(view['path']), Path(target).expanduser()
    reason = target_reason(ctx, target, source) or busy_reason(ctx, 'work')
    if reason:
        raise TabError(t('Could not move: ', '没能移动：') + refusal(reason))
    command = record_command(ctx, target.resolve())
    if not source.is_dir() or not any(source.iterdir()):
        return {'moved': 0, 'confirm': '', 'command': command,
                'message': t(f'There is nothing in {source} to move. To have the app use {target}, run once: {command}',
                             f'{source} 里没有可移动的内容。要让 App 用 {target}，运行一次：{command}')}
    volume = '' if _device(target) == _device(ctx.app) else t(
        ' The new folder is on another disk than the app: moving copies everything, and later builds copy the game data '
        'instead of cloning it.', ' 新文件夹和 App 不在同一块磁盘上：移动要整份复制，以后的打包也要复制游戏数据而不是克隆。')
    if not confirmed:
        return {'moved': 0, 'command': command, 'message': '',
                'confirm': t(f'Move everything in {source} into the new folder?\n\n{target}', f'把 {source} 里的全部内容移到新文件夹？\n\n{target}') + volume}
    count, reason = storage.move(source, target)
    if reason is not None:
        raise TabError(t('Could not move: ', '没能移动：') + refusal(reason))
    return {'moved': count, 'confirm': '', 'command': command,
            'message': t(f'Moved {count} entries to {target}. {ctx.app.name} still records {source}: until it records the new '
                         f'folder, an update works in the old place again and makes its files again. Run once: {command}',
                         f'已把 {count} 项移到 {target}。{ctx.app.name} 还记着 {source}：在它改记新文件夹之前，更新会回到旧位置、'
                         f'重新生成那些文件。运行一次：{command}')}


ACTIONS = {'state': state, 'measure': measure, 'clean': clean, 'move': move}
