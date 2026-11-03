"""Leaving the window safely (S3.4, B1; moved out of gui.py in S4.4 to keep it under 300 lines). A view's helpers.

Quit signals (SIGINT, SIGHUP when the Terminal window closes, SIGTERM) only set the view's exit_requested; Cmd-Q goes
through close(); settle() keeps following a running build after mainloop ends without close(), with no time limit."""
from contextlib import contextmanager
import signal
import sys

from tools.builder.strings import text

SETTLE_S = 30.0                # how often settle() says the build is still stopping; it never gives up


def _tell(err, line):
    try:
        print(line, file=err)
    except OSError:                     # B1: after SIGHUP the terminal is gone; the wait must go on
        pass


@contextmanager
def exit_signals(view):
    """B1: SIGINT, SIGHUP (the Terminal window closed) and SIGTERM only set view.exit_requested. Nothing is raised,
    so a signal that lands inside a Tk callback is not lost; the next pump acts on it."""
    def handler(signum, frame):
        view.exit_requested = True
    previous = {s: signal.signal(s, handler) for s in (signal.SIGINT, signal.SIGHUP, signal.SIGTERM)}
    try:
        yield
    finally:
        for s, handler in previous.items():
            signal.signal(s, handler)


def wire_quit(root, view):
    """B1: Cmd-Q and the app menu's Quit go through close() (which asks while busy) instead of ending Tk at once."""
    root.createcommand('::tk::mac::Quit', view.close)


def settle(view, update, monotonic, sleep, every_s=SETTLE_S, err=sys.stderr):
    """The safety net after mainloop ends without close() (an exception out of it): cancel a running build and keep
    the pump going until the controller is idle, then close. No time limit (B1): every every_s it says the build is
    still stopping; a KeyboardInterrupt here does not stop the wait."""
    if view.closed:
        return
    if not view.controller.idle:
        view.controller.cancel()
    start = monotonic()
    warn_at = start + every_s
    while not view.closed and not view.controller.idle:
        try:
            update()
            sleep(0.05)
        except KeyboardInterrupt:
            _tell(err, text('gui.still_stopping'))
        if monotonic() >= warn_at:
            _tell(err, text('gui.not_stopped', seconds=int(monotonic() - start)))
            warn_at += every_s
    if not view.closed:
        view.close()
