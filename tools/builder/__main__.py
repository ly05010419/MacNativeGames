"""python3.14 -m tools.builder: the builder's entry point (S2.1, T4; S2.1b, H1-H2; S2.2, W4).

Without --cli it opens the window; when this Python has no Tk it says so and uses the text mode, which needs --game.
Exit codes of the text mode: 0 success or ready, 1 the build did not produce an app, 2 checks blocked, paths or --game
missing, 3 the start failed, 130 cancelled or interrupted before a build."""
import argparse
from pathlib import Path
import signal
import sys
import time

from tools.builder.app import build_controller
from tools.builder.cli import CliView
from tools.builder.config import PATH_FIELDS
from tools.builder.games import GAMES
from tools.builder.strings import text

REPO_ROOT = Path(__file__).resolve().parents[2]


def path_arg(field):
    """The argument name of a path field: 'game' is the game id, so the game file is game_file (--game-file)."""
    return 'game_file' if field == 'game' else field


def build_parser():
    ap = argparse.ArgumentParser(prog='python3.14 -m tools.builder',
                                 description='Check this Mac and build a native app of one game.')
    ap.add_argument('--cli', action='store_true', help='use the text mode')
    ap.add_argument('--game', choices=sorted(GAMES), help='required with --cli; preselected in the window')
    for field in PATH_FIELDS:                   # --game is the game id; the game file is --game-file
        ap.add_argument('--' + path_arg(field).replace('_', '-'), dest=path_arg(field), default=None, metavar='PATH')
    ap.add_argument('--check-only', action='store_true', help='stop after the checks')
    ap.add_argument('--dry-run', action='store_true', help='print the command that would run, and stop')
    ap.add_argument('--yes', action='store_true', help='start without asking')
    return ap


def parse_args(argv, parser=None):
    args = (parser or build_parser()).parse_args(argv)
    args.paths = {f: getattr(args, path_arg(f)) for f in PATH_FIELDS if getattr(args, path_arg(f)) is not None}
    return args


class SigintFlag:
    """H1: while installed, a SIGINT only sets a flag. Called (it is the CLI's interrupted()), it raises
    KeyboardInterrupt once when the flag is set, so a Ctrl-C acts only at the head of a loop turn. SIGHUP (the
    terminal closed) and SIGTERM set the same flag (S3.4, B1): the build is cancelled and waited for, never left."""
    SIGNALS = (signal.SIGINT, signal.SIGHUP, signal.SIGTERM)

    def __init__(self):
        self._set, self._previous = False, {}

    def install(self):
        self._previous = {s: signal.signal(s, self._handler) for s in self.SIGNALS}

    def restore(self):
        for s, previous in self._previous.items():
            signal.signal(s, previous)

    def _handler(self, signum, frame):
        self._set = True

    def __call__(self):
        if self._set:
            self._set = False
            raise KeyboardInterrupt


def ask(prompt):
    """input() with Python's own Ctrl-C (a KeyboardInterrupt at the prompt), and an end of input read as 'no'."""
    previous = signal.signal(signal.SIGINT, signal.default_int_handler)
    try:
        return input(prompt)
    except EOFError:
        return ''
    finally:
        signal.signal(signal.SIGINT, previous)


def say(line):
    """print(), except that a closed terminal (SIGHUP, B1) does not stop the cancel and the wait that follow."""
    try:
        print(line, flush=True)
    except OSError:
        pass


def main(argv=None, home=None, repo_root=REPO_ROOT):
    parser = build_parser()
    args = parse_args(sys.argv[1:] if argv is None else argv, parser)
    home = Path.home() if home is None else home
    out = say
    if not args.cli:
        try:
            import tkinter                      # noqa: F401  only to know whether the window can open (W4)
        except ImportError:
            out(text('gui.no_tk'))
        else:
            from tools.builder.gui import open_window
            return open_window(build_controller(home, repo_root), args.game, args.paths)
    if args.game is None:
        parser.error('--game is required for the text mode')
    flag = SigintFlag()                         # the text mode's Ctrl-C only (S2.1b g7: the window has none)
    flag.install()
    try:
        controller = build_controller(home, repo_root)
        return CliView(controller, out, ask, time.sleep, flag).run(args)
    finally:
        flag.restore()


if __name__ == '__main__':
    sys.exit(main())
