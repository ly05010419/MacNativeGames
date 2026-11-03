"""The one path policy shared by the preflight checks (S1.2b) and the build command (S1.3): the paths a build uses,
with build_app.py's defaults for the work folder and the output app. Moved unchanged from checks_inputs.py in S1.3."""
from dataclasses import replace
from pathlib import Path


def _abs(path):
    return Path(path).expanduser().resolve() if path is not None else None


def effective_paths(spec, paths):
    """paths with every path expanded and resolved, and build_app.py's defaults for work and out."""
    game = _abs(paths.game)
    return replace(paths, game=game, keys=_abs(paths.keys), update=_abs(paths.update), firmware=_abs(paths.firmware),
                   shared_fonts=_abs(paths.shared_fonts),
                   work=_abs(paths.work) if paths.work is not None else game.parent / spec.work_folder_name,
                   out=_abs(paths.out) if paths.out is not None else game.parent / f'{spec.app_name}.app')
