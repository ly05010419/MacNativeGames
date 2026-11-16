"""MNG Launcher's switches on a game's card: one word each in a file next to the game's controls.txt (its data folder),
"on" or "off"; no file (or an empty one) is the switch's default. The game reads it when it starts, so a change counts
from the next start; it may be changed while the game runs. Only Ocarina of Time reads them for now (SWITCHES).

memory_saver -- "开启省内存模式" (user 2026-11-16: "launcher 里 增加一个 CheckBox 叫 开启省内存模式 我可以随时打开和关闭";
  "默认 不用 省内存模式"), memory-saver.txt, off by default. Off, the game keeps every pipeline list in memory after the
  warm-up. On ("on" in the file), it keeps the title list and every compute, mesh and tile-read pipeline; the rest come
  from the archive when first drawn (a short hitch each time). On a 16 GB Mac keeping every list swapped 3.7 GB out in
  90 s of play (276 long frames against 35 with the saver; nvn_warm_resident.h warm_saver_word, nvn_mtl.m
  warm_resident_mode).
diagnostics -- "诊断模式" (user 2026-11-16: "诊断模式 也改成 CheckBox， 正常启动 可以带上 诊断模式"), diagnostics.txt, off by
  default. On, a full App starts with every diagnostic a quick App always has: the game log, long-frame detail, the
  per-pipeline log and the per-frame record in Logs/MacNativeGames/diagnostics (games/oot/app/launcher.m
  diagnostics_switch, start_diagnostics). Off, the player's start carries none of them."""
import os
from pathlib import Path
import tempfile

# switch: (file, games that read it, what it is called, default)
SWITCHES = {'memory_saver': ('memory-saver.txt', ('oot',), 'memory saver mode', False),
            'diagnostics': ('diagnostics.txt', ('oot',), 'diagnostic mode', False)}
FILE, GAMES = SWITCHES['memory_saver'][:2]   # the memory saver's, as before


def path(ctx, switch='memory_saver'):
    return Path(ctx.data_dir) / SWITCHES[switch][0]


def word(text):
    """The file's word as the game reads it: 'on', 'off', '' (no word: the switch's default) or None for a word it does not
    know (also the default)."""
    w = text.strip()
    return w if w in ('', 'on', 'off') else None


def state(ctx, switch='memory_saver'):
    _, games, _, default = SWITCHES[switch]
    available = ctx.spec.id in games
    on, unknown = default, None
    if available:
        try:
            text = path(ctx, switch).read_text(encoding='utf-8', errors='replace')
        except FileNotFoundError:
            text = ''
        w = word(text)
        on = default if w in ('', None) else w == 'on'
        unknown = text.strip() if w is None else None
    return {'available': available, 'on': on, 'default': default, 'file': str(path(ctx, switch)), 'unknown_word': unknown}


def choose(ctx, on, switch='memory_saver'):
    """Set it: the switch's default removes the file, the other value is written ("on" / "off", whole or not at all).
    Returns the new state."""
    name, games, what, default = SWITCHES[switch]
    if ctx.spec.id not in games:
        raise ValueError(f'{ctx.spec.display_name} has no {what}')
    if not isinstance(on, bool):
        raise ValueError('on must be true or false')
    target = path(ctx, switch)
    if on == default:
        target.unlink(missing_ok=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix='.' + name + '.', dir=target.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write('on\n' if on else 'off\n')
            os.replace(temp, target)
        except BaseException:
            Path(temp).unlink(missing_ok=True)
            raise
    return state(ctx, switch)
