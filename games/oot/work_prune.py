"""What a finished build leaves in the work folder that no later build reads (user 2026-11-17: "每次 更新完 要清理 … 不影响
极速更新"). On 2026-11-17 the work folder held 256 GB, about 190 GB of it this kind:

- build/ns2-seed-cache/<version>/<context>: one folder of translations per translation context (chain, depth mode and
  translator; prepare_ns2_seed.py translation_context). A new translator starts a new folder and never reads the old
  one again: 13 old folders, about 116 GB. The contexts the seeds name (shader-seed*/<version>/seed-manifest.json)
  stay.
- build/shader-bundles/*.previous-* and build/shader-libs*.previous-*: what shader_bundle and build_shader_libs.py
  move aside before putting the new one in place, never read again: 14 bundles, about 59 GB, and a 1.4 GB library
  folder.
- build/compute-pipelines/oot-native-*: the pipeline-archive folders named after a chain from before pipeline_cache
  kept one folder across chains ('archive', 'archive-title'). Once that folder exists the chain-named ones of its
  kind are never read; without 'archive-title' the newest '-title' one stays (pipeline_cache takes it over).

Everything else stays: the current translations, seed, libraries, pipeline archive, game data bundles, the extracted
game files and the rewritten code are what an express update reuses.

Removing them takes long (a translation folder holds about 300,000 files; deleting two took 7 minutes on 2026-11-17, and
the update sat at 99 % meanwhile), so prune() only renames them into build/.trash-<time> on the same volume, which is
instant, and a detached process deletes that folder; a trash folder left by an interrupted delete is removed next time.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time

TRASH = '.trash-'


def _seed_contexts(build):
    """Translation contexts the seeds of this work folder were made with, by version folder name."""
    keep = {}
    for manifest in build.glob('shader-seed*/*/seed-manifest.json'):
        try:
            context = json.loads(manifest.read_text()).get('translation_context_sha256')
        except (OSError, ValueError):
            continue
        if context:
            keep.setdefault(manifest.parent.name, set()).add(context)
    return keep


def superseded(tree):
    """Paths under tree/build no later build reads. Nothing outside tree/build, no symbolic links."""
    build = Path(tree) / 'build'
    found = []
    contexts = _seed_contexts(build)
    for version in sorted((build / 'ns2-seed-cache').glob('*')):
        keep = contexts.get(version.name)
        if not keep or not version.is_dir() or version.is_symlink():
            continue   # no seed names a context for it: leave it rather than guess
        found += [p for p in sorted(version.iterdir()) if p.is_dir() and not p.is_symlink() and p.name not in keep]
    found += sorted(p for p in (build / 'shader-bundles').glob('*.previous-*') if p.is_dir() and not p.is_symlink())
    found += sorted(p for p in build.glob('shader-libs*.previous-*') if p.is_dir() and not p.is_symlink())
    compute = build / 'compute-pipelines'
    for suffix in ('', '-title'):
        chain = [p for p in compute.glob('oot-native-*') if p.is_dir() and not p.is_symlink() and p.name.endswith('-title') == bool(suffix)]
        if (compute / ('archive' + suffix)).is_dir():
            found += sorted(chain)
        elif len(chain) > 1:   # pipeline_cache takes over the newest one; the older ones are never read
            newest = max(chain, key=lambda p: ((p / 'pipelines').stat().st_mtime if (p / 'pipelines').exists() else 0, p.name))
            found += sorted(p for p in chain if p != newest)
    return found


def trash_folders(tree):
    """Trash folders a previous prune left (its delete was interrupted or is still running)."""
    return sorted(p for p in (Path(tree) / 'build').glob(TRASH + '*') if p.is_dir() and not p.is_symlink())


def prune(tree, log=print, delete=True):
    """Move what superseded() names into one trash folder and delete it in a detached process (delete=False leaves it,
    for tests). Returns the moved paths (their old locations)."""
    build = (Path(tree) / 'build').resolve()
    moved = []
    found = superseded(tree)
    if found:
        trash = build / f'{TRASH}{time.time_ns()}'
        trash.mkdir()
        for i, path in enumerate(found):
            if build not in path.resolve().parents:
                raise ValueError(f'refusing to remove {path}: not inside {build}')
            path.rename(trash / f'{i:04d}-{path.name}')   # same volume: instant
            moved.append(path)
        log(f'work folder: {len(moved)} superseded folders no later build reads moved out to be deleted: '
            + ', '.join(str(p.relative_to(Path(tree))) for p in moved[:6]) + (' …' if len(moved) > 6 else ''))
    left = trash_folders(tree)
    detached = dict(start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    if left and delete:
        # Detached, so the build returns now; it deletes every trash folder there is, the ones left last time too.
        subprocess.Popen([sys.executable, '-I', '-B', __file__, '--delete', *map(str, left)], **detached)
    if delete and old_spv(tree, limit=1):
        subprocess.Popen([sys.executable, '-I', '-B', __file__, '--delete-spv', str(tree)], **detached)
    return moved


def _unlink_all(files, threads=8):
    """Delete files on several threads: one unlink at a time waits on the drive for each of hundreds of thousands of small
    files (one thread deleted 25 folders in about 40 minutes on the external drive, 2026-11-17); os.unlink lets the GIL go."""
    from concurrent.futures import ThreadPoolExecutor

    def one(path):
        try:
            os.unlink(path)
        except OSError:
            pass
    with ThreadPoolExecutor(threads) as pool:
        for _ in pool.map(one, files, chunksize=256):
            pass


def _rmtree_parallel(root):
    files, dirs = [], []
    for here, subdirs, names in os.walk(root, topdown=False):   # bottom-up: a folder after its contents
        files += [os.path.join(here, n) for n in names]
        files += [os.path.join(here, d) for d in subdirs if os.path.islink(os.path.join(here, d))]   # links are removed, never followed
        dirs.append(here)
    _unlink_all(files)
    for d in dirs:
        try:
            os.rmdir(d)
        except OSError:
            pass
    if os.path.isdir(root):
        shutil.rmtree(root, ignore_errors=True)   # anything left (made meanwhile, or not removable above)


def delete_trash(paths):
    for p in map(Path, paths):
        if p.name.startswith(TRASH) and p.is_dir() and not p.is_symlink():
            _rmtree_parallel(str(p))


def old_spv(tree, older_than_s=600, limit=None):
    """The SPIR-V intermediates left in the translation folders that stay (runtime translations stop keeping them since
    2026-11-17, totk_shader_rt.m translate): nothing reads them afterwards. Only files older than older_than_s, so a
    translation in progress keeps its own until spirv-cross has read it."""
    build = Path(tree) / 'build'
    cutoff = time.time() - older_than_s
    found = []
    for version in (build / 'ns2-seed-cache').glob('*'):
        for context in version.glob('*'):
            if not context.is_dir() or context.is_symlink():
                continue
            with os.scandir(context) as it:
                for e in it:
                    if e.name.endswith('.spv') and e.is_file(follow_symlinks=False) and e.stat(follow_symlinks=False).st_mtime < cutoff:
                        found.append(e.path)
                        if limit and len(found) >= limit:
                            return found
    return found


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--delete':
        os.nice(10)
        delete_trash(sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == '--delete-spv':
        os.nice(10)
        _unlink_all(old_spv(sys.argv[2]))
    elif len(sys.argv) == 2:
        prune(sys.argv[1])
    else:
        sys.exit('usage: work_prune.py <work folder>/oot | --delete <trash folder> ... | --delete-spv <work folder>/oot')
