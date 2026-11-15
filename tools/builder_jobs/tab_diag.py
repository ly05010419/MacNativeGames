"""MNG Launcher's 诊断模式 (user 2026-11-13: "诊断模式 就是 快速模式加上所有诊断", "方便检查 渲染等等问题"; Start game itself stays
without any log or debug switch: tests/test_launcher_play_clean.py). The game starts with the switches a debug session
of this Mac carries (keep/tools/oot-debug/play.sh: the game log, long-frame detail, the per-frame record, wait
statistics, the per-pipeline log) and a run folder of its own; while it runs, the card sends it a few commands through
its input file: a screenshot, a pass record, a shader left out for a before/after picture, a normal close.

A command the game does not take ends the game (runtime/input.c runtime_fail), so only these are written, each checked
here first. The input file is the game's own (<data>/input.txt, which its launcher empties at each start): lines are
appended, never written over (the game reads on from where it was).

Actions: start() -> the run folder and the switches; command(what, key) -> the line sent; state() -> running, the run
folder, its pictures, the game log; reveal() -> the run folder in the Finder."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

from tools.builder import game_state
from tools.builder_jobs.tabs import TabError
from tools.builder_jobs.text import t

LOGS = {'totk': 'totk', 'botw': 'botw', 'oot': 'ocarina'}   # runtime/game_version.h GAME_LOG
OPEN_S = 30
SHADER = re.compile(r'[0-9a-f]{16}')   # nvn_mtl.m dm_gfx_set_flag: skipfs_<the first 16 hex of the fragment shader>


def switches(run):
    """The diagnostic environment (play.sh's): the run's per-frame record goes into its folder."""
    return {'TOTK_DEBUG_LOG': '1', 'TOTK_LONGFRAME_DETAIL': '1', 'TOTK_SVC_STATS': '1', 'TOTK_SURVEY_SHADER_LOG': '1',
            'DM_NVN_TSV': str(Path(run) / 'frames.tsv'), 'DM_NVN_UPTIME': '2', 'TOTK_SLOT_PURPOSE': 'diagnostic mode'}


def runs_dir(ctx):
    return ctx.home / 'Library/Logs/MacNativeGames/diagnostics'


def record_path(ctx):
    """The game's current diagnostic run (its folder and start), kept beside the Launcher's own files."""
    return ctx.builder_dir / f'diagnostic-{ctx.spec.id}.json'


def current(ctx):
    try:
        record = json.loads(record_path(ctx).read_text())
        return record if Path(record.get('run', '')).is_dir() else None
    except (OSError, ValueError):
        return None


def game_log(ctx):
    return ctx.home / 'Library/Logs/MacNativeGames' / f'{LOGS[ctx.spec.id]}.log'


def start(ctx, runner=subprocess.run):
    if game_state.game_running(ctx.data_dir):
        raise TabError(t('the game is running: close it first', '游戏正在运行：请先关掉'))
    run = runs_dir(ctx) / f'{ctx.spec.id}-{time.strftime("%Y%m%d-%H%M%S")}'
    run.mkdir(parents=True)
    env = switches(run)
    argv = ['/usr/bin/open'] + [a for k, v in sorted(env.items()) for a in ('--env', f'{k}={v}')] + [str(ctx.app)]
    done = runner(argv, capture_output=True, text=True, timeout=OPEN_S)
    if done.returncode:
        raise TabError(t('the game did not start: ', '游戏没有启动：') + (done.stderr or '').strip()[-300:])
    record = {'run': str(run), 'started': time.time(), 'app': str(ctx.app), 'env': env, 'shots': 0}
    (run / 'run.json').write_text(json.dumps(record, indent=1, ensure_ascii=False) + '\n')
    ctx.builder_dir.mkdir(parents=True, exist_ok=True)
    record_path(ctx).write_text(json.dumps(record, ensure_ascii=False) + '\n')
    return {'run': str(run), 'env': env}


def line_for(what, run, shot, key=''):
    """The input-file line of a command, checked: anything else ends the game."""
    if what == 'shot':
        return f'wshot {Path(run) / f"shot-{shot:03d}.png"}'
    if what == 'passdump':
        return 'passdump 1'
    if what in ('skip', 'unskip'):
        if not SHADER.fullmatch(key or ''):
            raise TabError(t('a shader is the first 16 hex digits of its key (0-9, a-f)', '着色器写它的键的前 16 位十六进制（0-9、a-f）'))
        return f'flag skipfs_{key} {1 if what == "skip" else 0}'
    if what == 'close':
        return 'close'
    raise TabError(f'no command {what!r}: one of shot, passdump, skip, unskip, close')


def command(ctx, what, key=''):
    record = current(ctx)
    if record is None:
        raise TabError(t('no diagnostic run: start the game in diagnostic mode', '没有诊断局：请用诊断模式启动游戏'))
    if not game_state.game_running(ctx.data_dir):
        raise TabError(t('the game is not running', '游戏没有在运行'))
    shot = int(record.get('shots', 0)) + (1 if what == 'shot' else 0)
    line = line_for(what, record['run'], shot, key)
    with (ctx.data_dir / 'input.txt').open('a') as f:   # appended: the game reads on from where it was
        f.write(line + '\n')
    if what == 'shot':
        record['shots'] = shot
        record_path(ctx).write_text(json.dumps(record, ensure_ascii=False) + '\n')
    return {'line': line}


def state(ctx):
    record = current(ctx)
    running = game_state.game_running(ctx.data_dir)
    if record is None:
        return {'running': running, 'run': None, 'shots': [], 'log': None}
    run = Path(record['run'])
    log = game_log(ctx)
    kept = run / 'game.log'
    # the game log of this run, kept in its folder once the game closed (the next start moves the log aside)
    if not running and log.is_file() and log.stat().st_mtime >= record['started'] and \
            (not kept.is_file() or kept.stat().st_mtime < log.stat().st_mtime):
        shutil.copy2(log, kept)
    return {'running': running, 'run': str(run), 'shots': sorted(str(p) for p in run.glob('shot-*.png')),
            'log': str(kept if kept.is_file() else log), 'frames': (run / 'frames.tsv').is_file()}


def reveal(ctx, runner=subprocess.run):
    record = current(ctx)
    folder = Path(record['run']) if record else runs_dir(ctx)
    folder.mkdir(parents=True, exist_ok=True)
    runner(['/usr/bin/open', str(folder)], capture_output=True, timeout=OPEN_S)
    return {'opened': str(folder)}


ACTIONS = {'start': start, 'command': command, 'state': state, 'reveal': reveal}
