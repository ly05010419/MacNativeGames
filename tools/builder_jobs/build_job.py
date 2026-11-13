"""One build: build_app.py with the plan's files, its output turned into events (events.py) for the window.

Contract: command() makes build_app.py's arguments from a request and refuses a plan that blocks. run() starts
build_app.py in a process group of its own, reports each stage (its 'i/4' lines), each step (each log it starts in
the work folder's logs/) and the progress once a second, and ends with exactly one 'done' or 'error' event. A cancel
(SIGINT or SIGTERM to this process) sends the build's group SIGINT, then SIGKILL 10 seconds later. Every line the build
prints is also kept in ~/Library/Logs/MacNativeGames/builder-app-<game>-<date>-<time>.log."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time

from tools.builder import progress as parse
from tools.builder_jobs import history, repo, shared
from tools.builder_jobs.events import Emitter
from tools.builder_jobs.text import t

LOGS = Path.home() / 'Library/Logs/MacNativeGames'   # tests replace it with a temporary folder
_FULL_OUTPUT = re.compile(r'full output: (\S.*\.log)')
_COUNT = re.compile(r'(?<![\d/.])(\d{1,9}) ?/ ?(\d{1,9})(?![\d/.])')   # 'prepared 16590/25787' in a step's log


class Refused(Exception):
    pass


def express_plan(app, python=sys.executable, work=None):
    """update_runtime.py --plan for an installed app (tools/builder_jobs/installed.py asks the same)."""
    from tools.builder_jobs.installed import plan
    return plan(app, python, work) if work else plan(app, python)


def same_version(plan, update_plan):
    """Whether the dropped files are of the installed app's game version, so an update of the app is the same build. Only
    when it is known: a card image without [vNNN] in its name, or files of another version, take the full build."""
    if update_plan.get('game') != plan.get('game'):
        return False
    if plan['game'] == 'oot':   # one version is built (131072), checked by the build itself
        return True
    app_version = repo.build_app().VERSIONS.get(update_plan.get('version') or '', {}).get('version')
    dropped = plan.get('update_version') if plan.get('update') else 0
    return dropped is not None and app_version is not None and str(app_version) == str(dropped)


def command(request, python=sys.executable):
    """(script, arguments, work folder, app path, kind, game) for a request, kind being express, fast or full (the three updates
    of docs/install.md, the fastest that fits; user 2026-11-12: "不要用最慢的形式打包"):
      {update: <app>, shaders: true}                  only its shaders and pipelines, from the newest list: shader_update.py
      {update: <app>, work?}                          an installed app, without files: update_runtime.py [--seed]
                                                      [--work <work>] (a work folder moved on the Storage page;
                                                      the app records it from then on)
      {plan, output_dir, work_root, platform, app?}   dropped files: into app (an installed one of that game) or
                                                      <output_dir>/<name>.app; an update when that app is there, of the
                                                      same version, and an update fits; else build_app.py
    The work folder is the one the app records, else work_root/<game>, else build_app.default_work (the one rule of
    build_app.py, update_runtime.py and the Launcher)."""
    b = repo.build_app()
    if request.get('update') and request.get('shaders'):   # only the shaders and pipelines (shader_update.py)
        app = Path(request['update']).expanduser()
        up = express_plan(app, python)
        if not up.get('work'):
            raise Refused(up.get('reason') or t('the app records no work folder', '这个 App 没有记录工作文件夹'))
        return repo.SHADER_UPDATE, [str(app)], Path(up['work']), app, 'shaders', up.get('game')
    if request.get('update'):
        app = Path(request['update']).expanduser()
        given = request.get('work')
        up = express_plan(app, python, given)
        if up['kind'] == 'full':
            raise Refused(t(f'this app needs a full build: {up["reason"]}. Drop its game files in step 1',
                            f'这个 App 要完整打包：{up["reason"]}。请在第一步拖入它的游戏文件'))
        if up['kind'] not in ('express', 'fast'):
            raise Refused(up['reason'])
        argv = [str(app)] + (['--seed'] if up['kind'] == 'fast' else []) + (['--work', str(given)] if given else [])
        return repo.UPDATE_RUNTIME, argv, Path(up['work']), app, up['kind'], up.get('game')
    plan = request['plan']
    if plan.get('blocks'):
        raise Refused(plan['blocks'][0])
    if not plan.get('game') or not plan.get('base'):
        raise Refused(t('no game to build', '没有要打包的游戏'))
    platform = request.get('platform', 'macos')
    if platform != 'macos':
        raise Refused(t('this builder makes Mac apps so far', '这个打包器目前只打 Mac 包'))
    out = Path(request['app']).expanduser() if request.get('app') else \
        Path(request['output_dir']).expanduser() / f'{repo.GAMES[plan["game"]]}.app'
    work_root = (request.get('work_root') or '').strip()
    work = b.default_work(out, plan['game'])[0] if out.is_dir() or not work_root else Path(work_root).expanduser() / plan['game']
    if out.is_dir():
        up = express_plan(out, python)
        if up['kind'] in ('express', 'fast') and same_version(plan, up):
            return (repo.UPDATE_RUNTIME, [str(out)] + (['--seed'] if up['kind'] == 'fast' else []), Path(up['work']), out,
                    up['kind'], plan['game'])
    argv = [plan['base']]
    if plan.get('keys') and plan['game'] in ('totk', 'botw'):
        argv += ['-k', plan['keys']]
    if plan.get('update'):
        argv += ['--update', plan['update']]
    for dlc in plan.get('dlc') or []:
        argv += ['--dlc', dlc]
    if plan.get('firmware'):
        argv += ['--firmware', plan['firmware']]
    elif plan.get('fonts'):
        argv += ['--shared-fonts', plan['fonts']]
    argv += ['--work', str(work), '-o', str(out)]
    return repo.BUILD_APP, argv, work, out, 'full', plan['game']


class Steps:
    """The logs build_app.py starts in <work>/logs after `since`, in the order they start."""

    def __init__(self, logs, since):
        self.logs, self.since, self.seen = Path(logs), since, {}

    def poll(self, now):
        try:
            entries = list(os.scandir(self.logs))
        except OSError:
            return self.ordered()
        for e in entries:
            if e.name.endswith('.log') and e.name not in self.seen:
                try:
                    if e.stat().st_mtime >= self.since:
                        self.seen[e.name] = now
                except OSError:
                    pass
        return self.ordered()

    def ordered(self):
        return [(n[:-4], t) for n, t in sorted(self.seen.items(), key=lambda kv: kv[1])]

    def count(self, name):
        """(done, total) of the last 'N/M' counter in the end of a step's log, or None: the tools that run long
        (translation, shader libraries, pipelines) print one as they go."""
        try:
            with open(self.logs / f'{name}.log', 'rb') as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 4096))
                tail = f.read().decode(errors='replace')
        except OSError:
            return None
        for m in reversed(list(_COUNT.finditer(tail))):
            done, total = int(m.group(1)), int(m.group(2))
            if 2 <= total and done <= total:
                return done, total
        return None


def run(request, emit, history_dir, python=sys.executable, script=None, record=None):
    chosen, argv, work, out, kind, game = command(request, python)
    if record is not None:   # the shared record names the job for a window that did not start it (shared.py)
        shared.describe(record, game=game, app=str(out), kind=kind)
    home = Path(os.environ.get('MNG_HOME') or Path.home())
    record_mods = None
    if kind == 'full' and script is None:   # a real build (tests pass a script of their own): the Mods page's code patches
        from tools.builder_jobs import code_mods
        extra, record_mods = code_mods.compose(request.get('plan') or {}, home)
        if extra:
            at = argv.index('--work')
            argv = argv[:at] + extra + argv[at:]
    # each kind of update has its own measured history: an express update takes minutes, a full build much longer
    record = history.record_path(history_dir, game, 'macos' if kind == 'full' else f'macos-{kind}')
    estimator = history.Estimator(history.load(record))
    start = time.time()
    LOGS.mkdir(parents=True, exist_ok=True)
    keep = LOGS / f'builder-app-{game}-{time.strftime("%Y%m%d-%H%M%S")}.log'
    script = script or chosen
    emit('plan', game=game, app=str(out), work=str(work), log=str(keep), measured=estimator.last is not None,
         command=[str(script), *argv], kind=kind)
    proc = subprocess.Popen([python, '-u', str(script), *argv], cwd=repo.REPO, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', start_new_session=True)
    cancelled = threading.Event()

    def cancel(*_):
        if cancelled.is_set() or proc.poll() is not None:
            return
        cancelled.set()
        emit('cancelling')
        try:
            os.killpg(proc.pid, signal.SIGINT)
        except ProcessLookupError:
            return
        def kill():
            try:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:   # it ended between the poll and the signal
                pass
        threading.Timer(10, kill).start()
    previous = {s: signal.signal(s, cancel) for s in (signal.SIGINT, signal.SIGTERM)}
    steps = Steps(work / 'logs', start - 1)
    state = {'stage': 1, 'step': None, 'reported': 0, 'done': False}

    def new_steps(now):
        """Report every step started since the last call, in order; the steps of one poll can be several."""
        seen = steps.poll(now)
        for i in range(state['reported'], len(seen)):
            emit('step', name=seen[i][0], index=i + 1)
        state['reported'] = len(seen)
        state['step'] = seen[-1][0] if seen else None
        return seen

    def tick():
        last = 0
        while not state['done']:
            now = time.time()
            seen = new_steps(now)
            if now - last >= 1:
                last = now
                since = now - seen[-1][1] if seen else 0
                fraction, left, measured = estimator.update(state['step'], since, state['stage'])
                counted = steps.count(state['step']) if state['step'] else None
                emit('progress', fraction=round(fraction, 4), left=None if left is None else round(left),
                     elapsed=round(now - start), measured=measured,
                     step_done=counted and counted[0], step_total=counted and counted[1])
            time.sleep(0.25)
    ticker = threading.Thread(target=tick, daemon=True)
    ticker.start()
    error, failed_log = [], None
    try:
        with keep.open('w') as log:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                event = parse.parse_line(line)
                if isinstance(event, parse.Stage):
                    state['stage'] = event.index
                    emit('stage', index=event.index, total=event.total, name=event.name, text=event.text)
                elif isinstance(event, parse.Failure) or error:
                    error.append(event.text if isinstance(event, parse.Failure) else line.rstrip('\n'))
                    found = _FULL_OUTPUT.search(line)
                    failed_log = failed_log or (found and found.group(1))
                else:
                    emit('log', text=getattr(event, 'text', line.rstrip('\n')))
        code = proc.wait()
    finally:
        state['done'] = True
        ticker.join(timeout=3)
        new_steps(time.time())   # a step that started after the ticker's last look
        for s, h in previous.items():
            signal.signal(s, h)
    end = time.time()
    if code == 0:
        history.record(record, steps.poll(end), end)
        if record_mods:
            record_mods(out)
        emit('progress', fraction=1.0, left=0, elapsed=round(end - start), measured=True)
        emit('done', ok=True, app=str(out), elapsed=round(end - start), note=mods_note(game, out, True, ''))
        return 0
    message = error[0] if error else (t(f'the build was cancelled (exit {code})', f'打包已停止（退出码 {code}）')
                                      if cancelled.is_set() else
                                      t(f'{Path(script).name} stopped (exit {code})', f'{Path(script).name} 停下了（退出码 {code}）'))
    emit('done', ok=False, cancelled=cancelled.is_set(), message=message, detail='\n'.join(error[1:])[-6000:],
         step_log=failed_log, log=str(keep), elapsed=round(end - start),
         note=mods_note(game, out, False, '\n'.join(error)) if kind == 'full' else '')
    return code or 1


def mods_note(game, app, ok, error):
    """The Mods page's line after a build of this app (tab_mods.after_build): a code patch for another build stopped it,
    or the applied mods no longer fit the new app; '' for other games or when there is nothing to say."""
    if game not in ('totk', 'botw') or not Path(app).is_dir():
        return ''
    from tools.builder_jobs.tabs import call
    answer = call('mods', 'after_build', app, {'ok': ok, 'error': error}, os.environ.get('MNG_HOME'))
    return (answer.get('result') or {}).get('note', '') if answer.get('ok') else ''


def start(request, stream, history_dir, source=None):
    """Run one request as the Mac's one job (shared.claim), its events into stream and current.ndjson; the exit code.
    source says who started it, for whoever else looks (MNG_JOB_SOURCE: the window sets launcher:<pid>)."""
    source = source or os.environ.get('MNG_JOB_SOURCE') or 'the command line'
    try:
        with shared.claim(stream, source) as (tee, record):
            try:
                return run(request, Emitter(tee), history_dir, record=record)
            except (Refused, OSError, ValueError, KeyError) as e:
                Emitter(tee)('done', ok=False, cancelled=False, message=str(e), detail='', step_log=None, log=None, elapsed=0)
                return 2
    except shared.Busy as e:
        Emitter(stream)('done', ok=False, cancelled=False, message=str(e), detail='', step_log=None, log=None, elapsed=0, busy=True)
        return 3


def main(request_path, history_dir):
    try:
        request = json.loads(Path(request_path).read_text())
    except (OSError, ValueError) as e:
        Emitter(sys.stdout)('done', ok=False, cancelled=False, message=str(e), detail='', step_log=None, log=None, elapsed=0)
        return 2
    return start(request, sys.stdout, history_dir)
