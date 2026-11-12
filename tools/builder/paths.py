"""The one path policy shared by the preflight checks (S1.2b) and the build command (S1.3): the paths a build uses,
with build_app.py's defaults for the work folder and the output app. The work folder's default is build_app.default_work
itself (the folder the app records, else MacNativeGames-work/<game> beside the folder of the app), one rule with
build_app.py and MNG Launcher (user 2026-11-12); until then this window put TOTK_build / BotW_build next to the game file."""
from dataclasses import replace
from pathlib import Path


def _abs(path):
    return Path(path).expanduser().resolve() if path is not None else None


def default_work(spec, out):
    """build_app.default_work for this game and app (read from build_app.py, not restated here)."""
    from tools.builder_jobs.repo import build_app
    return build_app().default_work(Path(out), spec.id)[0]


def effective_paths(spec, paths):
    """paths with every path expanded and resolved, and build_app.py's defaults for work and out."""
    game = _abs(paths.game)
    out = _abs(paths.out) if paths.out is not None else game.parent / f'{spec.app_name}.app'
    return replace(paths, game=game, keys=_abs(paths.keys), update=_abs(paths.update), firmware=_abs(paths.firmware),
                   shared_fonts=_abs(paths.shared_fonts),
                   work=_abs(paths.work) if paths.work is not None else default_work(spec, out), out=out)
