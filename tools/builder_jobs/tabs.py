"""MNG Launcher's tabs besides building: the old window's Settings, Mods, Controls, Storage and Saves (user 2026-11-12:
move them into MNG Launcher, then retire the Tk window). One command, one JSON answer; the rules stay in tools/builder's
models, which these call unchanged.

  python -m tools.builder_jobs tab <tab> <action> --app <the game's app> [--args '<json object>']

<tab> is the module tools/builder_jobs/tab_<tab>.py, which holds ACTIONS {action: fn(ctx, **args)}. The answer is
{"ok": true, "result": …} or {"ok": false, "error": "<words>"}; the exit code is 0 either way, 2 for a bad call.
Texts are Chinese when MNG_LANG starts with zh (text.t), as the build jobs'. A game has the pages
tools.builder.games PAGES gives it; another tab is refused."""
from dataclasses import dataclass, fields, is_dataclass
import enum
import importlib
import json
import os
from pathlib import Path
import plistlib
import sys

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

TABS = ('settings', 'mods', 'controls', 'storage', 'saves')


@dataclass(frozen=True)
class Context:
    spec: object          # tools.builder.games.GameSpec
    app: Path             # the game's app
    home: Path
    repo: Path            # this checkout
    data_dir: Path        # ~/Library/Application Support/MacNativeGames/<TOTK|BotW>, which the game reads
    builder_dir: Path     # ~/Library/Application Support/MacNativeGames/Builder, the Launcher's own files (start options, the Mods page's lock, icons venv)


class TabError(Exception):
    """A refusal in words, for the window to show."""


def context(app, home=None):
    """The Context of an app of a game the tabs serve (tools.builder.games PAGE_GAMES: Tears of the Kingdom, Breath of the
    Wild, Ocarina of Time)."""
    from tools.builder.games import PAGE_GAMES
    app = Path(app).expanduser()
    home = Path(home or Path.home())
    try:
        bundle = plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier')
    except Exception:   # unreadable or not a property list (plistlib raises expat's errors too)
        raise TabError(t(f'{app} is not an app', f'{app} 不是一个 App')) from None
    spec = next((s for s in PAGE_GAMES.values() if bundle == s.bundle_id), None)
    if spec is None:
        raise TabError(t('these pages serve Tears of the Kingdom, Breath of the Wild and Ocarina of Time for now',
                         '这几页目前只支持王国之泪、旷野之息和时之笛'))
    support = home / 'Library/Application Support/MacNativeGames'
    return Context(spec, app, home, repo.REPO, support / spec.data_folder, support / 'Builder')


def jsonable(value):
    """Dataclasses, paths, enums, tuples and sets as JSON values."""
    if is_dataclass(value) and not isinstance(value, type):
        return {f.name: jsonable(getattr(value, f.name)) for f in fields(value)}
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, enum.Enum):
        return value.value if isinstance(value.value, (str, int, float)) else value.name
    if isinstance(value, dict):
        return {str(k): jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set, frozenset)):
        return [jsonable(v) for v in value]
    return value


def call(tab, action, app, args=None, home=None):
    """The answer of one call, as a dict (never raises for a refusal or a model's error: it is the answer)."""
    if tab not in TABS:
        return {'ok': False, 'error': f'no tab {tab!r}: one of {", ".join(TABS)}'}
    try:
        module = importlib.import_module(f'tools.builder_jobs.tab_{tab}')
    except ModuleNotFoundError as e:
        if e.name != f'tools.builder_jobs.tab_{tab}':
            raise
        return {'ok': False, 'error': t('this page is being moved here from the old window', '这一页正在从旧窗口搬过来')}
    fn = module.ACTIONS.get(action)
    if fn is None:
        return {'ok': False, 'error': f'no action {action!r} in {tab}: one of {", ".join(module.ACTIONS)}'}
    from tools.builder.games import PAGES
    try:
        ctx = context(app, home)
        if tab not in PAGES[ctx.spec.id]:
            raise TabError(t(f'{ctx.spec.display_name} has no {tab} page yet', f'{ctx.spec.display_name} 还没有这一页（{tab}）'))
        return {'ok': True, 'result': jsonable(fn(ctx, **(args or {})))}
    except (TabError, ValueError, OSError, RuntimeError) as e:
        return {'ok': False, 'error': str(e)}


def main(tab, action, app, args_json):
    try:
        args = json.loads(args_json) if args_json else {}
    except ValueError as e:
        print(json.dumps({'ok': False, 'error': f'--args is not JSON: {e}'}))
        return 2
    print(json.dumps(call(tab, action, app, args, os.environ.get('MNG_HOME')), ensure_ascii=False))
    return 0
