"""The engine of `tools/mng.py`: MNG Launcher's jobs in words, for a player in a terminal or the AI helping them (user
2026-11-12: "纯用ai打包 也和launcher 能统一使用方法 或者调用launcher的服务"). The same rules as the window, from the same
code: what the files are (classify), what this Mac needs (checks), the games installed and the update each needs (apps),
a build or update as the Mac's one job (build_job.start, so the window shows its progress and can stop it), and the
pages (tabs). Nothing here decides anything the window does not.

  migrate [--apply]            is this Mac migrated to MNG Launcher and the folder rule? --apply does what is left
  status                       the games on this Mac and the update each needs
  check [FILES…]               what this Mac still needs; with files, what they are and the build they make
  build FILES… [--keys K]      build or update the game of these files the fastest way that fits
  update GAME|APP [--work W]   update an installed game without files (express, else fast)
  page TAB ACTION --game GAME|--app APP [--args JSON]   one page action (settings, mods, controls, storage, saves)
  open [FILES…]                open MNG Launcher, with these files filled in (only once migrate says done)
Add --json for the raw JSON the window reads."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from tools.builder_jobs import build_job, repo
from tools.builder_jobs.text import t

HISTORY = Path.home() / 'Library/Application Support/MacNativeGames/Builder/history'
GAME_NAMES = {**repo.GAMES}


def installed(apps_dir=None):
    from tools.builder_jobs.installed import apps
    return apps(apps_dir or Path.home() / 'Applications')


def find_app(name, apps_dir=None):
    """An installed app by its path, its game id (totk, botw, oot, dkb, pkp) or its name."""
    if name.endswith('.app') and Path(name).expanduser().is_dir():
        return str(Path(name).expanduser().resolve())
    found = [a for a in installed(apps_dir) if name.lower() in (a['game'] or '', a['name'].lower())]
    if len(found) != 1:
        raise SystemExit(t(f'{name}: {len(found)} installed apps match; give the app\'s path',
                           f'{name}：匹配到 {len(found)} 个已装的 App，请给出 App 的路径'))
    return found[0]['app']


class Words:
    """A stream build_job writes its events into, printed as lines a person (or an AI reading the output) follows: the
    plan, each stage and step, every log line, the progress once a minute, and the end."""

    def __init__(self, raw=False):
        self.raw, self.last = raw, 0.0

    def write(self, text):
        for line in text.splitlines():
            if self.raw:
                print(line, flush=True)
                continue
            try:
                e = json.loads(line)
            except ValueError:
                print(line, flush=True)
                continue
            self.say(e)

    def flush(self):
        sys.stdout.flush()

    def say(self, e):
        kind = e.get('event')
        if kind == 'plan':
            how = {'express': t('express update', '极速更新'), 'fast': t('fast update', '快速更新'),
                   'full': t('full build', '完整打包')}.get(e.get('kind'), e.get('kind'))
            print(t(f'{how} of {e["app"]}; work folder {e["work"]}; log {e["log"]}',
                    f'{how}：{e["app"]}；工作目录 {e["work"]}；日志 {e["log"]}'), flush=True)
        elif kind == 'stage':
            print(f'== {e.get("index")}/{e.get("total")} {e.get("name")}: {e.get("text")}', flush=True)
        elif kind == 'step':
            print(f'-- {e.get("name")}', flush=True)
        elif kind == 'log':
            print(e.get('text', ''), flush=True)
        elif kind == 'progress' and time.time() - self.last >= 60:
            self.last = time.time()
            left = e.get('left')
            count = f' {e["step_done"]}/{e["step_total"]}' if e.get('step_total') else ''
            print(t(f'.. {round(100 * e.get("fraction", 0))}%{count}, {e.get("elapsed", 0)} s'
                    + (f', about {round(left / 60)} min left' if left else ''),
                    f'.. {round(100 * e.get("fraction", 0))}%{count}，已用 {e.get("elapsed", 0)} 秒'
                    + (f'，约剩 {round(left / 60)} 分钟' if left else '')), flush=True)
        elif kind == 'cancelling':
            print(t('stopping…', '正在停止…'), flush=True)
        elif kind == 'done':
            if e.get('ok'):
                print(t(f'done in {e.get("elapsed")} s: {e.get("app")}', f'完成，用时 {e.get("elapsed")} 秒：{e.get("app")}'), flush=True)
            else:
                print(t('stopped: ', '停下了：') + (e.get('message') or ''), flush=True)
                if e.get('detail'):
                    print(e['detail'], flush=True)
                if e.get('step_log') or e.get('log'):
                    print(t('log: ', '日志：') + (e.get('step_log') or e.get('log')), flush=True)
            if e.get('note'):
                print(e['note'], flush=True)


def show(obj, raw):
    print(json.dumps(obj, ensure_ascii=False, indent=None if raw else 2))


def cmd_status(a):
    apps = installed(a.apps_dir)
    if a.json:
        return show({'apps': apps}, True) or 0
    if not apps:
        print(t('No game app on this Mac yet.', '这台 Mac 上还没有游戏 App。'))
    for x in apps:
        print(f'{x["name"]} {x["version"] or ""}: {x["kind"]} — {x["reason"]}\n    {x["app"]}\n    work: {x["work"]}')
    from tools.builder_jobs import shared
    running = shared.current()
    if running:
        print(t(f'Running now: {running.get("kind")} of {running.get("app")} (from {running.get("source")})',
                f'正在进行：{running.get("app")} 的 {running.get("kind")}（从 {running.get("source")} 开始）'))
    return 0


def cmd_check(a):
    from tools.builder_jobs.checks import checks
    from tools.builder_jobs.classify import report
    out = {'checks': checks(str(Path.home()))}
    if a.files:
        out['files'] = report(a.files + ([a.keys] if a.keys else []))
    if a.json:
        return show(out, True) or 0
    for c in out['checks']:
        print(('ok  ' if c['ok'] else 'NO  ') + c['text'] + ('' if c['ok'] or not c.get('fix') else f'\n    fix: {c["fix"]}'))
    if a.files:
        p = out['files']['plan']
        for i in out['files']['items']:
            print(f'{i["role"]:9} {i["path"]}' + (f'  ({i["note"]})' if i['note'] else ''))
        print(t(f'game: {p.get("app_name")}', f'游戏：{p.get("app_name")}'))
        for b in p['blocks']:
            print('BLOCK ' + b)
        for n in p['notes']:
            print('note  ' + n)
    return 0


def cmd_build(a):
    from tools.builder_jobs.checks import checks
    from tools.builder_jobs.classify import report
    plan = report(a.files + ([a.keys] if a.keys else []))['plan']
    blocking = plan['blocks'] + [c['text'] for c in checks(str(Path.home())) if not c['ok'] and c['id'] != 'disk']
    if blocking:
        for b in blocking:
            print('BLOCK ' + b)
        return 2
    there = next((x['app'] for x in installed(a.apps_dir) if x['game'] == plan.get('game')), None)
    request = {'plan': plan, 'output_dir': str(Path(a.apps_dir or Path.home() / 'Applications').expanduser()),
               'work_root': a.work_root or '', 'platform': 'macos', 'app': a.app or there}
    return build_job.start(request, Words(a.json), HISTORY, source=a.source)


def cmd_update(a):
    request = {'update': find_app(a.target, a.apps_dir)}
    if a.work:
        request['work'] = str(Path(a.work).expanduser())
    return build_job.start(request, Words(a.json), HISTORY, source=a.source)


def cmd_page(a):
    from tools.builder_jobs.tabs import call
    app = find_app(a.app or a.game, a.apps_dir)
    answer = call(a.tab, a.action, app, json.loads(a.args) if a.args else {}, os.environ.get('MNG_HOME'))
    show(answer, a.json)
    return 0 if answer.get('ok') else 1


STATES = {'ok': t('ok', '已迁移'), 'move': t('to move', '要搬移'), 'record': t('to record', '要记录'),
          'later': t('at its next full build', '下次完整打包时'), 'unreachable': t('disk missing', '盘没接上'),
          'make': t('to make', '要生成')}


def print_migration(report):
    for i in report['apps']:
        print(f'[{STATES.get(i["state"], i["state"])}] {i["name"]}: {i["reason"]}\n    {i["app"]}'
              + (f'\n    work: {i["work"]}' if i['work'] else '')
              + (f'\n    -> {i["target"]}' if i['state'] in ('move', 'record') else ''))
    l = report['launcher']
    print(f'[{STATES.get(l["state"], l["state"])}] MNG Launcher: {l["reason"]}')
    print(t('Migrated: MNG Launcher can be opened.', '迁移完成：可以打开 MNG Launcher。') if report['done'] else
          t('Not migrated yet: python3 tools/mng.py migrate --apply', '还没迁移完：运行 python3 tools/mng.py migrate --apply'))


def cmd_migrate(a):
    from tools.builder_jobs import migrate
    report = migrate.check(installed(a.apps_dir))
    if a.json and not a.apply:
        return show(report, True) or (0 if report['done'] else 1)
    print_migration(report)
    if not a.apply or report['done']:
        return 0 if report['done'] else 1
    def run_update(app, work):
        print(t(f'== recording {work} in {app} (express update)', f'== 把 {work} 记进 {app}（极速更新）'), flush=True)
        return build_job.start({'update': app, 'work': work}, Words(a.json), HISTORY, source=a.source)
    for what, result in migrate.apply(report, run_update, migrate.make_launcher):
        print(f'{what}: {result}')
    after = migrate.check(installed(a.apps_dir))
    print_migration(after)
    return 0 if after['done'] else 1


def cmd_open(a):
    from tools.builder_jobs import migrate
    report = migrate.check(installed(a.apps_dir))
    if not report['done']:   # an AI opens the Launcher only on a migrated Mac (user 2026-11-12)
        print_migration(report)
        return 1
    app = Path.home() / 'Applications/MNG Launcher.app'
    if not app.is_dir():
        raise SystemExit(t('MNG Launcher is not made yet: python3 tools/builder_app/make_app.py',
                           '还没有 MNG Launcher：先运行 python3 tools/builder_app/make_app.py'))
    return subprocess.run(['/usr/bin/open', '-a', str(app), *[str(Path(f).expanduser()) for f in a.files]]).returncode


def main(argv):
    ap = argparse.ArgumentParser(prog='mng', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json', action='store_true', help='print the JSON the window reads')
    ap.add_argument('--apps-dir', help='where new apps go and installed ones are also looked for (default ~/Applications)')
    ap.add_argument('--source', default=os.environ.get('MNG_JOB_SOURCE') or 'mng (terminal or AI)',
                    help='who starts the job, as MNG Launcher shows it')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status')
    m = sub.add_parser('migrate'); m.add_argument('--apply', action='store_true', help='do what is left')
    c = sub.add_parser('check'); c.add_argument('files', nargs='*'); c.add_argument('--keys')
    b = sub.add_parser('build'); b.add_argument('files', nargs='+'); b.add_argument('--keys')
    b.add_argument('--app', help='build into this app (default: the installed app of that game, else the apps folder)')
    b.add_argument('--work-root', help='work folders root (default: the one folder rule, build_app.default_work)')
    u = sub.add_parser('update'); u.add_argument('target', help='game id (totk, botw, oot), name or app path')
    u.add_argument('--work', help='a work folder moved elsewhere: the app records it from then on')
    p = sub.add_parser('page'); p.add_argument('tab'); p.add_argument('action')
    p.add_argument('--game'); p.add_argument('--app'); p.add_argument('--args')
    o = sub.add_parser('open'); o.add_argument('files', nargs='*')
    a = ap.parse_args(argv)
    if a.cmd == 'page' and not (a.game or a.app):
        ap.error('page needs --game or --app')
    return {'migrate': cmd_migrate, 'status': cmd_status, 'check': cmd_check, 'build': cmd_build, 'update': cmd_update, 'page': cmd_page,
            'open': cmd_open}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
