"""MNG Launcher's switches on a game's card: one word each in a file next to the game's controls.txt (its data folder),
"on", or no file for off. The game reads it when it starts, so a change counts from the next start; it may be changed
while the game runs. Only Ocarina of Time reads them for now (SWITCHES).

memory_saver -- "开启省内存模式" (user 2026-11-16: "改成 默认 常驻， 但是 launcher 里 增加一个 CheckBox 叫 开启省内存模式 我
  可以随时打开和关闭"), memory-saver.txt. Off (the default), the game keeps every pipeline list in memory after the
  warm-up, so nothing is fetched from the pipeline archive while it is played. On, it keeps the title list and every
  compute, mesh and tile-read pipeline only; the rest come from the archive when first drawn (a short hitch each time) and
  the memory it saves shrinks the further one plays (nvn_warm_resident.h warm_saver_word, nvn_mtl.m warm_resident_mode).
diagnostics -- "诊断模式" (user 2026-11-16: "诊断模式 也改成 CheckBox， 正常启动 可以带上 诊断模式"), diagnostics.txt. On, a
  full App starts with every diagnostic a quick App always has: the game log, long-frame detail, the per-pipeline log and
  the per-frame record in Logs/MacNativeGames/diagnostics (games/oot/app/launcher.m diagnostics_switch,
  start_diagnostics). Off (the default), the player's start carries none of them."""
import os
from pathlib import Path
import tempfile

SWITCHES = {'memory_saver': ('memory-saver.txt', ('oot',), 'memory saver mode'),
            'diagnostics': ('diagnostics.txt', ('oot',), 'diagnostic mode')}
FILE, GAMES = SWITCHES['memory_saver'][:2]   # the memory saver's, as before


def path(ctx, switch='memory_saver'):
    return Path(ctx.data_dir) / SWITCHES[switch][0]


def word(text):
    """The file's word as the game reads it: 'on', 'off' (also an empty file) or None for a word it does not know."""
    w = text.strip()
    return 'off' if w in ('', 'off') else 'on' if w == 'on' else None


def state(ctx, switch='memory_saver'):
    available = ctx.spec.id in SWITCHES[switch][1]
    on, unknown = False, None
    if available:
        try:
            text = path(ctx, switch).read_text(encoding='utf-8', errors='replace')
        except FileNotFoundError:
            text = ''
        w = word(text)
        on, unknown = w == 'on', (text.strip() if w is None else None)
    return {'available': available, 'on': on, 'file': str(path(ctx, switch)), 'unknown_word': unknown}


def choose(ctx, on, switch='memory_saver'):
    """Turn it on (write "on", whole or not at all) or off (remove the file); returns the new state."""
    name, games, what = SWITCHES[switch]
    if ctx.spec.id not in games:
        raise ValueError(f'{ctx.spec.display_name} has no {what}')
    if not isinstance(on, bool):
        raise ValueError('on must be true or false')
    target = path(ctx, switch)
    if on:
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix='.' + name + '.', dir=target.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write('on\n')
            os.replace(temp, target)
        except BaseException:
            Path(temp).unlink(missing_ok=True)
            raise
    else:
        target.unlink(missing_ok=True)
    return state(ctx, switch)
