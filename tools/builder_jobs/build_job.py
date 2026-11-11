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
from tools.builder_jobs import history, repo
from tools.builder_jobs.events import Emitter
from tools.builder_jobs.text import t

LOGS = Path.home() / 'Library/Logs/MacNativeGames'   # tests replace it with a temporary folder
_FULL_OUTPUT = re.compile(r'full output: (\S.*\.log)')
_COUNT = re.compile(r'(?<![\d/.])(\d{1,9}) ?/ ?(\d{1,9})(?![\d/.])')   # 'prepared 16590/25787' in a step's log


class Refused(Exception):
    pass


def command(request):
    """(build_app.py arguments, work folder, app path) for a request {plan, output_dir, work_root, platform}."""
    plan = request['plan']
    if plan.get('blocks'):
        raise Refused(plan['blocks'][0])
    if not plan.get('game') or not plan.get('base'):
        raise Refused(t('no game to build', '没有要打包的游戏'))
    platform = request.get('platform', 'macos')
    if platform != 'macos':
        raise Refused(t('this builder makes Mac apps so far', '这个打包器目前只打 Mac 包'))
    out = Path(request['output_dir']).expanduser() / f'{repo.GAMES[plan["game"]]}.app'
    work = Path(request['work_root']).expanduser() / plan['game']
    if out.is_dir():   # an app built before: its own work folder, so the build reuses its files (express update rule)
        try:
            work, _ = repo.build_app().recorded_work_folder(out, plan['game'])
        except repo.build_app().BuildError:
            pass
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
    return argv, work, out


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


def run(request, emit, history_dir, python=sys.executable, script=None):
    argv, work, out = command(request)
    game = request['plan']['game']
    record = history.record_path(history_dir, game, 'macos')
    estimator = history.Estimator(history.load(record))
    start = time.time()
    LOGS.mkdir(parents=True, exist_ok=True)
    keep = LOGS / f'builder-app-{game}-{time.strftime("%Y%m%d-%H%M%S")}.log'
    script = script or repo.BUILD_APP
    emit('plan', game=game, app=str(out), work=str(work), log=str(keep), measured=estimator.last is not None,
         command=[str(script), *argv])
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
        emit('progress', fraction=1.0, left=0, elapsed=round(end - start), measured=True)
        emit('done', ok=True, app=str(out), elapsed=round(end - start))
        return 0
    message = error[0] if error else (t(f'the build was cancelled (exit {code})', f'打包已停止（退出码 {code}）')
                                      if cancelled.is_set() else
                                      t(f'build_app.py stopped (exit {code})', f'build_app.py 停下了（退出码 {code}）'))
    emit('done', ok=False, cancelled=cancelled.is_set(), message=message, detail='\n'.join(error[1:])[-6000:],
         step_log=failed_log, log=str(keep), elapsed=round(end - start))
    return code or 1


def main(request_path, history_dir):
    emit = Emitter(sys.stdout)
    try:
        request = json.loads(Path(request_path).read_text())
        return run(request, emit, history_dir)
    except (Refused, OSError, ValueError, KeyError) as e:
        emit('done', ok=False, cancelled=False, message=str(e), detail='', step_log=None, log=None, elapsed=0)
        return 2
