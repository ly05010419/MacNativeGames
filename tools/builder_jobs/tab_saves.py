"""The Saves tab of MNG Launcher: where the game keeps its save, backups made of plain files plus backup.json, and
restore (tools/builder/saves.py, unchanged; the removed Tk window did the same steps).

A restore never starts with deleting, as in the old window: it refuses while the game runs, the plan (what it writes,
what it leaves, what already matches) is shown before the player confirms, the current save is copied to
<game>-pre-restore-<time> under the backup folder first, and files the backup does not hold stay where they are. One
addition: restore takes the plan's id and refuses when the plan read again differs (the game saved in between, or the
backup changed), so what is written is what the player confirmed.

Actions: state() -> the save sources and the backups; backup(kind, title_id, root, label) -> the new backup;
plan(backup) -> what a restore of that folder does; restore(backup, plan_id) -> what it did; reveal(path) -> opens the
backup folder (or a backup) in the Finder."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from tools.builder import game_state, saves, storage
from tools.builder.games import PAGE_GAMES as GAMES   # the games these pages serve
from tools.builder_jobs.tabs import TabError
from tools.builder_jobs.text import t

LIST_LIMIT = 200            # the plan lists at most this many paths per group; the counts are always whole
LABEL = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,39}')

# What the old window showed (its gui.saves.* texts, removed with the Tk window 2026-11-12), and in Chinese.
NOTE = t('A backup is a plain folder: the same files, plus a backup.json saying what it holds.',
         '备份就是一个普通文件夹：同样的文件，加一个说明内容的 backup.json。')
HASH_NOTE = t('SHA-256 for files up to 2 GiB.', '2 GiB 以内的文件记 SHA-256。')
REASONS = {
    'source_missing': t('The save folder is not there (no save yet).', '存档文件夹不存在（还没有存档）。'),
    'root_inside_source': t('The backup folder is inside the save folder: choose another one.',
                            '备份文件夹在存档文件夹里面：换一个。'),
    'backup_exists': t('A backup of that name already exists: wait a second and try again.',
                       '同名备份已经存在：等一秒再试。'),
}


def human(size):
    return storage.human(size)


def backup_root(ctx):
    return saves.default_backup_root(ctx.home)


def sources(ctx):
    """The old window's rows: this game's profile, then every nxnative title folder."""
    return (saves.source(ctx.spec, ctx.home, os.environ), *saves.nxnative_sources(ctx.home, (ctx.spec.title_id,)))


def slot_held(data_dir):
    """The game slot's lock held by any process (a player's game or a test game): game_state.game_running only counts
    the player's own, but any game on this profile writes its save back over a restored one."""
    try:
        fd = os.open(game_state.slot_dir(data_dir) / 'lock', os.O_RDONLY | os.O_CLOEXEC)
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


def running(ctx):
    return game_state.game_running(ctx.data_dir) or slot_held(ctx.data_dir)


def source_view(source):
    has_save = source.exists and bool(source.files)
    nx = source.kind == 'nxnative'
    return {'kind': source.kind, 'title_id': source.title_id, 'game': source.game, 'path': str(source.path),
            'name': f"{t('nxnative title', 'nxnative 游戏')} {source.title_id}" if nx else t('Game save', '游戏存档'),
            'files': len(source.files), 'bytes': source.bytes, 'can_backup': has_save,
            'line': t(f'{human(source.bytes)} in {len(source.files)} files', f'{len(source.files)} 个文件，共 {human(source.bytes)}')
            if has_save else t(f'Nothing at {source.path} (no save yet).', f'{source.path} 里没有东西（还没有存档）。')}


def game_name(game):
    return GAMES[game].display_name if game in GAMES else (game or t('unknown game', '未知游戏'))


def backup_view(item):
    return {'path': str(item.directory), 'name': item.directory.name, 'game': item.game, 'game_name': game_name(item.game),
            'title_id': item.title_id, 'source': item.source, 'created': item.created, 'files': len(item.files),
            'bytes': item.bytes, 'size': human(item.bytes), 'hashed': sum(1 for f in item.files if f.sha256),
            'line': t(f'{game_name(item.game)} · {item.created} · {human(item.bytes)} in {len(item.files)} files',
                      f'{game_name(item.game)} · {item.created} · {len(item.files)} 个文件，共 {human(item.bytes)}')}


