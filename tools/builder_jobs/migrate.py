"""Is this Mac migrated to MNG Launcher and the one folder rule (docs/install.md "Moving an older install to MNG
Launcher", "Folders")? An AI opens the Launcher only once it is (user 2026-11-12: "用 ai 打开 launcher 第一步要检查
迁移情况，迁移完才能打开launcher"): `mng migrate` says what is left, `mng migrate --apply` does it, `mng open` refuses
until nothing is left.

Migrated means:
- every installed game app records its work folder (MNGWorkFolder), and that folder obeys the Folders rule: outside this
  checkout and outside the folder that holds the app, on the app's disk (its parent there, at least: a deleted work
  folder is made again by the next update);
- MNG Launcher is made, from this checkout, with a window no older than this checkout's tools/builder_app.
A work folder elsewhere that obeys the rule is migrated (it need not be MacNativeGames-work/<game>, the default).

--apply, one app at a time (the Mac's one job, shared.py): a work folder that breaks the rule is renamed to
build_app.default_work's place (MacNativeGames-work/<game> beside the apps folder) when that is on the same disk (instant,
nothing copied), then the express update records it (update_runtime.py --work); an app that records none gets the
express update with that folder. A game whose next update is a full build (Bananza, Pokopia, another version) is not
blocking: its next full build, which needs the game files, puts the work folder there. A stale MNG Launcher is made again."""
import os
from pathlib import Path
import plistlib
import subprocess

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

LAUNCHER = Path(os.environ.get('MNG_LAUNCHER_APP') or Path.home() / 'Applications/MNG Launcher.app')   # tests move it
# the work folder an app built before 31 October 2026 used, next to the app (update_runtime.py's fallback)
LEGACY = {'totk': 'TOTK_build', 'botw': 'BotW_build', 'oot': 'OcarinaOfTime_build'}


def _inside(path, folder):
    path, folder = Path(path).resolve(), Path(folder).resolve()
    return path == folder or folder in path.parents


def _device(path):
    """The disk of path, or of the nearest folder above it that exists."""
    path = Path(path)
    while not path.exists() and path != path.parent:
        path = path.parent
    return os.stat(path).st_dev


def app_state(app, game, kind):
    """{state, reason, work, target, source} for one installed app. state: ok, move (rename into target), record (no
    folder recorded: the express update records target), later (its next build is a full one: not blocking),
    unreachable (recorded on a disk that is not connected)."""
    b = repo.build_app()
    app = Path(app)
    try:
        recorded = plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('MNGWorkFolder')
    except Exception:   # unreadable or not a property list (plistlib raises expat's errors too)
        recorded = None
    target = _rule_place(b, app, game)
    out = {'work': recorded, 'target': str(target), 'source': None}
    full = kind not in ('express', 'fast')
    if not isinstance(recorded, str) or not recorded:
        legacy = app.parent / LEGACY.get(game, '')
        out['source'] = str(legacy) if game in LEGACY and legacy.is_dir() else None
        if full:
            return {**out, 'state': 'later', 'reason': t('records no work folder; its next full build puts it in place',
                                                          '没有记录工作目录；下次完整打包时会放到规定位置')}
        return {**out, 'state': 'move' if out['source'] else 'record',
                'reason': t('records no work folder (built before 31 October 2026)', '没有记录工作目录（2026-10-31 以前打的）')}
    work = Path(recorded)
    if not work.is_dir() and not work.parent.is_dir():
        return {**out, 'state': 'later' if full else 'unreachable',
                'reason': t(f'its work folder {work} is on a disk that is not connected, or gone',
                            f'工作目录 {work} 所在的盘没接上，或已不存在')}
    checkout = repo.REPO
    broken = (t('inside the MacNativeGames checkout', '在 MacNativeGames 源码目录里') if _inside(work, checkout) else
              t('inside the folder that holds the app', '在放 App 的文件夹里') if _inside(work, app.parent) else
              t('on another disk than the app', '和 App 不在同一个盘上') if _device(work) != _device(app) else None)
    if broken is None:
        return {**out, 'state': 'ok', 'reason': t('obeys the folder rule', '符合目录规则')}
    if full:
        return {**out, 'state': 'later', 'reason': t(f'its work folder is {broken}; its next full build moves it',
                                                     f'工作目录{broken}；下次完整打包时会放到规定位置')}
    return {**out, 'state': 'move', 'source': str(work),
            'reason': t(f'its work folder is {broken}', f'工作目录{broken}')}


