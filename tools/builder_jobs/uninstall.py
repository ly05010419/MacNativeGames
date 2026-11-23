"""游戏卸载 on MNG Launcher's Storage page (user 2026-11-20: "添加 一个 游戏卸载 功能 … 在launcher里 存储 里添加按钮 叫
游戏卸载"; the saves kept unless asked, deleted for good, not to the Trash; "不要删除 用户自己的游戏rom").

What goes: the app, the work folder it records (MNGWorkFolder), its game data image (MNGGameDataFolder, made from the
player's game files by the build), the shader cache; with saves=True also the game's whole data folder (saves, settings).
What never goes: the player's own game files and keys. No record says where they are, so every folder is read through
first and one that holds any (ROM_SUFFIXES) is left whole and named in the answer. Nor this checkout, the Launcher's own
folder, another game's data folder, a disk root, the home folder or a system folder.

Each folder is renamed beside itself first (the same disk: instant, and the app is gone from the Launcher at once), then
deleted in a detached process (a work folder holds hundreds of thousands of files: minutes), as work_prune.py does."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import time

# tools.* are imported inside the functions: the detached delete runs this file on its own (python -I uninstall.py --delete)

ROM_SUFFIXES = ('.xci', '.nsp', '.nsz', '.xcz', '.nca', '.tik')   # the player's game files
# the keys by their names only: the pipeline archives hold thousands of "<hash>-8.keys" (their key lists, 2026-11-20)
KEY_NAMES = ('prod.keys', 'title.keys', 'dev.keys')
TRASH = '.mng-uninstall-'


def is_rom(name):
    name = name.lower()
    return name.endswith(ROM_SUFFIXES) or name in KEY_NAMES


def rom_files(path, limit=3):
    """The player's game files or keys inside path (at most limit), or []: a folder holding one is never deleted."""
    found = []
    path = Path(path)
    if path.is_file() or path.is_symlink():
        return [str(path)] if is_rom(path.name) else []
    for here, dirs, names in os.walk(path, followlinks=False):
        for name in names:
            if is_rom(name):
                found.append(os.path.join(here, name))
                if len(found) >= limit:
                    return found
    return found


def app_reason(ctx):
    """Why the app may not be deleted, or None: it must be this game's app bundle (its bundle ID), not a link."""
    app = Path(ctx.app)
    if app.is_symlink() or not app.is_dir() or app.suffix != '.app':
        return 'not_app'
    try:
        bundle = plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier')
    except Exception:
        return 'not_app'
    return None if bundle == ctx.spec.bundle_id else 'not_app'


def folder_reason(ctx, path, saves_too):
    """Why this folder may not be deleted, or None: storage.refuse_reason's guards (no root, home, system folder, link),
    nothing holding this checkout, the Launcher's folder, the app (it goes on its own) or another game's data; the saves
    only when saves_too."""
    from tools.builder import saves, storage
    path = Path(path)
    keep = [ctx.repo, ctx.builder_dir, ctx.app]
    if not saves_too:
        keep.append(saves.profile_dir(ctx.spec, ctx.home))
    # a link (a data folder kept on another disk): the folder it points to is what goes, so that is what is checked
    reason = storage.refuse_reason(path.resolve() if path.is_symlink() else path, ctx.home, keep)
    if reason is not None:
        return reason
    for support in {Path(ctx.home) / 'Library/Application Support/MacNativeGames',
                    (Path(ctx.home) / 'Library/Application Support/MacNativeGames').resolve()}:
        for p in {path.absolute(), path.resolve()}:
            if p == support or p in support.parents:
                return 'protected'
            if support in p.parents and p.relative_to(support).parts[0] != Path(ctx.data_dir).name:
                return 'protected'   # another game's data folder, or the Launcher's own
    return None


def items(ctx, saves_too, work_folder, shader_folder):
    """What 游戏卸载 removes: [(id, label, path)], each an existing path; the data folder (saves too) holds the shader
    cache and the game data image when they are there, so those are not listed twice."""
    from tools.builder_jobs.text import t
    out = [('app', t('The app', 'App'), Path(ctx.app))]
    work, kind = work_folder
    if kind in ('recorded', 'unrecorded') and work.is_dir():
        out.append(('work', t('Build cache (work folder)', '打包缓存（工作文件夹）'), work))
    try:
        data = plistlib.loads((Path(ctx.app) / 'Contents/Info.plist').read_bytes()).get('MNGGameDataFolder')
    except Exception:
        data = None
    data_dir = Path(ctx.data_dir)
    real_data = data_dir.resolve() if data_dir.exists() else None
    def inside_data(p):
        return saves_too and real_data is not None and (p.resolve() == real_data or real_data in p.resolve().parents)
    if data and Path(data).is_dir() and not inside_data(Path(data)):
        out.append(('gamedata', t('Game data image (made from your game files by the build)', '游戏数据镜像（打包时由你的游戏文件生成）'), Path(data)))
    if Path(shader_folder).is_dir() and not inside_data(Path(shader_folder)):
        out.append(('shader', t('Shader cache', '着色器缓存'), Path(shader_folder)))
    if saves_too and data_dir.exists():
        out.append(('data', t('Saves and settings (the whole data folder)', '存档和设置（整个数据文件夹）'), data_dir))
    return out


def detach_delete(paths):
    """Delete these renamed folders in a process of their own (the page answers at once)."""
    if not paths:
        return
    subprocess.Popen([sys.executable, '-I', '-B', __file__, '--delete', *map(str, paths)], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)


def set_aside(path):
    """Rename path beside itself (a link: the folder it points to, then the link goes) and return the new name."""
    path = Path(path)
    if path.is_symlink():
        target = path.resolve()
        aside = target.with_name(f'{TRASH}{time.time_ns()}-{target.name}')
        target.rename(aside)
        path.unlink()
        return aside
    aside = path.with_name(f'{TRASH}{time.time_ns()}-{path.name}')
    path.rename(aside)
    return aside


def _rmtree(root):
    root = str(root)
    if not os.path.basename(root).startswith(TRASH) or os.path.islink(root):
        return   # only what set_aside renamed
    files, dirs = [], []
    for here, subdirs, names in os.walk(root, topdown=False):
        files += [os.path.join(here, n) for n in names]
        files += [os.path.join(here, d) for d in subdirs if os.path.islink(os.path.join(here, d))]
        dirs.append(here)
    from concurrent.futures import ThreadPoolExecutor

    def unlink(p):
        try:
            os.unlink(p)
        except OSError:
            pass
    with ThreadPoolExecutor(8) as pool:
        for _ in pool.map(unlink, files, chunksize=256):
            pass
    for d in dirs:
        try:
            os.rmdir(d)
        except OSError:
            pass
    if os.path.isdir(root):
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__' and len(sys.argv) > 2 and sys.argv[1] == '--delete':
    for p in sys.argv[2:]:
        _rmtree(p)