def state(ctx):
    """Reads only: the sources, the backups under the default folder, and whether the game is running."""
    root = backup_root(ctx)
    found = saves.find_backups(root)
    busy = running(ctx)
    return {'game': ctx.spec.id, 'game_name': ctx.spec.display_name, 'note': NOTE, 'hash_note': HASH_NOTE,
            'root': str(root), 'root_line': t(f'Backups: {root}', f'备份位置：{root}'),
            'sources': [source_view(s) for s in sources(ctx)],
            'backups': [backup_view(b) for b in found],
            'backups_line': t(f'Backups in {root}  ({len(found)})', f'{root} 里的备份（{len(found)} 个）') if found
            else t(f'No backups in {root} yet.', f'{root} 里还没有备份。'),
            'running': busy,
            'running_line': t('The game is running: restore waits until it is closed.', '游戏开着：要先关掉游戏才能恢复。')
            if busy else ''}


def reason(code):
    return REASONS.get(code, code)


def backup(ctx, kind='profile', title_id='', root=None, label=''):
    """Copy one source (named by kind and title id, read again here, never a path from the window) into a new
    <label>-<time> folder. The label is the game id as in the old window, or the player's own word."""
    source = next((s for s in sources(ctx) if s.kind == kind and (kind == 'profile' or s.title_id == title_id)), None)
    if source is None:
        raise TabError(t(f'No such save here: {kind} {title_id}'.strip(), f'这里没有这份存档：{kind} {title_id}'.strip()))
    if not source.exists or not source.files:
        raise TabError(t(f'Nothing at {source.path} (no save yet).', f'{source.path} 里没有东西（还没有存档）。'))
    label = (label or '').strip()
    if label and not LABEL.fullmatch(label):
        raise TabError(t('A label is up to 40 letters, digits, dots, dashes or underscores, starting with a letter or digit.',
                         '标签最多 40 个字母、数字、点、减号或下划线，以字母或数字开头。'))
    game = ctx.spec.id
    label = label or (game if source.kind == 'profile' else f'{game}-{source.title_id}')
    target = Path(root).expanduser() if root else backup_root(ctx)
    made, why = saves.backup(source.path, target, label, game or source.game, time.time(), source.title_id, with_hash=True)
    if made is None:
        raise TabError(t(f'Could not back up: {reason(why)}', f'没能备份：{reason(why)}'))
    return {'backup': backup_view(made), 'message': t(f'Backed up to {made.directory}.', f'已备份到 {made.directory}。')}


def read(ctx, folder):
    """The backup at folder for this game, or a refusal in words (the old window's not_backup / wrong_game)."""
    item = saves.read_backup(Path(folder).expanduser())
    if item is None:
        raise TabError(t('That folder is not a backup this builder made.', '这个文件夹不是打包工具做的备份。'))
    if item.game and item.game != ctx.spec.id:
        if item.game not in GAMES:
            raise TabError(t('That folder is not a backup this builder made.', '这个文件夹不是打包工具做的备份。'))
        name = GAMES[item.game].display_name
        raise TabError(t(f'That backup is for {name}: choose that game first.', f'这份备份是{name}的：先选那个游戏。'))
    return item


def plan_hash(item, target, plan):
    """What the player confirms, as one hash: the backup, the target and every path in each group."""
    data = [str(item.directory), str(target), [[f.path, f.size, f.sha256] for f in plan.write],
            [f.path for f in plan.same], list(plan.kept)]
    return hashlib.sha256(json.dumps(data).encode()).hexdigest()