def _rule_place(b, app, game):
    """build_app.default_work's place for an app that records nothing: MacNativeGames-work/<game> beside its folder."""
    base = app.parent.parent
    if base == Path(base.anchor) or not os.access(base, os.W_OK):
        base = Path.home()
    return base / b.WORK_FOLDERS / game


def launcher_state():
    """{state: ok|make, reason}: MNG Launcher made from this checkout, with a window as new as tools/builder_app here."""
    try:
        info = plistlib.loads((LAUNCHER / 'Contents/Info.plist').read_bytes())
    except Exception:   # not made yet, or unreadable
        return {'state': 'make', 'reason': t('MNG Launcher is not made yet', '还没有生成 MNG Launcher')}
    digest = info.get('MNGSourceArchiveSHA256')
    if digest and (repo.REPO / '.ready').is_file() and (repo.REPO / '.ready').read_text().strip() == digest:
        return {'state': 'ok', 'reason': t('using the Launcher release source', '使用 Launcher 发布版配套源码')}
    if Path(info.get('MNGRepo', '')).resolve() != repo.REPO.resolve():
        return {'state': 'make', 'reason': t(f'MNG Launcher runs another checkout ({info.get("MNGRepo")})',
                                             f'MNG Launcher 用的是另一份源码（{info.get("MNGRepo")}）')}
    commit = info.get('MNGSourceCommit', '')
    same = commit and subprocess.run(['git', '-C', str(repo.REPO), 'diff', '--quiet', commit, 'HEAD', '--',
                                      'tools/builder_app'], capture_output=True).returncode == 0
    if not same:
        return {'state': 'make', 'reason': t('its window is older than this checkout', '它的窗口比这份源码旧')}
    return {'state': 'ok', 'reason': t('made from this checkout', '由这份源码生成')}


def check(apps=None):
    """The whole picture: each app's state, the Launcher's, and whether anything blocks (done)."""
    if apps is None:
        from tools.builder_jobs.installed import apps as installed
        apps = installed()
    items = [{'app': a['app'], 'name': a['name'], 'game': a['game'], 'kind': a['kind'],
              **app_state(a['app'], a['game'], a['kind'])} for a in apps if a.get('game')]
    launcher = launcher_state()
    blocking = [i for i in items if i['state'] in ('move', 'record', 'unreachable')]
    return {'apps': items, 'launcher': launcher, 'done': not blocking and launcher['state'] == 'ok'}


def apply(report, run_update, make_launcher):
    """Do what check() found, one app at a time; a list of (app or 'MNG Launcher', what happened)."""
    done = []
    for item in report['apps']:
        if item['state'] not in ('move', 'record'):
            continue
        target = Path(item['target'])
        if item['state'] == 'move':
            source = Path(item['source'])
            if target.exists():
                done.append((item['app'], t(f'{target} already exists: move or delete it, then run again',
                                            f'{target} 已经存在：先挪开或删掉，再运行一次')))
                continue
            if _device(source) != _device(target.parent):
                done.append((item['app'], t(f'{source} and {target} are on different disks: choose a work folder on the '
                                            f'app\'s disk with mng update --work', f'{source} 和 {target} 不在同一个盘上：用 '
                                            f'mng update --work 选一个和 App 同盘的工作目录')))
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            source.rename(target)   # same disk: instant, nothing copied
        code = run_update(item['app'], str(target))
        done.append((item['app'], t('moved and recorded' if code == 0 else f'the update stopped (exit {code})',
                                    '已搬移并记录' if code == 0 else f'更新停下了（退出码 {code}）')))
    if report['launcher']['state'] == 'make':
        code = make_launcher()
        done.append(('MNG Launcher', t('made' if code == 0 else f'make_app.py stopped (exit {code})',
                                       '已生成' if code == 0 else f'make_app.py 停下了（退出码 {code}）')))
    return done


def make_launcher():
    return subprocess.run(['/usr/bin/python3', str(repo.REPO / 'tools/builder_app/make_app.py'), '-o', str(LAUNCHER)],
                          cwd=repo.REPO).returncode
