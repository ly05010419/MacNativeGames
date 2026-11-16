"""MNG Launcher's "开启省内存模式" (memory saver mode) switch on a game's card (user 2026-11-16: "改成 默认 常驻， 但是 launcher
里 增加一个 CheckBox 叫 开启省内存模式 我可以随时打开和关闭").

Off (the default), the game keeps every pipeline list in memory after the warm-up, so nothing is fetched from the pipeline
archive while it is played. On, it keeps the title list and every compute, mesh and tile-read pipeline only; the rest
come from the archive when first drawn (a short hitch each time) and the memory it saves shrinks the further one plays.

The switch is one word in memory-saver.txt next to the game's controls.txt (its data folder): "on", or no file for off.
The game reads it when it starts (nvn_warm_resident.h warm_saver_word, nvn_mtl.m warm_resident_mode), so a change counts
from the next start; it may be changed while the game runs. Only Ocarina of Time reads it for now (GAMES)."""
import os
from pathlib import Path
import tempfile

FILE = 'memory-saver.txt'
GAMES = ('oot',)


def path(ctx):
    return Path(ctx.data_dir) / FILE


def word(text):
    """The file's word as the game reads it: 'on', 'off' (also an empty file) or None for a word it does not know."""
    w = text.strip()
    return 'off' if w in ('', 'off') else 'on' if w == 'on' else None


def state(ctx):
    available = ctx.spec.id in GAMES
    on, unknown = False, None
    if available:
        try:
            text = path(ctx).read_text(encoding='utf-8', errors='replace')
        except FileNotFoundError:
            text = ''
        w = word(text)
        on, unknown = w == 'on', (text.strip() if w is None else None)
    return {'available': available, 'on': on, 'file': str(path(ctx)), 'unknown_word': unknown}


def choose(ctx, on):
    """Turn it on (write "on", whole or not at all) or off (remove the file); returns the new state."""
    if ctx.spec.id not in GAMES:
        raise ValueError(f'{ctx.spec.display_name} has no memory saver mode')
    if not isinstance(on, bool):
        raise ValueError('on must be true or false')
    target = path(ctx)
    if on:
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix='.' + FILE + '.', dir=target.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write('on\n')
            os.replace(temp, target)
        except BaseException:
            Path(temp).unlink(missing_ok=True)
            raise
    else:
        target.unlink(missing_ok=True)
    return state(ctx)