def plan_view(ctx, item, target, plan):
    write, kept, same = len(plan.write), len(plan.kept), len(plan.same)
    return {'backup': backup_view(item), 'target': str(target), 'target_exists': plan.target_exists,
            'write': [f.path for f in plan.write[:LIST_LIMIT]], 'kept': list(plan.kept[:LIST_LIMIT]),
            'same': [f.path for f in plan.same[:LIST_LIMIT]],
            'counts': {'write': write, 'kept': kept, 'same': same}, 'plan_id': plan_hash(item, target, plan),
            'running': running(ctx),
            'message': t(f'Restore {item.directory} over {target}? {write} files will be written, {kept} files kept as '
                         f'they are, {same} already match. The current save is backed up first.',
                         f'用 {item.directory} 恢复到 {target}？将写入 {write} 个文件，保留 {kept} 个原有文件不动，'
                         f'{same} 个已经相同。恢复前会先备份当前存档。'),
            'kept_line': t(f'Kept (not in the backup): {kept} files.', f'保留（备份里没有）：{kept} 个文件。')}


def plan(ctx, backup):
    """What restoring the backup folder over this game's save writes, keeps and finds already equal. Reads only."""
    item = read(ctx, backup)
    target = saves.source(ctx.spec, ctx.home, os.environ).path
    return plan_view(ctx, item, target, saves.plan_restore(item, target))


def restore(ctx, backup, plan_id=''):
    """The old window's restore: refuse while the game runs, back the current save up, then write the backup over it.
    Refuses when the plan read again is not the one confirmed (plan_id)."""
    item = read(ctx, backup)
    target = saves.source(ctx.spec, ctx.home, os.environ).path
    if running(ctx):
        raise TabError(t('The game is running: quit it first, or it writes its own save back over the restored one.',
                         '游戏开着：先退出游戏，否则它会把自己的存档写回去，盖掉恢复的存档。'))
    now_plan = saves.plan_restore(item, target)
    if plan_hash(item, target, now_plan) != plan_id:
        raise TabError(t('The save or the backup changed since the plan was shown: look at the plan again.',
                         '显示计划之后存档或备份变了：请重新查看恢复计划。'))
    pre = None
    if saves.list_files(target):        # nothing to keep when the game has not saved yet (a new Mac)
        pre, why = saves.backup(target, backup_root(ctx), f'{item.game or "save"}-pre-restore', item.game, time.time(),
                                item.title_id, with_hash=False)
        if pre is None:
            raise TabError(t(f'Could not back up the current save, nothing restored: {reason(why)}',
                             f'没能备份当前存档，什么都没有恢复：{reason(why)}'))
    done, error = saves.restore(item, target)
    pre_line = t(f'Current save backed up to {pre.directory}.', f'当前存档已备份到 {pre.directory}。') if pre else ''
    if error:
        raise TabError(' '.join(filter(None, [pre_line, t(f'Could not restore: {error}', f'没能恢复：{error}')])))
    return {'written': len(done.write), 'pre_backup': str(pre.directory) if pre else '',
            'message': '\n'.join(filter(None, [pre_line, t(f'Restored {len(done.write)} files from {item.directory}.',
                                                           f'已从 {item.directory} 恢复 {len(done.write)} 个文件。')]))}


def reveal(ctx, path=None):
    """Open the backup folder (made if missing: it is where backups go) or one backup under it in the Finder."""
    root = backup_root(ctx)
    folder = Path(path).expanduser() if path else root
    if path and saves.read_backup(folder) is None:
        raise TabError(t('That folder is not a backup this builder made.', '这个文件夹不是打包工具做的备份。'))
    if not path:
        folder.mkdir(parents=True, exist_ok=True)
    try:
        done = subprocess.run(['/usr/bin/open', str(folder)], capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise TabError(t(f'The Finder did not open {folder} in time.', f'访达没有及时打开 {folder}。')) from None
    if done.returncode != 0:
        raise TabError(t(f'The Finder could not open {folder}: {done.stderr.strip()}', f'访达打不开 {folder}：{done.stderr.strip()}'))
    return {'opened': str(folder)}


ACTIONS = {'state': state, 'backup': backup, 'plan': plan, 'restore': restore, 'reveal': reveal}
