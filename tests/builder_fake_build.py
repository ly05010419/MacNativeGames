"""A configurable fake build for tests/test_builder_process*.py, run with sys.executable; plus the helpers those
tests share to start it, wait for it and clean it up."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

SCRIPT = Path(__file__).resolve()
REAL_KILLPG = os.killpg


def fake_argv(*args):
    return [sys.executable, str(SCRIPT), *args]


def wait_until(predicate, timeout_s=10.0):
    """Real-time polling with a deadline; returns whether the predicate became true."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return False


def stop(build):
    """Test cleanup: SIGKILL the group with the real killpg, then reap the leader through the BuildProcess itself
    (its poll() calls Popen.poll(), so Popen knows the process is gone), then close the reader."""
    try:
        if build.pid is not None:
            try:
                REAL_KILLPG(build.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):   # gone, or a zombie leader (EPERM on macOS, S1.8c)
                pass
            wait_until(lambda: build.poll() is not None and build.exit_code is not None)
    finally:
        build.close()


def start_child(args):
    """A child in the same group that sets its SIGINT handling and only then writes its own ready file; the parent
    waits for that file, so the parent's ready file means the child's handler is in place. With --child-writes,
    the child writes those hex bytes in two halves after --child-delay and exits; otherwise it sleeps 30 s."""
    handler = 'signal.SIG_IGN' if args.child == 'ignore' else 'signal.SIG_DFL'
    child_ready = Path(f'{args.ready or SCRIPT}.child-{os.getpid()}')
    if args.child_writes:
        data = bytes.fromhex(args.child_writes)
        half = len(data) // 2
        work = (f'time.sleep({args.child_delay}); out = sys.stdout.buffer; out.write({data[:half]!r}); out.flush(); '
                f'time.sleep(0.05); out.write({data[half:]!r}); out.flush()')
    else:
        work = 'time.sleep(30)'
    code = (f'import pathlib, signal, sys, time; signal.signal(signal.SIGINT, {handler}); '
            f'pathlib.Path({str(child_ready)!r}).write_text("ready"); {work}')
    subprocess.Popen([sys.executable, '-c', code])
    if not wait_until(child_ready.exists):
        sys.exit('the child never got ready')
    child_ready.unlink()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--lines', type=int, default=0)
    ap.add_argument('--every', type=float, default=0.0)          # seconds between lines
    ap.add_argument('--stderr', default=None)                   # one line printed on stderr after the others
    ap.add_argument('--partial', default=None)                  # a final line without '\n'
    ap.add_argument('--long', type=int, default=0)              # characters of 'x' without '\n'
    ap.add_argument('--raw', default=None)                      # hex bytes written as they are
    ap.add_argument('--child', choices=('none', 'plain', 'ignore'), default='none')
    ap.add_argument('--child-writes', default=None)             # hex bytes the child writes after the leader is done
    ap.add_argument('--child-delay', type=float, default=0.5)
    ap.add_argument('--ignore-sigint', action='store_true')
    ap.add_argument('--print-env', action='append', default=[])
    ap.add_argument('--ready', default=None)
    ap.add_argument('--sleep', type=float, default=0.0)         # after the output, before exiting
    ap.add_argument('--exit', type=int, default=0)
    args = ap.parse_args(argv)
    signal.signal(signal.SIGINT, signal.SIG_IGN if args.ignore_sigint else lambda *_: sys.exit(130))
    if args.child != 'none':
        start_child(args)
    if args.ready:
        Path(args.ready).write_text('ready')
    out = sys.stdout.buffer
    for i in range(args.lines):
        out.write(f'line {i}\n'.encode())
        out.flush()
        time.sleep(args.every)
    for name in args.print_env:
        out.write(f'{name}={os.environ.get(name)}\n'.encode())
    if args.stderr:
        sys.stderr.buffer.write(args.stderr.encode() + b'\n')
        sys.stderr.flush()
    if args.raw:
        out.write(bytes.fromhex(args.raw))
    if args.long:
        out.write(b'x' * args.long)
    if args.partial:
        out.write(args.partial.encode())
    out.flush()
    time.sleep(args.sleep)
    return args.exit


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
